"""Writes a trained model in the form observex runs live: models/<category>/model.onnx.

    tools/train/.venv/Scripts/python tools/train/export.py [--category character]

train.py runs this by itself after training. ONNX is a plain description of the network and its numbers. observex
turns it into a TensorRT engine for this GPU the first time it observes a screen after a new model (about a minute
or two), and keeps that next to it as model.engine.

The network is stored in half precision, because TensorRT 11 takes the precision from the file; its input and outputs
stay 32-bit. Input: the screen image squeezed to LIVE_RESOLUTION (train.py), colours scaled the way RF-DETR does.
Outputs: 300 candidate boxes (centre x, centre y, width and height, as shares of the image) and two scores for each,
before the sigmoid: the first is the category, the second an unused slot.
"""

from __future__ import annotations

import argparse
import os
import tempfile
import time
from pathlib import Path

from marks import model_folder
from train import load_live, utf8_output


def export_onnx(category: str) -> Path:
    """Writes models/<category>/model.onnx from model.pth and returns its path."""
    from rfdetr.export._tensorrt.exporter import _cast_onnx_to_fp16  # RF-DETR's own half-precision cast (1.11.2).

    folder = model_folder(category)
    model = load_live(folder / "model.pth")
    target = folder / "model.onnx"
    with tempfile.TemporaryDirectory(dir=folder) as work:
        full = model.export(output_dir=work, format="onnx", verbose=False)
        half = _cast_onnx_to_fp16(str(full))
        os.replace(half, target)  # Whole, or not at all: observex may read it at any time.
    return target


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--category", default="character")
    args = parser.parse_args()
    utf8_output()

    if not (model_folder(args.category) / "model.pth").exists():
        raise SystemExit(f"No trained model in {model_folder(args.category)}. Run train.py first.")
    started = time.time()
    target = export_onnx(args.category)
    print(f"Wrote {target} ({target.stat().st_size / 1e6:.0f} MB) in {time.time() - started:.0f} s.")


if __name__ == "__main__":
    main()
