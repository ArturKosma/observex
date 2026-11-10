"""Trains observex's detector for one category (e.g. characters) on the user's review answers.

    tools/train/.venv/Scripts/python tools/train/train.py [--category character] [--kinds] [--model medium]
                                                           [--resolution 1024]

1. Collects every recording with a <category>-answers.txt in the recordings folder (observex.cfg, or "recordings"
   next to observex.exe). Only reviewed images count; on them, every "yes" mark is the thing and everything else
   is background. With --kinds each kind (monster, player, NPC, the user's own, "can't tell" and dead) is a class of
   its own, which observex then shows live, and only images whose kinds the user has checked in the review count.
2. Keeps the last part of each recording aside (--held-out, a fifth by default) and never trains on it: it gives the
   score. The last part, not random images, because neighbouring images are near copies and would flatter it.
3. Fine-tunes RF-DETR (pretrained on everyday photos) on the rest, on the GPU.
4. Saves the model as models/<category>/model.pth, its settings and score in model.json, and prints the score:
   how many of the held-out things it finds, and how many false alarms it raises (and how many of those are one box
   around several things), at LIVE_RESOLUTION.
5. Writes model.onnx, the form observex runs live (see export.py).
"""

from __future__ import annotations

import argparse
import json
import math
import os
import random
import shutil
import sys
import time
from datetime import datetime
from pathlib import Path

from marks import Mark, model_folder, overlap, read_marks, recordings_folder

MODELS = {"nano": "RFDETRNano", "small": "RFDETRSmall", "medium": "RFDETRMedium", "large": "RFDETRLarge"}
# The kinds --kinds learns, as the answers file names them, in the order of the review's keys (kCharacterKindIds in
# src/core/Marks.h). model.json lists them in this order; observex reads them from there. "unknown" (2026-11-08): a
# character the user cannot tell the kind of (far off, a few pixels), learnt as a kind of its own so the model says
# "can't tell" where the user would; observex shows it as "Character?". "dead" (2026-11-08): a corpse, answered no
# before that day (older recordings still hold such no's). New kinds go last: an older model then still fits the
# review (src/core/ModelInfo.h's kindsFit).
KINDS = {"character": ("monster", "player", "npc", "own", "unknown", "dead")}
SCORE_CUTOFFS = (0.3, 0.4, 0.5)  # The score counts marks the model is at least this sure about, for each of these.
# observex runs the model on images squeezed to this many pixels square. The score, guess.py and model.onnx all use
# it. From 2026-11-01 it was 768, a third faster than 1024 for nearly the same score then (80 held-out images, 238
# marked: AP50 0.957 against 0.960). With more and harder images 1024 is clearly better: the ninth model on 211
# held-out images (1103 marked) scores AP50 0.902 against 0.873 at 768, and at 50% sure finds 939 with 61 false
# alarms against 892 and 69. It costs 3.2 ms per image live instead of 2.0 (still all 240 images a second on an idle
# desktop). The user chose 1024 on 2026-11-03. Training at 768 instead was worse (AP50 0.931 on the 80, 2026-11-01).
LIVE_RESOLUTION = 1024
# Images per learning step, RF-DETR's usual recipe. They go through the GPU --batch at a time, and the step waits for
# all of them. Not batch_size="auto": it tries bigger batches until one does not fit, but on Windows the NVIDIA driver
# then moves GPU memory into normal RAM instead of failing, so the search never ends (it ran 7 minutes, 15 GB RAM).
IMAGES_PER_STEP = 16
# How fast it learns falls along a cosine curve, to nothing by the last pass, so the last passes fine-tune instead of
# jumping around. RF-DETR's default ("step") divides it by 10 after 100 passes, so in 40 it never fell: the sixth
# model (2026-11-02) stopped getting better from about pass 20, still learning at full speed.
LR_SCHEDULE = "cosine"
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
# With --kinds, images holding a rare kind are trained on more often (LVIS's repeat-factor sampling): each counts
# max(1, sqrt(REPEAT_RARE / share of training images with its rarest kind)) times. In 2026-11-03's 845 training images,
# 63 have a player (2.6 times) and 175 an NPC (1.6 times); monsters and the user's own are in most (once). Measured
# 2026-11-03 at 768: on the usual 211 held-out images (1103 marked) AP50 0.872 -> 0.873, false alarms at 30% 156 ->
# 135, the right kind 96.6% -> 97.1%; on a recording held out of training whole (110 images, 558 marked, mostly NPCs)
# AP50 0.897 -> 0.912, at 50% 400 -> 459 found (false alarms 36 -> 50).
REPEAT_RARE = 0.5
# With --kinds, images holding a crowd are also trained on CROWD_REPEAT times per pass (or as often as their rarest kind
# says, if more): the model drew one box around several characters standing close (2026-11-08: the twelfth drew 29 on
# the 431 held-out images at 50%, the eleventh 42; the user had split such a box on 251 of 2266 reviewed images). A
# crowd: two marked things at most CROWD_MAX_HEIGHT pixels tall (far off, where the model groups them) whose boxes touch
# once each is grown by CROWD_GAP of its width and height on every side: 35% of the 2266 images, and 201 of those 251.
CROWD_REPEAT = 2.0
CROWD_GAP = 0.25
CROWD_MAX_HEIGHT = 120


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


