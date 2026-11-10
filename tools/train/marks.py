"""Reads and writes observex's marks files: the text format of src/core/Marks.h.

Each image starts with an "image <file name>" line, and its marks follow, one per line: what it is, then x, y, width
and height in image pixels. What it is: a kind of the category (a yes; for characters "monster", "player", "npc",
"own", "unknown" = can't tell which, or "dead"), "yes" (answered before the category had kinds: its kind is not
checked yet, maybe with a suggested kind after it, e.g. "yes own 1260 708 51 57"), "no", or a guess (how sure the finder is, 0 to 1). A guess of a model
trained with kinds then names the kind it is surest of and the next, each with how sure of it: "0.820 monster 0.700
player 0.400 1012 540 80 120" (82% sure it is one of the two: src/core/Detection.h's eitherKind). Lines starting
with '#' are comments.

    <category>-answers.txt  written by observex's review: only reviewed images, marks answered with a kind or no.
    <category>-guesses.txt  written by observex's review (or guess.py): the model's marks, with how sure it is.
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
    answer: str | None = None  # "yes" (of any kind), "no", or None if unanswered.
    guess: float | None = None  # How sure the model was, 0 to 1; None if it did not make this mark.
    kind: str | None = None  # For a yes: its kind ("monster"), or the suggested one while unchecked; None if no kind.
    kind_checked: bool = True  # False for a yes answered before kinds whose kind the user has not checked yet.
    # For a guess of a model with kinds: `kind` is the one it was surest of, kind_guess how sure of it, and these the
    # next and how sure (0 to 1).
    second_kind: str | None = None
    second_guess: float | None = None
    kind_guess: float | None = None


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
        if image is None:
            continue
        what = words[0]
        suggested = None
        second_kind = None
        second_guess = None
        kind_guess = None
        if what == "yes" and len(words) == 6 and words[1].isalpha():  # "yes own 1260 708 51 57"
            suggested = words[1]
            words = [what] + words[2:]
        elif what[:1].isdigit() and len(words) in (7, 9) and words[1].isalpha():  # A guess with kinds.
            suggested = words[1]
            try:
                kind_guess = float(words[2])
                if len(words) == 9 and words[3].isalpha():  # "0.820 monster 0.700 player 0.400 1012 540 80 120"
                    second_kind, second_guess = words[3], float(words[4])
            except ValueError:
                continue
            words = [what] + words[-4:]
        if len(words) != 5:
            continue
        try:
            x, y, width, height = (int(word) for word in words[1:])
        except ValueError:
            continue
        if width <= 0 or height <= 0:
            continue
        if what == "yes":
            image.append(Mark(x, y, width, height, answer="yes", kind=suggested, kind_checked=False))
        elif what == "no":
            image.append(Mark(x, y, width, height, answer="no"))
        elif what.isalpha():  # A kind: "monster 1012 540 80 120".
            image.append(Mark(x, y, width, height, answer="yes", kind=what))
        else:
            try:
                guess = float(what)
            except ValueError:
                continue
            if 0.0 <= guess <= 1.0:
                image.append(Mark(x, y, width, height, guess=guess, kind=suggested, second_kind=second_kind,
                                  second_guess=second_guess, kind_guess=kind_guess))
    return marks


def write_guesses(path: Path, guesses: dict[str, list[Mark]], comment: str) -> None:
    """Writes `guesses` (marks with a guess) to `path`, images in name order, replacing the file in one step."""
    lines = ["# " + line for line in comment.splitlines()]
    for name in sorted(guesses):
        lines.append(f"image {name}")
        for mark in guesses[name]:
            kinds = ""  # With kinds: the surest and how sure of it, and the next if known.
            if mark.kind:
                kinds = f"{mark.kind} {mark.guess if mark.kind_guess is None else mark.kind_guess:.3f} "
                if mark.second_kind and mark.second_guess is not None:
                    kinds += f"{mark.second_kind} {mark.second_guess:.3f} "
            lines.append(f"{mark.guess:.3f} {kinds}{mark.x} {mark.y} {mark.width} {mark.height}")
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
