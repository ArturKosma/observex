"""Trains observex's detector for one category (e.g. characters) on the user's review answers.

    tools/train/.venv/Scripts/python tools/train/train.py [--category character] [--model medium] [--resolution 1024]

1. Collects every recording with a <category>-answers.txt in the recordings folder (observex.cfg, or "recordings"
   next to observex.exe). Only reviewed images count; on them, every "yes" mark is the thing and everything else
   is background.
2. Keeps the last part of each recording aside (--held-out, a fifth by default) and never trains on it: it gives the
   score. The last part, not random images, because neighbouring images are near copies and would flatter it.
3. Fine-tunes RF-DETR (pretrained on everyday photos) on the rest, on the GPU.
4. Saves the model as models/<category>/model.pth, its settings and score in model.json, and prints the score:
   how many of the held-out things it finds, and how many false alarms it raises, at LIVE_RESOLUTION.
5. Writes model.onnx, the form observex runs live (see export.py).
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import time
from datetime import datetime
from pathlib import Path

from marks import Mark, model_folder, overlap, read_marks, recordings_folder

MODELS = {"nano": "RFDETRNano", "small": "RFDETRSmall", "medium": "RFDETRMedium", "large": "RFDETRLarge"}
SCORE_CUTOFFS = (0.3, 0.4, 0.5)  # The score counts marks the model is at least this sure about, for each of these.
# observex runs the model on images squeezed to this many pixels square: smaller than it trains at (--resolution),
# so it costs about a quarter less live. Tested 2026-11-01 on 80 held-out images (238 marked): trained at 1024 and
# run at 768, it found 214 at 40% sure with 9 false alarms (run at 1024: 219 and 14; AP50 0.957 against 0.960);
# trained at 768 it did worse (AP50 0.931, 17 false alarms). The score, guess.py and model.onnx all use it.
LIVE_RESOLUTION = 768
# Images per learning step, RF-DETR's usual recipe. They go through the GPU --batch at a time, and the step waits for
# all of them. Not batch_size="auto": it tries bigger batches until one does not fit, but on Windows the NVIDIA driver
# then moves GPU memory into normal RAM instead of failing, so the search never ends (it ran 7 minutes, 15 GB RAM).
IMAGES_PER_STEP = 16
# Random changes to each training image, so the model learns characters, not one recording's look. "flip" is
# RF-DETR's own default (mirror only). "color" also shifts brightness, contrast, saturation and hue a little: the same
# character looks different in a dark cave, in fog or in daylight. Needs the rfdetr[augment] extra.
AUGMENTATIONS = {
    "flip": None,
    "color": {
        "HorizontalFlip": {"p": 0.5},
        "ColorJitter": {"brightness": 0.25, "contrast": 0.25, "saturation": 0.25, "hue": 0.05, "p": 0.6},
    },
}


def utf8_output() -> None:
    """RF-DETR prints tables with box-drawing characters, which crash Python's default Windows codepage output."""
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding="utf-8", errors="replace")


def new_model(size: str, resolution: int):
    """An RF-DETR of `size` pretrained on everyday photos, ready to fine-tune."""
    import rfdetr

    return getattr(rfdetr, MODELS[size])(resolution=resolution)


def load_trained(path: Path):
    """A model saved by train.py. The file itself records its size, resolution and categories."""
    from rfdetr import RFDETR

    return RFDETR.from_checkpoint(str(path))


def load_live(path: Path):
    """A model saved by train.py, set to see images at LIVE_RESOLUTION, as observex runs it: predict, inference
    (half precision, compiled) and export then all use that size."""
    model = load_trained(path)
    model.model.resolution = LIVE_RESOLUTION
    return model


def link_or_copy(source: Path, target: Path) -> None:
    """A hard link costs no disk space; a copy is the fallback (e.g. across drives)."""
    try:
        os.link(source, target)
    except OSError:
        shutil.copy2(source, target)