def write_split(folder: Path, items: list[tuple[Path, str, list[Mark]]], classes: list[str]) -> int:
    """Writes one split in COCO format (the layout RF-DETR reads): images plus _annotations.coco.json. `classes` is
    the category alone, or its kinds; each class's id is its place in the list, which is its slot in the model's
    scores. Returns how many things are marked in it."""
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
                "category_id": classes.index(mark.kind) if len(classes) > 1 else 0,
                "bbox": [mark.x, mark.y, mark.width, mark.height],
                "area": mark.width * mark.height,
                "iscrowd": 0,
            })
    coco = {
        "images": images,
        "annotations": annotations,
        "categories": [{"id": i, "name": name, "supercategory": "none"} for i, name in enumerate(classes)],
    }
    (folder / "_annotations.coco.json").write_text(json.dumps(coco), encoding="utf-8")
    return len(annotations)


def build_dataset(recordings: list[Path], category: str, held_out_share: float, dataset: Path,
                  kinds: tuple[str, ...] = ()) -> dict:
    """Splits the reviewed images of `recordings` into train and valid (held out), and writes them to `dataset`.
    With `kinds`, only images whose every yes has a checked kind among them."""
    if dataset.exists():
        shutil.rmtree(dataset)
    train, valid = [], []
    unchecked = to_answer = 0
    for recording in recordings:
        answers = read_marks(recording / f"{category}-answers.txt")
        names = sorted(name for name in answers if (recording / name).exists())
        # An image holding a guess added after its review (a thing the model was sure of that no answer covered,
        # 2026-11-09) waits for the user's answer, so it does not teach that the thing there is nothing.
        waiting = {name for name in names if any(mark.answer is None for mark in answers[name])}
        to_answer += len(waiting)
        names = [name for name in names if name not in waiting]
        if kinds:
            checked = [name for name in names if all(mark.kind_checked and mark.kind in kinds
                                                     for mark in answers[name] if mark.answer == "yes")]
            unchecked += len(names) - len(checked)
            names = checked
        held_out = max(1, round(len(names) * held_out_share)) if len(names) >= 5 else 0
        for index, name in enumerate(names):
            things = [mark for mark in answers[name] if mark.answer == "yes"]
            item = (recording / name, f"{recording.name}_{name}", things)
            (valid if index >= len(names) - held_out else train).append(item)
    if not train or not valid:
        raise SystemExit(f"Not enough reviewed images: {len(train)} to train on, {len(valid)} to hold out. "
                         + ("Check the kinds on more images in the review first." if kinds else
                            "Review at least 5 images first."))
    classes = list(kinds) if kinds else [category]
    counts = {
        "train_images": len(train),
        "train_things": write_split(dataset / "train", train, classes),
        "held_out_images": len(valid),
        "held_out_things": write_split(dataset / "valid", valid, classes),
        "images_to_answer": to_answer,  # Left out: a guess added since their review waits for its answer.
    }
    if kinds:
        counts["kinds_unchecked_images"] = unchecked  # Left out: their kinds are not checked yet.
        counts["train_kinds"] = {kind: sum(mark.kind == kind for _, _, marks in train for mark in marks)
                                 for kind in kinds}
    return counts


