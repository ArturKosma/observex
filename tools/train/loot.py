"""Drops (loot): looked for only around a corpse, on a full-resolution square cut from the screen image.

Drops are tiny on the whole screen (an adena pile is 20 x 9 pixels of 2560 x 1440 when the camera is far off: 8 x 6
once squeezed to the character model's 1024), and the game drops a kill's loot within 70 game units of the corpse in
each direction (L2J's RANDOM_ITEM_DROP_LIMIT; the own character is 47 units tall). So observex looks for them only
around a target it saw die (core/TargetWatch), on a square cut round the corpse's box and scaled to the model's input
(src/core/LootWatch.h, lootSquare, holds the same rule; keep the two alike):

    side = LOOT_SIDE x the box's longer side, at least LOOT_MIN_SIDE and at most the screen's height
    middle = the box's middle across; down, half its shorter side above its bottom (a corpse lying down: its middle;
             one still standing: near its feet)

A drop is then about a twentieth to a quarter of the square across, whatever the camera's distance.

The training images (the user's Shift+Win screenshots, reviewed for Drops) were taken after the corpse was gone, so the
squares are made round the drops instead: of many sizes (a drop LOOT_SHARES of the side across) and placed so the drops
lie anywhere in the square's middle LOOT_INSIDE (where they land round a corpse at its middle), each image several
times; and squares of the same sizes with no drop in them, as "nothing here". On a reviewed image every drop is
marked, so a square without one is truly empty. Images go in or out of the score whole (the last fifth by name), as
in train.py.
"""

from __future__ import annotations

import math
import random
import shutil
from pathlib import Path

from marks import Mark, overlap, read_marks, write_guesses

# The rule above (src/core/LootWatch.h: kLootSide, kLootMinSide).
LOOT_SIDE = 4.0
LOOT_MIN_SIDE = 192
# The model sees the square at this many pixels (RF-DETR Medium's own size; a multiple of 32).
LOOT_RESOLUTION = 576
# How much of the square's side a drop takes across, from the camera far off to close: the square is the largest drop's
# longer side divided by a share from this range (picked evenly on a log scale).
LOOT_SHARES = (0.05, 0.25)
# The drops lie in the middle part of the square this wide (a share of its side).
LOOT_INSIDE = 0.8
# Squares per training image: with drops, and without.
SQUARES_WITH = 8
SQUARES_WITHOUT = 4
# Held-out images give fewer, made the same way every time (their own seed).
HELD_SQUARES_WITH = 4
HELD_SQUARES_WITHOUT = 4
# Look-alikes: what is not a drop but looks like one to the model (the yellow ring under the target, a skill's blue
# rune, spell sparkles): a "no" answer, or a guess (in <category>-guesses.txt) at least LOOK_ALIKE_SURE sure that
# touches no drop of a reviewed image, where every drop is marked. Squares cut round each, sized and placed as round
# drops, teach the model what is not one (the drops in them marked as usual). Guesses found so are kept in
# <category>-look-alikes.txt next to the answers, so they still count once a model that learned them no longer
# guesses them (the review writes the guesses afresh for each new model).
LOOK_ALIKE_SURE = 0.3
SQUARES_ROUND_LOOK_ALIKE = 5
HELD_SQUARES_ROUND_LOOK_ALIKE = 2


def spot_side(longest: float, screen_height: int) -> int:
    """The square's side round a corpse whose box's longer side is `longest` (the rule above)."""
    return int(round(min(max(LOOT_SIDE * longest, LOOT_MIN_SIDE), screen_height)))


def _inside(left: float, top: float, side: float, size: tuple[int, int]) -> tuple[int, int, int]:
    """The square moved (not shrunk) to lie inside the image, as observex cuts it at the screen's edges."""
    width, height = size
    side = min(side, width, height)
    left = min(max(left, 0.0), width - side)
    top = min(max(top, 0.0), height - side)
    return int(round(left)), int(round(top)), int(round(side))