def image_size(path: Path) -> tuple[int, int]:
    from PIL import Image

    with Image.open(path) as image:
        return image.size


def write_split(folder: Path, items: list[tuple[Path, str, list[Mark]]], category: str) -> int:
    """Writes one split in COCO format (the layout RF-DETR reads): images plus _annotations.coco.json.
    Returns how many things are marked in it."""
    folder.mkdir(parents=True)
    images, annotations = [], []
    for image_id, (source, name, marks) in enumerate(items, start=1):
        link_or_copy(source, folder / name)
        width, height = image_size(source)
        images.append({"id": image_id, "file_name": name, "width": width, "height": height})
        for mark in marks:
            annotations.append({
                "id": len(annotations) + 1,
                "image_id": image_id,
                "category_id": 0,
                "bbox": [mark.x, mark.y, mark.width, mark.height],
                "area": mark.width * mark.height,
                "iscrowd": 0,
            })
    coco = {
        "images": images,
        "annotations": annotations,
        "categories": [{"id": 0, "name": category, "supercategory": "none"}],
    }
    (folder / "_annotations.coco.json").write_text(json.dumps(coco), encoding="utf-8")
    return len(annotations)


def build_dataset(recordings: list[Path], category: str, held_out_share: float, dataset: Path) -> dict:
    """Splits the reviewed images of `recordings` into train and valid (held out), and writes them to `dataset`."""
    if dataset.exists():
        shutil.rmtree(dataset)
    train, valid = [], []
    for recording in recordings:
        answers = read_marks(recording / f"{category}-answers.txt")
        names = sorted(name for name in answers if (recording / name).exists())
        held_out = max(1, round(len(names) * held_out_share)) if len(names) >= 5 else 0
        for index, name in enumerate(names):
            things = [mark for mark in answers[name] if mark.answer == "yes"]
            item = (recording / name, f"{recording.name}_{name}", things)
            (valid if index >= len(names) - held_out else train).append(item)
    if not train or not valid:
        raise SystemExit(f"Not enough reviewed images: {len(train)} to train on, {len(valid)} to hold out. "
                         "Review at least 5 images first.")
    counts = {
        "train_images": len(train),
        "train_things": write_split(dataset / "train", train, category),
        "held_out_images": len(valid),
        "held_out_things": write_split(dataset / "valid", valid, category),
    }
    return counts


