"""Draws the observex logo and writes assets/observex.ico, assets/logo.png and assets/logo.svg.

The mark is an eye reduced to two shapes: a ring and a pupil glancing up and to the right, on a dark rounded tile.
Standard library only. Run: python tools/make_logo.py
"""

import math
import struct
import zlib
from pathlib import Path

ASSETS = Path(__file__).resolve().parent.parent / "assets"

# Shapes on a 0..1 canvas. Small icon sizes get thicker strokes so the mark stays readable.
TILE_INSET = 0.03
TILE_RADIUS = 0.23
RING_RADIUS = 0.25  # Centre line of the ring.
RING_WIDTH = 0.09
PUPIL_RADIUS = 0.11
PUPIL_OFFSET = 0.055  # Towards the upper right.

TILE_TOP = (0x24, 0x24, 0x2C)
TILE_BOTTOM = (0x13, 0x13, 0x17)
RING = (0xEE, 0xEE, 0xF2)
PUPIL = (0x8B, 0x7B, 0xFF)

ICON_SIZES = (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)


def rounded_square(x, y, inset, radius):
    half = 0.5 - inset
    qx = abs(x - 0.5) - (half - radius)
    qy = abs(y - 0.5) - (half - radius)
    outside = math.hypot(max(qx, 0.0), max(qy, 0.0))
    return outside + min(max(qx, qy), 0.0) - radius


def shapes(size):
    ring_width = max(RING_WIDTH, 1.8 / size)
    pupil_radius = max(PUPIL_RADIUS, 1.6 / size)
    pupil = (0.5 + PUPIL_OFFSET, 0.5 - PUPIL_OFFSET)
    return ring_width, pupil_radius, pupil


def render(size):
    """Returns rows of straight-alpha RGBA bytes."""
    ring_width, pupil_radius, (px, py) = shapes(size)
    rows = []
    for j in range(size):
        row = bytearray()
        y = (j + 0.5) / size
        t = y
        tile_color = tuple(TILE_TOP[i] + (TILE_BOTTOM[i] - TILE_TOP[i]) * t for i in range(3))
        for i in range(size):
            x = (i + 0.5) / size

            def cover(distance):
                return min(max(0.5 - distance * size, 0.0), 1.0)

            tile = cover(rounded_square(x, y, TILE_INSET, TILE_RADIUS))
            ring = cover(abs(math.hypot(x - 0.5, y - 0.5) - RING_RADIUS) - ring_width / 2)
            pupil = cover(math.hypot(x - px, y - py) - pupil_radius)

            # Paint the tile, then the ring and pupil over it (premultiplied).
            r, g, b = (c * tile for c in tile_color)
            a = tile
            for color, coverage in ((RING, ring * tile), (PUPIL, pupil * tile)):
                r = color[0] * coverage + r * (1 - coverage)
                g = color[1] * coverage + g * (1 - coverage)
                b = color[2] * coverage + b * (1 - coverage)
            if a > 0:
                r, g, b = r / a, g / a, b / a
            row += bytes((round(r), round(g), round(b), round(a * 255)))
        rows.append(bytes(row))
    return rows


def png(size, rows):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + row for row in rows)
    header = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def ico(images):
    header = struct.pack("<HHH", 0, 1, len(images))
    offset = len(header) + 16 * len(images)
    entries = b""
    for size, data in images:
        side = 0 if size >= 256 else size
        entries += struct.pack("<BBBBHHII", side, side, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    return header + entries + b"".join(data for _, data in images)


def svg():
    s = 256
    inset = TILE_INSET * s
    side = s - 2 * inset
    px, py = (0.5 + PUPIL_OFFSET) * s, (0.5 - PUPIL_OFFSET) * s
    hexa = lambda c: "#%02X%02X%02X" % c
    return f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {s} {s}" width="{s}" height="{s}">
  <defs>
    <linearGradient id="tile" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="{hexa(TILE_TOP)}"/>
      <stop offset="1" stop-color="{hexa(TILE_BOTTOM)}"/>
    </linearGradient>
  </defs>
  <rect x="{inset:g}" y="{inset:g}" width="{side:g}" height="{side:g}" rx="{TILE_RADIUS * s:g}" fill="url(#tile)"/>
  <circle cx="{s / 2:g}" cy="{s / 2:g}" r="{RING_RADIUS * s:g}" fill="none" stroke="{hexa(RING)}" stroke-width="{RING_WIDTH * s:g}"/>
  <circle cx="{px:g}" cy="{py:g}" r="{PUPIL_RADIUS * s:g}" fill="{hexa(PUPIL)}"/>
</svg>
"""


def main():
    ASSETS.mkdir(exist_ok=True)
    images = [(size, png(size, render(size))) for size in ICON_SIZES]
    (ASSETS / "observex.ico").write_bytes(ico(images))
    (ASSETS / "logo.png").write_bytes(dict(images)[256])
    (ASSETS / "logo.svg").write_text(svg(), encoding="utf-8", newline="\n")
    print("wrote", ", ".join(p.name for p in (ASSETS / "observex.ico", ASSETS / "logo.png", ASSETS / "logo.svg")))


if __name__ == "__main__":
    main()
