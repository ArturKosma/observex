"""Reads and writes observex's marks files: the text format of src/core/Marks.h.

Each image starts with an "image <file name>" line, and its marks follow, one per line: "yes", "no" or a guess
(how sure the finder is, 0 to 1), then x, y, width and height in image pixels. Lines starting with '#' are comments.

    <category>-answers.txt  written by observex's review: only reviewed images, marks answered yes or no.
    <category>-guesses.txt  written by guess.py: the trained model's marks, with how sure it is.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]  # The project folder, where observex.exe lives.


@dataclass
class Mark:
    x: int
    y: int
    width: int
    height: int
    answer: str | None = None  # "yes", "no", or None if unanswered.
    guess: float | None = None  # How sure the model was, 0 to 1; None if it did not make this mark.


def read_marks(path: Path) -> dict[str, list[Mark]]:
    """The marks of each image in `path`, by image file name. Lines it does not understand are skipped."""
    marks: dict[str, list[Mark]] = {}
    image: list[Mark] | None = None
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        words = line.split()
        if words[0] == "image":
            name = line[len("image"):].strip()
            image = marks.setdefault(name, []) if name else None
            continue
        if image is None or len(words) != 5:
            continue
        try:
            x, y, width, height = (int(word) for word in words[1:])
        except ValueError:
            continue
        if width <= 0 or height <= 0:
            continue
        if words[0] in ("yes", "no"):
            image.append(Mark(x, y, width, height, answer=words[0]))
            continue
        try:
            guess = float(words[0])
        except ValueError:
            continue
        if 0.0 <= guess <= 1.0:
            image.append(Mark(x, y, width, height, guess=guess))
    return marks


def write_guesses(path: Path, guesses: dict[str, list[Mark]], comment: str) -> None:
    """Writes `guesses` (marks with a guess) to `path`, images in name order, replacing the file in one step."""
    lines = ["# " + line for line in comment.splitlines()]
    for name in sorted(guesses):
        lines.append(f"image {name}")
        for mark in guesses[name]:
            lines.append(f"{mark.guess:.3f} {mark.x} {mark.y} {mark.width} {mark.height}")
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text("\n".join(lines) + "\n", encoding="utf-8")
    temporary.replace(path)


def recordings_folder() -> Path:
    """Where observex saves recordings: the folder set in observex.cfg, else "recordings" next to observex.exe."""
    settings = ROOT / "observex.cfg"
    if settings.exists():
        for line in settings.read_text(encoding="utf-8").splitlines():
            key, _, value = line.partition("=")
            if key == "recordings" and value.strip():
                return Path(value.strip())
    return ROOT / "recordings"


def model_folder(category: str) -> Path:
    """Where the trained model of `category` lives: models/<category> next to observex.exe."""
    return ROOT / "models" / category


def image_files(recording: Path) -> list[Path]:
    """The images of a recording, in name order, as observex's review lists them."""
    return sorted(p for p in recording.iterdir() if p.suffix.lower() in (".jpg", ".jpeg", ".png"))


def overlap(a: Mark, b: Mark) -> float:
    """How much two boxes cover the same area: 0 = not at all, 1 = the same box (intersection over union)."""
    width = min(a.x + a.width, b.x + b.width) - max(a.x, b.x)
    height = min(a.y + a.height, b.y + b.height) - max(a.y, b.y)
    if width <= 0 or height <= 0:
        return 0.0
    shared = width * height
    return shared / (a.width * a.height + b.width * b.height - shared)
