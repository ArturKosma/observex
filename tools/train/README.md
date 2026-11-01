# Training tool

Teaches observex to find one category of things (for now: monsters) from the answers given in observex's review
screen, and writes the trained model's guesses back for the next review. Python, on the GPU. Step 4 of
[the training loop](../../docs/training-loop.md).

## Setup (once)

About 2.5 GB to download and 4 GB on disk, all inside `tools/train/.venv` (delete that folder to remove it):
PyTorch for CUDA 13, RF-DETR (Apache 2.0), its training packages and its image-changes extra (`rfdetr[augment]`:
Albumentations, for the colour changes below). The first training also downloads RF-DETR's pretrained weights
(about 0.4 GB) into `%USERPROFILE%\.roboflow\models`. Needs Python 3.10 or newer and an NVIDIA GPU. An older setup
without the extra fails with "Custom Albumentations augmentations require the optional augmentation extra", and one
without the ONNX packages (added for live detection) with "No module named 'onnx'": run the pip line below again.

```bash
py -3.13 -m venv tools/train/.venv
tools/train/.venv/Scripts/python -m pip install -r tools/train/requirements.txt
```

## Use

From the project folder:

```bash
tools/train/.venv/Scripts/python tools/train/train.py
tools/train/.venv/Scripts/python tools/train/guess.py recordings/2026-11-01_12-00-00
```

`train.py` trains on every recording that has `monster-answers.txt` (reviewed images only), keeps the last fifth of
each recording aside, and prints the score on it: how many of those monsters it finds and how many false alarms it
raises. The model goes to `models/monster/` next to observex.exe (`model.pth`, plus `model.json` with its settings,
numbers and score); its working files (`dataset/`, `training/`) stay there too and are rebuilt on each run.
It prints the score at three cutoffs (marks at least 30%, 40% and 50% sure): a lower cutoff finds more and raises
more false alarms. On the RTX 4090, 165 images take about 9 s per pass, so the default 40 passes take about
6 minutes, using about 12.6 GB of GPU memory. Run it in the background or in its own terminal, and never two
trainings at once: they do not fit in 24 GB together, Windows then moves GPU memory into normal RAM, and both crawl
or hang.

The answers it learns from should follow one rule (body only, every visible monster, no box for a hidden one); see
[the training loop](../../docs/training-loop.md), step 2.

It starts from RF-DETR pretrained on everyday photos, replaces its list of 80 kinds of things with the one category,
and fine-tunes it on the answers: everything else it learned (edges, shapes, what an object is) stays, which is why a
hundred images can be enough to start.

At the end, `train.py` also writes `model.onnx` (through `export.py`): the same model in the format observex's live
detection reads (half-precision weights). observex prepares it for the GPU the first time it observes a screen
with it (about 35 s on the 4090, once per new model) and saves that as `model.engine` next to it. To export an
existing `model.pth` again by hand:

```bash
tools/train/.venv/Scripts/python tools/train/export.py
```

`guess.py` runs the model over a recording and writes `monster-guesses.txt` into it, in half precision (about 24 ms
per image on the 4090 instead of 43, the same guesses, after about 2.5 s of setup). observex's review runs it by
itself, in the background with no window, when you open a recording whose guesses are missing or older than the
model, and shows its progress; the guesses then show as marks with how sure the model is, to answer yes or no.
Running it by hand does the same.

Options: `--category` (default `monster`), and for `train.py` `--model nano|small|medium|large` (default `medium`),
`--resolution` (default 1024: the image is squeezed to that many pixels square, so far-away monsters stay a few
pixels big), `--epochs` (passes over the images, default 40), `--augment flip|color` (random changes to each
training image, default `color`: mirror, plus small shifts of brightness, contrast, saturation and hue, so a monster
in a dark cave or in fog still looks like itself), `--held-out` (default 0.2) and `--batch` (images through the GPU
at once, default 4; each learning step uses 16 either way). `guess.py --threshold` (default 0.3) leaves out guesses
less sure than that.

Why these defaults (tested 2026-11-01 on 5 recordings, each place scored by a model that never saw it): Medium beat
Small and Large; colour changes beat mirror only; 1024 beat 1280 and tied 768 (768 is faster, keep it in mind for
live detection); the best pass always came before 35, so 80 passes gained nothing. Details in
[the training loop](../../docs/training-loop.md), step 4.

The batch size is fixed on purpose. RF-DETR's `batch_size="auto"` tries bigger batches until one does not fit, but on
Windows the NVIDIA driver then moves GPU memory into normal RAM instead of failing, so the search never ends.

## Files

| File | What it does |
| --- | --- |
| `marks.py` | Reads and writes observex's marks files (`src/core/Marks.h` is the same format in C++). |
| `train.py` | Builds the dataset from the answers, trains RF-DETR, saves the model and its score. |
| `guess.py` | Writes the model's guesses for a recording. |
| `export.py` | Writes `model.onnx` for observex's live detection (`train.py` runs it too). |
| `requirements.txt` | The pinned Python packages (the last four are for `export.py`). |