def keep_kind_scores(model, kinds: list[str]) -> dict:
    """For a model with kinds: makes each model.predict also keep every candidate's kind scores (0 to 1) and box in
    the returned dict, as observex's Detector reads them. RF-DETR's own predict lists a candidate once per kind it
    scores high, so the same thing can come twice."""
    if hasattr(model, "kept_kind_scores"):  # Already keeping them.
        return model.kept_kind_scores
    kept: dict = {}
    model.kept_kind_scores = kept
    original = model.model.postprocess

    def keep(predictions, target_sizes, *rest, **named):
        kept["scores"] = predictions["pred_logits"][0, :, :len(kinds)].float().sigmoid().cpu()
        kept["boxes"] = predictions["pred_boxes"][0].float().cpu()
        kept["size"] = [float(v) for v in target_sizes[0]]
        return original(predictions, target_sizes, *rest, **named)

    model.model.postprocess = keep
    return kept


def kinds_guesses(kept: dict, kinds: list[str], threshold: float) -> list[Mark]:
    """After a predict, with keep_kind_scores: each candidate at least `threshold` sure, as the Detector keeps it.
    How sure is that it is one of its two surest kinds (src/core/Detection.h's eitherKind); `kind` is the surest,
    with how sure of it, and the next."""
    height, width = kept["size"]
    top = kept["scores"].topk(2, dim=1)
    marks = []
    for (first, second), (kind, second_kind), (cx, cy, w, h) in zip(top.values.tolist(), top.indices.tolist(),
                                                                    kept["boxes"].tolist()):
        sure = 1.0 - (1.0 - first) * (1.0 - second)
        if sure < threshold:
            continue
        left = min(max((cx - w / 2) * width, 0.0), width)
        right = min(max((cx + w / 2) * width, 0.0), width)
        top_edge = min(max((cy - h / 2) * height, 0.0), height)
        bottom = min(max((cy + h / 2) * height, 0.0), height)
        marks.append(Mark(round(left), round(top_edge), max(1, round(right - left)), max(1, round(bottom - top_edge)),
                          guess=sure, kind=kinds[kind], kind_guess=first, second_kind=kinds[second_kind],
                          second_guess=second))
    return marks


def crowded(boxes: list[list[float]], gap: float = CROWD_GAP) -> bool:
    """Whether two of `boxes` ([x, y, width, height]) touch once each is grown by `gap` of its width and height on
    every side."""
    grown = [(x - gap * w, y - gap * h, x + w + gap * w, y + h + gap * h) for x, y, w, h in boxes]
    return any(a[0] < b[2] and b[0] < a[2] and a[1] < b[3] and b[1] < a[3]
               for i, a in enumerate(grown) for b in grown[i + 1:])


