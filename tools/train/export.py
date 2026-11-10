"""Writes a trained model in the form observex runs live: models/<category>/model.onnx.

    tools/train/.venv/Scripts/python tools/train/export.py [--category character]

train.py runs this by itself after training. ONNX is a plain description of the network and its numbers. observex
turns it into a TensorRT engine for this GPU the first time it observes a screen after a new model (about a minute
or two), and keeps that next to it as model.engine.

The network is stored in half precision, because TensorRT 11 takes the precision from the file; its input and outputs
stay 32-bit. Input: the screen image squeezed to LIVE_RESOLUTION (train.py), colours scaled the way RF-DETR does.
Outputs: 300 candidate boxes (centre x, centre y, width and height, as shares of the image), scores for each,
before the sigmoid: the category's (or with --kinds one per kind, in model.json's order), then an unused slot; and
(since 2026-11-08) "features": for each candidate, the 256 numbers the model's kind classifier reads (the input of
RF-DETR's class_embed). The classifier itself (its weights and biases for each kind) goes into model.json as
"kind_head". observex's review re-fits that classifier to the user's answers while they answer (core/KindLearner).
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import os
import tempfile
import time
from pathlib import Path

import torch

from marks import model_folder
from train import load_live, utf8_output


class WithFeatures(torch.nn.Module):
    """RF-DETR's exported network with one more output: per candidate, what its kind classifier reads."""

    def __init__(self, network: torch.nn.Module) -> None:
        super().__init__()
        self.network = network
        self.seen: dict = {}
        network.class_embed.register_forward_hook(lambda module, inputs, output: self.seen.update(features=inputs[0]))

    def export(self) -> None:
        """RF-DETR's exporter switches the network to its export form through this."""
        self.network.export()

    def forward(self, image: torch.Tensor):
        boxes, scores = self.network(image)
        features = self.seen["features"]
        return boxes, scores, features[-1] if features.dim() == 4 else features  # The last decoder layer's.


def write_onnx(model, folder: Path) -> Path:
    """Exports `model` (load_live) into `folder` with the features output, in half precision. Returns its path."""
    from rfdetr.export import prepare  # RF-DETR 1.11.2's own export, with the network wrapped on the way.
    from rfdetr.export._tensorrt.exporter import _cast_onnx_to_fp16  # Its half-precision cast.

    original = prepare.prepare_export_graph

    def with_features(*args, **kwargs):
        graph = original(*args, **kwargs)
        return dataclasses.replace(graph, model=WithFeatures(graph.model),
                                   output_names=(*graph.output_names, "features"))

    prepare.prepare_export_graph = with_features
    try:
        full = model.export(output_dir=str(folder), format="onnx", verbose=False)
    finally:
        prepare.prepare_export_graph = original
    return Path(_cast_onnx_to_fp16(str(full)))


def save_kind_head(model, info_file: Path) -> None:
    """Adds the kind classifier to model.json as "kind_head": one bias and 256 weights per kind (one row for a
    model of one category), rounded to half precision as model.onnx holds them, so observex's sureness from the
    features is the model's own."""
    info = json.loads(info_file.read_text(encoding="utf-8"))
    head = model.model.model.class_embed
    rows = max(1, len(info.get("kinds") or []))
    info["kind_head"] = {
        "biases": head.bias.detach()[:rows].half().float().tolist(),
        "weights": head.weight.detach()[:rows].half().float().tolist(),
    }
    temporary = info_file.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(info, indent=2), encoding="utf-8")
    os.replace(temporary, info_file)  # Whole, or not at all: observex may read it at any time.


def export_onnx(category: str) -> Path:
    """Writes models/<category>/model.onnx from model.pth, and its kind head into model.json. Returns the path."""
    folder = model_folder(category)
    model = load_live(folder / "model.pth")
    target = folder / "model.onnx"
    with tempfile.TemporaryDirectory(dir=folder) as work:
        os.replace(write_onnx(model, Path(work)), target)  # Whole, or not at all: observex may read it at any time.
    save_kind_head(model, folder / "model.json")
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
