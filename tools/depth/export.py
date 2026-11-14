"""Writes the depth network observex runs live (the Show flag Depth): models/depth/model.onnx.

    tools/train/.venv/Scripts/python tools/depth/export.py [--width 924] [--height 518]

The network is Depth Anything V2 Small (Apache 2.0; https://huggingface.co/depth-anything/Depth-Anything-V2-Small-hf),
downloaded by huggingface_hub into its cache the first time (99 MB). From one image it guesses, for every pixel, how
near it is: relative inverse depth, larger nearer, in no unit (each image has its own scale and offset).

observex turns model.onnx into a TensorRT engine for this GPU the first time the flag is on after a new model.onnx,
and keeps it next to it as model.engine (engine/DepthNet). The network is stored in half precision, because TensorRT
11 takes the precision from the file. Input "image": 1 x 3 x height x width, 32-bit, the screen squeezed to that size,
colours 0 to 1 normalised with ImageNet's mean and spread (as Depth Anything's own image processor does). Output
"depth": 1 x height x width, half precision, the network's own output. Both sides a multiple of 14 (its patch size):
518 x 924 is 16:9 (2560 x 1440 screens) at the 518 the network was trained at.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

import torch

REPOSITORY = "depth-anything/Depth-Anything-V2-Small-hf"
ROOT = Path(__file__).resolve().parents[2]


class Exported(torch.nn.Module):
    """The network in half precision, taking a 32-bit image and giving its half-precision depth."""

    def __init__(self, network: torch.nn.Module) -> None:
        super().__init__()
        self.network = network

    def forward(self, image: torch.Tensor) -> torch.Tensor:
        return self.network(pixel_values=image.half()).predicted_depth


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--width", type=int, default=924)
    parser.add_argument("--height", type=int, default=518)
    parser.add_argument("--out", type=Path, default=ROOT / "models" / "depth")
    args = parser.parse_args()
    if args.width % 14 or args.height % 14:
        print("Width and height must be multiples of 14.", file=sys.stderr)
        return 1

    from huggingface_hub import snapshot_download
    from transformers import AutoModelForDepthEstimation

    folder = snapshot_download(REPOSITORY, allow_patterns=["*.json", "*.safetensors", "*.md"])
    network = AutoModelForDepthEstimation.from_pretrained(folder).cuda().half().eval()
    model = Exported(network).eval()
    image = torch.rand(1, 3, args.height, args.width, device="cuda")

    args.out.mkdir(parents=True, exist_ok=True)
    onnx_file = args.out / "model.onnx"
    temporary = onnx_file.with_suffix(".onnx.tmp")
    started = time.perf_counter()
    with torch.inference_mode():
        torch.onnx.export(model, (image,), str(temporary), input_names=["image"], output_names=["depth"],
                          opset_version=17, do_constant_folding=True, dynamo=False)
    temporary.replace(onnx_file)
    info = {
        "network": REPOSITORY,
        "width": args.width,
        "height": args.height,
        "output": "relative inverse depth, larger nearer, half precision",
        "written": time.strftime("%Y-%m-%d %H:%M:%S"),
    }
    (args.out / "model.json").write_text(json.dumps(info, indent=2) + "\n", encoding="utf-8")
    print(f"Wrote {onnx_file} ({onnx_file.stat().st_size / 1e6:.1f} MB) in {time.perf_counter() - started:.1f} s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