def repeat_rare(split: Path, threshold: float, crowd_times: float = 1.0, seed: int = 0) -> tuple[int, int]:
    """Repeats the images of `split` (written by write_split) that hold rare kinds, as REPEAT_RARE says, or a crowd
    (crowded), `crowd_times` times if that is more: extra copies are hard links with their marks copied, a fraction of
    a time rounded at random (seeded, so a rerun repeats the same images). Returns how many images it has after, and
    how many of those before hold a crowd."""
    path = split / "_annotations.coco.json"
    coco = json.loads(path.read_text(encoding="utf-8"))
    kinds_in: dict[int, set[int]] = {}
    marks_of: dict[int, list[dict]] = {}
    for annotation in coco["annotations"]:
        kinds_in.setdefault(annotation["image_id"], set()).add(annotation["category_id"])
        marks_of.setdefault(annotation["image_id"], []).append(annotation)
    count = len(coco["images"])
    share: dict[int, float] = {}
    for kinds in kinds_in.values():
        for kind in kinds:
            share[kind] = share.get(kind, 0.0) + 1.0 / count
    factor = {kind: max(1.0, math.sqrt(threshold / part)) for kind, part in share.items()}
    chance = random.Random(seed)
    images, annotations = list(coco["images"]), list(coco["annotations"])
    next_id = max((image["id"] for image in images), default=0) + 1
    crowds = 0
    for image in coco["images"]:
        times = max([factor[kind] for kind in kinds_in.get(image["id"], ())] or [1.0])
        if crowded([a["bbox"] for a in marks_of.get(image["id"], []) if a["bbox"][3] <= CROWD_MAX_HEIGHT]):
            crowds += 1
            times = max(times, crowd_times)
        extra = int(times) - 1 + (chance.random() < times - int(times))
        stem, _, suffix = image["file_name"].rpartition(".")
        for n in range(1, extra + 1):
            name = f"{stem}__again{n}.{suffix}"
            link_or_copy(split / image["file_name"], split / name)
            images.append({**image, "id": next_id, "file_name": name})
            for annotation in marks_of.get(image["id"], []):
                annotations.append({**annotation, "id": len(annotations) + 1, "image_id": next_id})
            next_id += 1
    coco["images"], coco["annotations"] = images, annotations
    path.write_text(json.dumps(coco), encoding="utf-8")
    return len(images), crowds


def share_inside(a: Mark, b: Mark) -> float:
    """How much of box `a` lies inside box `b`: 0 = none, 1 = all of it."""
    width = min(a.x + a.width, b.x + b.width) - max(a.x, b.x)
    height = min(a.y + a.height, b.y + b.height) - max(a.y, b.y)
    return 0.0 if width <= 0 or height <= 0 else width * height / max(a.width * a.height, 1e-6)