def _square_with(drops: list[Mark], size: tuple[int, int], rng: random.Random) -> tuple[int, int, int] | None:
    """A square holding every drop of the image, sized and placed as described above; None if none fits."""
    left = min(d.x for d in drops)
    top = min(d.y for d in drops)
    right = max(d.x + d.width for d in drops)
    bottom = max(d.y + d.height for d in drops)
    largest = max(max(d.width, d.height) for d in drops)
    low, high = LOOT_SHARES
    share = math.exp(rng.uniform(math.log(low), math.log(high)))
    # Big enough for the drops' spread in the middle part.
    side = max(largest / share, (right - left) / LOOT_INSIDE, (bottom - top) / LOOT_INSIDE, LOOT_MIN_SIDE)
    if side > min(size):
        return None
    margin = side * (1.0 - LOOT_INSIDE) / 2.0
    x = rng.uniform(right + margin - side, left - margin)
    y = rng.uniform(bottom + margin - side, top - margin)
    return _inside(x, y, side, size)


def _square_without(drops: list[Mark], largest: float, size: tuple[int, int],
                    rng: random.Random) -> tuple[int, int, int] | None:
    """A square of the sizes _square_with makes for a drop `largest` across, touching no drop; None if 50 tries
    found none."""
    low, high = LOOT_SHARES
    for _ in range(50):
        side = max(largest / math.exp(rng.uniform(math.log(low), math.log(high))), LOOT_MIN_SIDE)
        if side > min(size):
            continue
        x, y, side = _inside(rng.uniform(0, size[0] - side), rng.uniform(0, size[1] - side), side, size)
        if all(d.x + d.width <= x or d.x >= x + side or d.y + d.height <= y or d.y >= y + side for d in drops):
            return x, y, side
    return None


def guesses_file(recording: Path, category: str) -> Path:
    """Where a recording's guesses are (observex's review writes them with the live model)."""
    return recording / f"{category}-guesses.txt"


def look_alikes_file(recording: Path, category: str) -> Path:
    """Where the look-alikes found by earlier models on a recording are kept (marks format, as guesses)."""
    return recording / f"{category}-look-alikes.txt"


def _marks_if_any(path: Path) -> dict[str, list[Mark]]:
    return read_marks(path) if path.exists() else {}


def _touches(a: Mark, b: Mark) -> bool:
    return a.x < b.x + b.width and b.x < a.x + a.width and a.y < b.y + b.height and b.y < a.y + a.height


def _look_alikes(answered: list[Mark], guessed: list[Mark], drops: list[Mark]) -> list[Mark]:
    """The look-alikes of one reviewed image (described above), each once."""
    found = [mark for mark in answered if mark.answer == "no"]
    for guess in guessed:
        if (guess.guess is not None and guess.guess >= LOOK_ALIKE_SURE and
                not any(_touches(guess, d) for d in drops) and not any(overlap(guess, f) >= 0.5 for f in found)):
            found.append(guess)
    return found


def _square_round(thing: Mark, size: tuple[int, int], rng: random.Random) -> tuple[int, int, int] | None:
    """A square round `thing` as _square_with makes round drops; None if 10 tries found none that fits."""
    for _ in range(10):
        square = _square_with([thing], size, rng)
        if square is not None:
            return square
    return None


def _cut(image, square: tuple[int, int, int], drops: list[Mark]) -> tuple[object, list[Mark]]:
    """The square cut out at full resolution, and the drops in it (in its pixels)."""
    x, y, side = square
    piece = image.crop((x, y, x + side, y + side))
    inside = []
    for d in drops:
        left, top = max(d.x, x), max(d.y, y)
        right, bottom = min(d.x + d.width, x + side), min(d.y + d.height, y + side)
        if right - left >= 2 and bottom - top >= 2:
            inside.append(Mark(left - x, top - y, right - left, bottom - top, answer="yes"))
    return piece, inside


