"""Runs the trained model over a recording and writes its guesses for observex's review.

    tools/train/.venv/Scripts/python tools/train/guess.py RECORDING_FOLDER [--category monster] [--threshold 0.3]

Writes <category>-guesses.txt into the recording's folder: for every image, each thing the model found with how
sure it is. The review then shows them as marks to answer yes or no. The threshold is low on purpose: unsure
guesses are the ones worth asking about, and answering no to a wrong one teaches the model too.
"""

from __future__ import annotations

import argparse
import json
import time
from pathlib import Path

import torch

from marks import Mark, image_files, model_folder, write_guesses
from train import load_trained, utf8_output


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("recording", type=Path)
    parser.add_argument("--category", default="monster")
    parser.add_argument("--threshold", type=float, default=0.3, help="leave out guesses less sure than this (0 to 1)")
    args = parser.parse_args()
    utf8_output()

    folder = model_folder(args.category)
    if not (folder / "model.pth").exists():
        raise SystemExit(f"No trained model in {folder}. Run train.py first.")
    info = json.loads((folder / "model.json").read_text(encoding="utf-8"))
    model = load_trained(folder / "model.pth")
    # Half precision, compiled: about 2.5 s once, then about 24 ms per image instead of 43 (RTX 4090, 2026-11-01),
    # with the same guesses (boxes at most a pixel apart).
    model.inference(dtype=torch.float16)

    images = image_files(args.recording)
    if not images:
        raise SystemExit(f"No images in {args.recording}.")
    guesses: dict[str, list[Mark]] = {}
    started = time.time()
    for index, path in enumerate(images, start=1):
        detections = model.predict(str(path), threshold=args.threshold, include_source_image=False)
        marks = []
        for (left, top, right, bottom), confidence in zip(detections.xyxy, detections.confidence):
            marks.append(Mark(round(float(left)), round(float(top)), max(1, round(float(right - left))),
                              max(1, round(float(bottom - top))), guess=float(confidence)))
        guesses[path.name] = sorted(marks, key=lambda mark: (mark.y, mark.x))  # Reading order.
        print(f"{index} of {len(images)} images", flush=True)  # observex's review shows this progress.

    target = args.recording / f"{args.category}-guesses.txt"
    write_guesses(target, guesses,
                  f"observex guesses, category: {args.category}, by models/{args.category}/model.pth "
                  f"(trained {info['trained']}).\n"
                  "Each mark: how sure the model is (0 to 1), then x, y, width and height in image pixels.")
    count = sum(len(marks) for marks in guesses.values())
    seconds = time.time() - started
    print(f"Wrote {count} guesses for {len(images)} images to {target} in {seconds:.0f} s "
          f"({seconds / len(images) * 1000:.0f} ms per image).")


if __name__ == "__main__":
    main()