def score(model, dataset: Path, cutoff: float) -> dict:
    """How the model (from load_live) does on the held-out images, counting marks at least `cutoff` sure: things
    found, things missed, false alarms."""
    coco = json.loads((dataset / "valid" / "_annotations.coco.json").read_text(encoding="utf-8"))
    truth: dict[int, list[Mark]] = {image["id"]: [] for image in coco["images"]}
    for annotation in coco["annotations"]:
        x, y, width, height = annotation["bbox"]
        truth[annotation["image_id"]].append(Mark(x, y, width, height))
    found = missed = false_alarms = 0
    for image in coco["images"]:
        detections = model.predict(str(dataset / "valid" / image["file_name"]), threshold=cutoff,
                                   include_source_image=False)
        order = sorted(range(len(detections.xyxy)), key=lambda i: -float(detections.confidence[i]))
        unmatched = list(truth[image["id"]])
        for i in order:
            left, top, right, bottom = (float(v) for v in detections.xyxy[i])
            guess = Mark(round(left), round(top), max(1, round(right - left)), max(1, round(bottom - top)))
            best = max(unmatched, key=lambda thing: overlap(guess, thing), default=None)
            if best is not None and overlap(guess, best) >= 0.5:
                unmatched.remove(best)
                found += 1
            else:
                false_alarms += 1
        missed += len(unmatched)
    total = found + missed
    return {
        "cutoff": cutoff,
        "found": found,
        "missed": missed,
        "false_alarms": false_alarms,
        "found_share": found / total if total else 0.0,
        "false_alarms_per_image": false_alarms / max(1, len(coco["images"])),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--category", default="character")
    parser.add_argument("--recordings", type=Path, default=None, help="default: the folder in observex.cfg")
    parser.add_argument("--model", choices=sorted(MODELS), default="medium")
    parser.add_argument("--resolution", type=int, default=1024,
                        help="while training, the image is squeezed to this many pixels square (multiple of 32); "
                             "observex runs the model at LIVE_RESOLUTION")
    parser.add_argument("--epochs", type=int, default=40,
                        help="passes over the images; on 2026-11-01 the best pass was always before 35")
    parser.add_argument("--augment", choices=sorted(AUGMENTATIONS), default="color",
                        help="random changes to training images (see AUGMENTATIONS)")
    parser.add_argument("--batch", type=int, choices=[1, 2, 4, 8, 16], default=4,
                        help="images through the GPU at once; lower it if training is slow or runs out of memory")
    parser.add_argument("--held-out", type=float, default=0.2, help="share of each recording kept aside for the score")
    args = parser.parse_args()
    if args.resolution % 32 != 0:
        parser.error("--resolution must be a multiple of 32")
    utf8_output()

    recordings_root = args.recordings or recordings_folder()
    recordings = sorted(p.parent for p in recordings_root.glob(f"*/{args.category}-answers.txt"))
    if not recordings:
        raise SystemExit(f"No {args.category}-answers.txt in any recording in {recordings_root}. Review some first.")
    out = model_folder(args.category)
    out.mkdir(parents=True, exist_ok=True)
    dataset = out / "dataset"
    counts = build_dataset(recordings, args.category, args.held_out, dataset)
    print(f"Training on {counts['train_images']} images ({counts['train_things']} marked), holding out "
          f"{counts['held_out_images']} ({counts['held_out_things']} marked), from {len(recordings)} recording(s).",
          flush=True)

    started = time.time()
    training = out / "training"
    if training.exists():
        shutil.rmtree(training)
    model = new_model(args.model, args.resolution)
    model.train(dataset_dir=str(dataset), output_dir=str(training), epochs=args.epochs, batch_size=args.batch,
                grad_accum_steps=IMAGES_PER_STEP // args.batch, checkpoint_interval=args.epochs, tensorboard=False,
                seed=0, progress_bar="tqdm", aug_config=AUGMENTATIONS[args.augment])
    minutes = (time.time() - started) / 60.0

    best = training / "checkpoint_best_total.pth"
    shutil.copy2(best, out / "model.pth")
    for resume_file in training.glob("*.ckpt"):  # 0.5 GB each, only for resuming a stopped run.
        resume_file.unlink()
    trained = load_live(out / "model.pth")
    scores = [score(trained, dataset, cutoff) for cutoff in SCORE_CUTOFFS]
    info = {
        "category": args.category,
        "model": args.model,
        "resolution": args.resolution,
        "live_resolution": LIVE_RESOLUTION,
        "trained": datetime.now().isoformat(timespec="seconds"),
        "minutes": round(minutes, 1),
        "epochs": args.epochs,
        "augment": args.augment,
        "recordings": [str(p) for p in recordings],
        **counts,
        "score": scores,
    }
    (out / "model.json").write_text(json.dumps(info, indent=2), encoding="utf-8")
    from export import export_onnx  # Here, not at the top: export.py imports this file.

    onnx = export_onnx(args.category)  # What observex runs live.
    print(f"\nTrained in {minutes:.0f} min. Saved {out / 'model.pth'} and {onnx.name}.")
    print(f"Held-out score, on {counts['held_out_images']} images it never learned from (at {LIVE_RESOLUTION} "
          "pixels, as observex runs it):")
    for result in scores:
        print(f"  marks at least {result['cutoff']:.0%} sure: finds {result['found']} of "
              f"{result['found'] + result['missed']} ({result['found_share']:.0%}), {result['false_alarms']} false "
              f"alarms ({result['false_alarms_per_image']:.1f} per image)")


if __name__ == "__main__":
    main()