def score(model, dataset: Path, cutoff: float, kinds: list[str] | tuple[str, ...] = ()) -> dict:
    """How the model (from load_live) does on the held-out images, counting marks at least `cutoff` sure: things
    found, things missed, false alarms, and of those the group boxes: one box around 2+ marked things (each
    60%+ inside it), as dangerous live as a miss. With `kinds`, read as observex reads it (kinds_guesses)."""
    kept = keep_kind_scores(model, list(kinds)) if kinds else None
    coco = json.loads((dataset / "valid" / "_annotations.coco.json").read_text(encoding="utf-8"))
    truth: dict[int, list[Mark]] = {image["id"]: [] for image in coco["images"]}
    for annotation in coco["annotations"]:
        x, y, width, height = annotation["bbox"]
        truth[annotation["image_id"]].append(Mark(x, y, width, height, kind=str(annotation["category_id"])))
    found = missed = false_alarms = group_boxes = right_kind = 0
    for image in coco["images"]:
        detections = model.predict(str(dataset / "valid" / image["file_name"]), threshold=cutoff,
                                   include_source_image=False)
        if kept is not None:
            guesses = [(g, str(list(kinds).index(g.kind))) for g in kinds_guesses(kept, list(kinds), cutoff)]
        else:
            guesses = []
            for (left, top, right, bottom), sure, kind in zip(detections.xyxy, detections.confidence,
                                                              detections.class_id):
                guesses.append((Mark(round(float(left)), round(float(top)), max(1, round(float(right - left))),
                                     max(1, round(float(bottom - top))), guess=float(sure)), str(int(kind))))
        unmatched = list(truth[image["id"]])
        for guess, kind in sorted(guesses, key=lambda pair: -pair[0].guess):
            best = max(unmatched, key=lambda thing: overlap(guess, thing), default=None)
            if best is not None and overlap(guess, best) >= 0.5:
                unmatched.remove(best)
                found += 1
                right_kind += kind == best.kind
            else:
                false_alarms += 1
                group_boxes += sum(share_inside(thing, guess) >= 0.6 for thing in truth[image["id"]]) >= 2
        missed += len(unmatched)
    total = found + missed
    return {
        "cutoff": cutoff,
        "found": found,
        "missed": missed,
        "false_alarms": false_alarms,
        "group_boxes": group_boxes,
        "found_share": found / total if total else 0.0,
        "false_alarms_per_image": false_alarms / max(1, len(coco["images"])),
        "right_kind_share": right_kind / found if found else 0.0,  # Of those found; 1 for a model without kinds.
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--category", default="character")
    parser.add_argument("--kinds", action="store_true",
                        help="learn the category's kinds (KINDS) as classes; only images with checked kinds count")
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
    if args.kinds and args.category not in KINDS:
        parser.error(f"--kinds: {args.category} has no kinds")
    kinds = KINDS[args.category] if args.kinds else ()
    utf8_output()

    recordings_root = args.recordings or recordings_folder()
    recordings = sorted(p.parent for p in recordings_root.glob(f"*/{args.category}-answers.txt"))
    if not recordings:
        raise SystemExit(f"No {args.category}-answers.txt in any recording in {recordings_root}. Review some first.")
    out = model_folder(args.category)
    out.mkdir(parents=True, exist_ok=True)
    dataset = out / "dataset"
    counts = build_dataset(recordings, args.category, args.held_out, dataset, kinds)
    print(f"Training on {counts['train_images']} images ({counts['train_things']} marked), holding out "
          f"{counts['held_out_images']} ({counts['held_out_things']} marked), from {len(recordings)} recording(s)."
          + (f" Left out {counts['images_to_answer']} images with a guess added since their review still to answer."
             if counts["images_to_answer"] else ""), flush=True)
    if kinds:
        print("Kinds: " + ", ".join(f"{n} {kind}" for kind, n in counts["train_kinds"].items())
              + f". Left out {counts['kinds_unchecked_images']} images whose kinds are not checked yet.", flush=True)
        counts["train_images_repeated"], counts["train_crowd_images"] = repeat_rare(dataset / "train", REPEAT_RARE,
                                                                                    CROWD_REPEAT)
        print(f"Rare kinds and crowds ({counts['train_crowd_images']} images) repeated: {counts['train_images']} -> "
              f"{counts['train_images_repeated']} images per pass.", flush=True)

    started = time.time()
    training = out / "training"
    if training.exists():
        shutil.rmtree(training)
    model = new_model(args.model, args.resolution)
    model.train(dataset_dir=str(dataset), output_dir=str(training), epochs=args.epochs, batch_size=args.batch,
                grad_accum_steps=IMAGES_PER_STEP // args.batch, checkpoint_interval=args.epochs, tensorboard=False,
                seed=0, progress_bar="tqdm", aug_config=AUGMENTATIONS[args.augment], lr_scheduler=LR_SCHEDULE)
    minutes = (time.time() - started) / 60.0

    best = training / "checkpoint_best_total.pth"
    shutil.copy2(best, out / "model.pth")
    for resume_file in training.glob("*.ckpt"):  # 0.5 GB each, only for resuming a stopped run.
        resume_file.unlink()
    trained = load_live(out / "model.pth")
    scores = [score(trained, dataset, cutoff, kinds) for cutoff in SCORE_CUTOFFS]
    info = {
        "category": args.category,
        "model": args.model,
        "resolution": args.resolution,
        "live_resolution": LIVE_RESOLUTION,
        "trained": datetime.now().isoformat(timespec="seconds"),
        "minutes": round(minutes, 1),
        "epochs": args.epochs,
        "augment": args.augment,
        "lr_schedule": LR_SCHEDULE,
        **({"kinds": list(kinds), "repeat_rare": REPEAT_RARE, "crowd_repeat": CROWD_REPEAT} if kinds else {}),
        # observex reads "kinds"
        # (core/ModelInfo): the score slots' kinds.
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
              f"alarms ({result['false_alarms_per_image']:.1f} per image; {result['group_boxes']} around 2+ things)"
              + (f", the right kind on {result['right_kind_share']:.0%} of those found" if kinds else ""))


if __name__ == "__main__":
    main()
