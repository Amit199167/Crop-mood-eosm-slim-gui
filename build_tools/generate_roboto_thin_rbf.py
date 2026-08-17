#!/usr/bin/env python3
"""Create the compact Roboto Thin bitmap font used by Slim-only UI screens.

The camera's RBF renderer is monochrome.  This generator deliberately keeps
Roboto Thin's light forms, but uses a low antialias threshold so one-pixel
strokes survive the EOS M LCD.
"""

from pathlib import Path
import struct
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "data" / "fonts" / "source" / "roboto-thin.ttf"
OUTPUT = ROOT / "data" / "fonts" / "roboto-thin.rbf"

FIRST = 32
LAST = 126
HEIGHT = 40
PIXEL_SIZE = 40
MAX_WIDTH = 48
THRESHOLD = 72


def glyph(font, character):
    """Return a 1-bit RBF glyph, its camera-side advance, and required width."""
    advance = max(1, round(font.getlength(character)))
    bbox = font.getbbox(character)
    right = max(advance, bbox[2] if bbox else 0)
    width = min(MAX_WIDTH, max(1, right + 2))

    image = Image.new("L", (MAX_WIDTH, HEIGHT), 0)
    draw = ImageDraw.Draw(image)
    # Roboto's 40 px bbox begins roughly eight pixels below the anchor.  Lift
    # it into the 40 px RBF cell so capitals and descenders share one baseline.
    draw.text((0, -8), character, font=font, fill=255)

    rows = bytearray(((MAX_WIDTH + 7) // 8) * HEIGHT)
    bytes_per_row = (MAX_WIDTH + 7) // 8
    pixels = image.load()
    for y in range(HEIGHT):
        for x in range(MAX_WIDTH):
            if pixels[x, y] >= THRESHOLD:
                # RBF uses least-significant-bit first inside each byte.
                rows[y * bytes_per_row + x // 8] |= 1 << (x % 8)
    return rows, min(MAX_WIDTH, advance), width


def main():
    font = ImageFont.truetype(str(SOURCE), PIXEL_SIZE)
    bytes_per_row = (MAX_WIDTH + 7) // 8
    char_size = bytes_per_row * HEIGHT
    count = LAST - FIRST + 1
    cmap_offset = 0x74 + count

    advances = []
    cmap = bytearray()
    for codepoint in range(FIRST, LAST + 1):
        data, advance, _ = glyph(font, chr(codepoint))
        advances.append(advance)
        cmap.extend(data)

    name = b"Roboto Thin EOSM"
    name += b"\0" * (64 - len(name))
    header = struct.pack(
        "<II64s11i",
        0x0DF00EE0, 0x00000003, name,
        char_size, PIXEL_SIZE, HEIGHT, MAX_WIDTH,
        FIRST, LAST, 0,
        0x74, cmap_offset,
        0, HEIGHT,
    )
    assert len(header) == 0x74
    OUTPUT.write_bytes(header + bytes(advances) + cmap)
    print(f"wrote {OUTPUT} ({OUTPUT.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
