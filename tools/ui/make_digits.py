"""Cuts the glyphs that models/ui/stats.txt lists out of recorded images (observex's recordings and clip pictures).

For each `glyphs NAME CHARS PITCH W H` line and its indented `from IMAGE X Y TEXT` lines it turns each IMAGE grey as
observex does ((blue * 29 + green * 150 + red * 77) >> 8), cuts the W x H picture of every character of TEXT (the first
at X Y, each next PITCH pixels on), and writes models/ui/NAME.pgm (each character's mean picture, side by side in
CHARS' order) and NAME-mask.pgm (255 on the letter, its white core, and the dark line round it; 0 elsewhere, where the
bar shows through). Every character of CHARS needs one sample at least. Also writes models/ui/NAME.png, larger, to
look them over.

A `glyphs ... yellow` font (a cooldown's seconds, yellow over any picture) is found by colour instead: its picture is
the share of samples each pixel was yellow on (red at least 170 and 12 above blue), times 255, and its mask is
everywhere. An IMAGE may be a frame of a clip's video: `recordings/clips/NAME/clip.mp4@FRAME` (OpenCV reads it).

    python tools/ui/make_digits.py [--ui models/ui]
"""
import argparse
import pathlib

import numpy as np
from PIL import Image

CORE = 175  # The letters are white: a pixel whose darkest colour channel is above this (on any sample) is the letter.
LEFT = 2  # Where a character's own cell starts in its picture (one pitch wide but its last column, which stays dark).


YELLOW_RED = 170  # A yellow letter's pixel: red at least this, and at least YELLOW_OVER_BLUE above blue.
YELLOW_OVER_BLUE = 12


def read_rgb(here, image):
    """IMAGE (from the project folder) as RGB, or frame FRAME of a video for `VIDEO@FRAME`."""
    if "@" in image:
        import cv2  # Only for clip frames.

        video, frame = image.rsplit("@", 1)
        capture = cv2.VideoCapture(str(here / video))
        capture.set(cv2.CAP_PROP_POS_FRAMES, int(frame))
        ok, bgr = capture.read()
        if not ok:
            raise SystemExit(f"{image}: no such frame")
        return bgr[..., ::-1].astype(np.uint32)
    return np.asarray(Image.open(here / image).convert("RGB")).astype(np.uint32)


def write_pgm(path, pixels):
    h, w = pixels.shape
    with open(path, "wb") as out:
        out.write(b"P5\n%d %d\n255\n" % (w, h))
        out.write(pixels.astype(np.uint8).tobytes())


def main():
    here = pathlib.Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser()
    parser.add_argument("--ui", default=str(here / "models" / "ui"))
    args = parser.parse_args()
    ui = pathlib.Path(args.ui)

    fonts = []  # (name, chars, pitch, w, h, [(image, x, y, text)], yellow)
    for line in (ui / "stats.txt").read_text(encoding="utf-8").splitlines():
        words = line.split()
        if len(words) in (6, 7) and words[0] == "glyphs":
            fonts.append((words[1], words[2], int(words[3]), int(words[4]), int(words[5]), [], len(words) == 7))
        elif len(words) == 5 and words[0] == "from" and fonts:
            fonts[-1][5].append((words[1], int(words[2]), int(words[3]), words[4]))

    for name, chars, pitch, w, h, samples, yellow in fonts:
        if yellow:
            make_yellow(ui, here, name, chars, pitch, w, h, samples)
            continue
        sums = {c: np.zeros((h, w)) for c in chars}
        cores = {c: np.zeros((h, w)) for c in chars}
        counts = {c: 0 for c in chars}
        for image, x, y, text in samples:
            rgb = read_rgb(here, image)
            grey = ((rgb[..., 2] * 29 + rgb[..., 1] * 150 + rgb[..., 0] * 77) >> 8).astype(np.float64)
            core = rgb.min(axis=2) > CORE
            for i, c in enumerate(text):
                if c not in sums:
                    raise SystemExit(f"{image}: '{c}' is not one of {chars}")
                x0 = x + i * pitch
                sums[c] += grey[y:y + h, x0:x0 + w]
                cores[c] += core[y:y + h, x0:x0 + w]
                counts[c] += 1
        missing = [c for c in chars if counts[c] == 0]
        if missing:
            raise SystemExit(f"{name}: no sample of {''.join(missing)}")

        pictures = np.zeros((h, w * len(chars)))
        masks = np.zeros((h, w * len(chars)))
        for i, c in enumerate(chars):
            mean = sums[c] / counts[c]
            letter = cores[c] / counts[c] >= 0.5
            # Only in its own cell: the columns round it hold the next and the last character's edges.
            letter[:, :LEFT] = False
            letter[:, LEFT + pitch - 1:] = False
            # With the dark line round the letter: its 8 neighbours.
            padded = np.pad(letter, 1)
            mask = np.zeros_like(letter)
            for dy in (0, 1, 2):
                for dx in (0, 1, 2):
                    mask |= padded[dy:dy + h, dx:dx + w]
            pictures[:, i * w:(i + 1) * w] = mean
            masks[:, i * w:(i + 1) * w] = mask * 255
            print(f"{name} '{c}': {counts[c]} samples, {int(letter.sum())} letter pixels, {int(mask.sum())} masked")
        write_pgm(ui / f"{name}.pgm", np.round(pictures))
        write_pgm(ui / f"{name}-mask.pgm", masks)
        look = np.where(masks > 0, pictures, 255 * 0.35)
        Image.fromarray(look.astype(np.uint8)).resize((w * len(chars) * 6, h * 6), Image.NEAREST).save(ui / f"{name}.png")


def make_yellow(ui, here, name, chars, pitch, w, h, samples):
    shares = {c: np.zeros((h, w)) for c in chars}
    counts = {c: 0 for c in chars}
    for image, x, y, text in samples:
        rgb = read_rgb(here, image).astype(np.int64)
        on = (rgb[..., 0] >= YELLOW_RED) & (rgb[..., 0] - rgb[..., 2] >= YELLOW_OVER_BLUE)
        for i, c in enumerate(text):
            if c not in shares:
                raise SystemExit(f"{image}: '{c}' is not one of {chars}")
            x0 = x + i * pitch
            shares[c] += on[y:y + h, x0:x0 + w]
            counts[c] += 1
    missing = [c for c in chars if counts[c] == 0]
    if missing:
        raise SystemExit(f"{name}: no sample of {''.join(missing)}")
    pictures = np.zeros((h, w * len(chars)))
    for i, c in enumerate(chars):
        share = shares[c] / counts[c]
        pictures[:, i * w:(i + 1) * w] = share * 255
        print(f"{name} '{c}': {counts[c]} samples, {int((share >= 0.5).sum())} letter pixels")
    write_pgm(ui / f"{name}.pgm", np.round(pictures))
    write_pgm(ui / f"{name}-mask.pgm", np.full_like(pictures, 255))
    Image.fromarray(pictures.astype(np.uint8)).resize((w * len(chars) * 6, h * 6), Image.NEAREST).save(ui / f"{name}.png")


if __name__ == "__main__":
    main()