def build_loot_dataset(recordings: list[Path], category: str, held_out_share: float, dataset: Path,
                       write_split) -> dict:
    """Writes the squares of every reviewed image of `recordings` as train and valid splits in `dataset` (through
    train.py's `write_split`), the last `held_out_share` of each recording's images held out."""
    from PIL import Image

    if dataset.exists():
        shutil.rmtree(dataset)
    pieces = dataset / "pieces"  # The squares as files; the splits link to them.
    pieces.mkdir(parents=True)
    train, valid = [], []
    images = {"train": 0, "valid": 0}
    look_alikes = {"train": 0, "valid": 0}
    for recording in recordings:
        answers = read_marks(recording / f"{category}-answers.txt")
        guesses = _marks_if_any(guesses_file(recording, category))
        kept = _marks_if_any(look_alikes_file(recording, category))
        keep: dict[str, list[Mark]] = {}  # This recording's look-alikes from guesses, kept for later trainings.
        names = sorted(name for name in answers if (recording / name).exists())
        names = [name for name in names if all(mark.answer is not None for mark in answers[name])]
        held_out = max(1, round(len(names) * held_out_share)) if len(names) >= 5 else 0
        for index, name in enumerate(names):
            held = index >= len(names) - held_out
            drops = [mark for mark in answers[name] if mark.answer == "yes"]
            rng = random.Random(f"{recording.name}/{name}/{'valid' if held else 'train'}")
            with Image.open(recording / name) as opened:
                image = opened.convert("RGB")
            images["valid" if held else "train"] += 1
            wanted = [(True, HELD_SQUARES_WITH if held else SQUARES_WITH),
                      (False, HELD_SQUARES_WITHOUT if held else SQUARES_WITHOUT)]
            # An image with no drop sizes its empty squares as for a typical adena pile.
            largest = max((max(d.width, d.height) for d in drops), default=40)
            for with_drops, count in wanted:
                if with_drops and not drops:
                    continue
                for n in range(count):
                    square = (_square_with(drops, image.size, rng) if with_drops else
                              _square_without(drops, largest, image.size, rng))
                    if square is None:
                        continue
                    piece, inside = _cut(image, square, drops)
                    file = pieces / f"{recording.name}_{Path(name).stem}_{'d' if with_drops else 'e'}{n}.png"
                    piece.save(file)
                    (valid if held else train).append((file, file.name, inside))
            # Round each look-alike (its own seed, so the squares above stay as they were without them).
            alike_rng = random.Random(f"{recording.name}/{name}/{'valid' if held else 'train'}/look-alikes")
            alikes = _look_alikes(answers[name], kept.get(name, []) + guesses.get(name, []), drops)
            if any(thing.guess is not None for thing in alikes):
                keep[name] = [thing for thing in alikes if thing.guess is not None]
            for a, thing in enumerate(alikes):
                look_alikes["valid" if held else "train"] += 1
                for n in range(HELD_SQUARES_ROUND_LOOK_ALIKE if held else SQUARES_ROUND_LOOK_ALIKE):
                    square = _square_round(thing, image.size, alike_rng)
                    if square is None:
                        continue
                    piece, inside = _cut(image, square, drops)
                    file = pieces / f"{recording.name}_{Path(name).stem}_l{a}_{n}.png"
                    piece.save(file)
                    (valid if held else train).append((file, file.name, inside))
        if keep:
            write_guesses(look_alikes_file(recording, category), keep,
                          "Look-alikes: what drop models guessed on these reviewed images that touches no drop (the\n"
                          "user marked every drop), kept by tools/train/loot.py so later trainings still learn they\n"
                          "are not drops. Each line: how sure the model that found it was, and its box.")
    if not train or not valid:
        raise SystemExit(f"Not enough reviewed images: {images['train']} to train on, {images['valid']} to hold out. "
                         "Review at least 5 images first.")
    return {
        "train_images": images["train"],
        "train_look_alikes": look_alikes["train"],
        "held_out_look_alikes": look_alikes["valid"],
        "train_squares": len(train),
        "train_things": write_split(dataset / "train", train, [category]),
        "held_out_images": images["valid"],
        "held_out_squares": len(valid),
        "held_out_things": write_split(dataset / "valid", valid, [category]),
        "loot_side": LOOT_SIDE,
        "loot_min_side": LOOT_MIN_SIDE,
    }
