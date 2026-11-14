"""Cuts the anchors that models/ui/ui.txt lists out of clips (observex's own clip.mp4 files).

For each `anchor NAME MIN_SCORE W H from CLIP FIRST LAST X Y [from CLIP FIRST LAST X Y]...` line it reads frames
FIRST to LAST (1-based) of each recordings/clips/CLIP/clip.mp4 named, turns them grey as observex does ((blue * 29 +
green * 150 + red * 77) >> 8), and writes models/ui/NAME.pgm (the mean of the W x H area at X Y over all of them)
and NAME-mask.pgm (255 where it never changed by more than the video's own noise: the interface's drawing; 0 where
the world showed through). Use frames where the piece stays put and the camera moves, or several places with
different backgrounds, so the world under it changes. Also writes models/ui/anchors.png to look them over.

A place may end with `unlike FIRST LAST`: frames of the same clip before the piece showed (the cast bar, which shows
only while casting, over a camera that hardly moves); a pixel within SAME_GREY of what was there then is the world's,
so it is left out of the mask.

    python tools/ui/make_anchors.py [--ui models/ui] [--clips recordings/clips]
"""
import argparse
import pathlib

import cv2
import numpy as np

STEADY = 6.0  # Standard deviation over a place's frames (grey levels) up to which a pixel counts as the interface's own.
SAME = 0.4  # With two places or more: how far apart (in standard deviations of the patch) a pixel may be between them,
SAME_GREY = 12.0  # or how far apart in grey levels.


def grey(frame):
    b, g, r = (frame[..., i].astype(np.uint32) for i in range(3))
    return ((b * 29 + g * 150 + r * 77) >> 8).astype(np.float64)


def write_pgm(path, pixels):
    h, w = pixels.shape
    with open(path, "wb") as out:
        out.write(b"P5\n%d %d\n255\n" % (w, h))
        out.write(pixels.astype(np.uint8).tobytes())


def main():
    here = pathlib.Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser()
    parser.add_argument("--ui", default=str(here / "models" / "ui"))
    parser.add_argument("--clips", default=str(here / "recordings" / "clips"))
    args = parser.parse_args()
    ui = pathlib.Path(args.ui)

    sizes = {}  # name -> (w, h), in file order
    cuts = {}  # (clip, first, last) -> [(name, place, x, y)]; place ("unlike", n) for place n's frames before
    places = {}  # name -> one slot per place
    unlikes = {}  # name -> the places that have unlike frames
    for line in (ui / "ui.txt").read_text(encoding="utf-8").splitlines():
        words = line.split()
        if len(words) < 11 or words[0] != "anchor" or words[5] != "from":
            continue
        name, w, h = words[1], int(words[3]), int(words[4])
        sizes[name] = (w, h)
        at = 5
        while at < len(words):
            if words[at] != "from" or at + 6 > len(words):
                raise SystemExit(f"{name}: expected from CLIP FIRST LAST X Y [unlike FIRST LAST]")
            clip = words[at + 1]
            first, last, x, y = map(int, words[at + 2:at + 6])
            place = len(places.setdefault(name, []))
            places[name].append(None)
            cuts.setdefault((clip, first, last), []).append((name, place, x, y))
            at += 6
            if at < len(words) and words[at] == "unlike":
                before_first, before_last = int(words[at + 1]), int(words[at + 2])
                cuts.setdefault((clip, before_first, before_last), []).append((name, ("unlike", place), x, y))
                unlikes.setdefault(name, set()).add(place)
                at += 3

    sums = {name: [[0.0, 0.0, 0] for _ in places[name]] for name in sizes}
    before = {name: {place: [0.0, 0.0, 0] for place in unlikes.get(name, ())} for name in sizes}
    for (clip, first, last), anchors in cuts.items():
        video = cv2.VideoCapture(str(pathlib.Path(args.clips) / clip / "clip.mp4"))
        index = 0
        while True:
            ok, frame = video.read()
            if not ok or index >= last:
                break
            index += 1
            if index < first:
                continue
            g = grey(frame)
            for name, place, x, y in anchors:
                w, h = sizes[name]
                part = g[y:y + h, x:x + w]
                s = before[name][place[1]] if isinstance(place, tuple) else sums[name][place]
                s[0] = s[0] + part
                s[1] = s[1] + part * part
                s[2] += 1
    sheet = []
    for name, (w, h) in sizes.items():
        means, steady, frames = [], np.ones((h, w), bool), 0
        for place, (total, squares, count) in enumerate(sums[name]):
            if count == 0:
                raise SystemExit(f"{name}: no frames found at one of its places")
            mean = total / count
            std = np.sqrt(np.maximum(squares / count - mean * mean, 0.0))
            steady &= std <= STEADY
            if place in before[name]:
                total_before, _, count_before = before[name][place]
                if count_before == 0:
                    raise SystemExit(f"{name}: no unlike frames found at one of its places")
                steady &= np.abs(mean - total_before / count_before) > SAME_GREY
            means.append(mean)
            frames += count
        if len(means) > 1:
            # The same drawing at every place, its brightness evened out: the world behind it differs between them.
            # Or the same grey: evening out moves a bright line where the patch's contrast differs between places
            # (the chat's corner lost its frame line, 2026-11-12, its panel paler in one clip).
            z = [(m - m.mean()) / max(m.std(), 1e-6) for m in means]
            middle = np.mean(z, axis=0)
            evened = np.max([np.abs(each - middle) for each in z], axis=0) <= SAME
            steady &= evened | (np.max(means, axis=0) - np.min(means, axis=0) <= SAME_GREY)
        mask = steady.astype(np.uint8) * 255
        write_pgm(ui / f"{name}.pgm", np.round(np.mean(means, axis=0)))
        write_pgm(ui / f"{name}-mask.pgm", mask)
        print(f"{name}: {w}x{h} from {frames} frames at {len(means)} place(s), {mask.mean() / 2.55:.0f}% of it steady")
        sheet.append((name, np.round(np.mean(means, axis=0)).astype(np.uint8), mask))

    # A sheet to look them over: each anchor 4x, its pixels and beside it the mask.
    tiles = []
    for name, pixels, mask in sheet:
        big = lambda a: cv2.resize(a, None, fx=4, fy=4, interpolation=cv2.INTER_NEAREST)
        tile = np.hstack([big(pixels), np.full((pixels.shape[0] * 4, 8), 128, np.uint8), big(mask)])
        canvas = np.full((tile.shape[0] + 22, max(tile.shape[1], 200)), 40, np.uint8)
        canvas[22:, :tile.shape[1]] = tile
        cv2.putText(canvas, name, (2, 16), cv2.FONT_HERSHEY_SIMPLEX, 0.5, 255, 1)
        tiles.append(canvas)
    width = max(t.shape[1] for t in tiles)
    rows = [np.pad(t, ((0, 8), (0, width - t.shape[1])), constant_values=20) for t in tiles]
    cv2.imwrite(str(ui / "anchors.png"), np.vstack(rows))


if __name__ == "__main__":
    main()
