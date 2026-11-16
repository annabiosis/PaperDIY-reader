"""Compares text rendering methods and sizes for pixel-grid friendliness.

For every pixel size it renders the same sample with
  * grayscale antialiasing thresholded at 50% (what the firmware atlas does today),
  * FreeType's monochrome hinting target (stems snapped to whole pixels),
and reports how much ink is lost by the threshold and how uniform the stems are.
"""
from __future__ import annotations

import sys

import freetype
from PIL import Image, ImageDraw, ImageFont

SAMPLE = "Нейромант квартира 1234"
FONTS = [
    ("PT Serif", "tools/fonts/src/PTF55F.ttf"),
    ("JetBrains Mono", "tools/fonts/src/JetBrainsMono-Regular.ttf"),
]
SIZES = list(range(20, 37))


def ink_of_gray(path: str, size: int) -> tuple[int, int, int]:
    """Returns (ink after 50% threshold, gray mass, advance width)."""
    font = ImageFont.truetype(path, size)
    width = 420
    height = size * 3
    image = Image.new("L", (width, height), 0)
    draw = ImageDraw.Draw(image)
    draw.text((4, size * 2), SAMPLE, font=font, fill=255, anchor="ls")
    pixels = image.load()
    ink = 0
    mass = 0
    for y in range(height):
        for x in range(width):
            value = pixels[x, y]
            if value >= 128:
                ink += 1
            mass += value
    advance = font.getlength(SAMPLE)
    return ink, mass, advance


def ink_of_mono(path: str, size: int) -> tuple[int, int]:
    """Renders with FT_LOAD_TARGET_MONO (hinted, 1 bit). Returns (ink, advance)."""
    face = freetype.Face(path)
    face.set_pixel_sizes(0, size)
    ink = 0
    for ch in SAMPLE:
        face.load_char(ch, freetype.FT_LOAD_TARGET_MONO | freetype.FT_LOAD_RENDER)
        bitmap = face.glyph.bitmap
        pitch = bitmap.pitch
        width = bitmap.width
        rows = bitmap.rows
        buffer = bitmap.buffer
        for row in range(rows):
            for col in range(width):
                byte = buffer[row * pitch + (col >> 3)]
                if byte & (0x80 >> (col & 7)):
                    ink += 1
    advance = 0
    for ch in SAMPLE:
        face.load_char(ch, freetype.FT_LOAD_TARGET_MONO)
        advance += face.glyph.advance.x >> 6
    return ink, int(advance)


def stems(mono_rows: list[list[int]]) -> dict[int, int]:
    """Run-length histogram of horizontal ink runs (stroke widths)."""
    histogram: dict[int, int] = {}
    for row in mono_rows:
        run = 0
        for value in row:
            if value:
                run += 1
            elif run:
                histogram[run] = histogram.get(run, 0) + 1
                run = 0
        if run:
            histogram[run] = histogram.get(run, 0) + 1
    return histogram


def mono_bitmap(path: str, size: int, text: str) -> tuple[list[list[int]], int]:
    face = freetype.Face(path)
    face.set_pixel_sizes(0, size)
    rows: list[list[int]] = []
    pen_x = 0
    height = size * 2
    for ch in text:
        face.load_char(ch, freetype.FT_LOAD_TARGET_MONO | freetype.FT_LOAD_RENDER)
        slot = face.glyph
        bitmap = slot.bitmap
        left = slot.bitmap_left
        top = slot.bitmap_top
        pitch = bitmap.pitch
        buffer = bitmap.buffer
        for row in range(bitmap.rows):
            y = top + row
            while len(rows) <= y:
                rows.append([])
            line = rows[y]
            need = max(len(line), pen_x + left + bitmap.width)
            line.extend([0] * (need - len(line)))
            for col in range(bitmap.width):
                if buffer[row * pitch + (col >> 3)] & (0x80 >> (col & 7)):
                    line[pen_x + left + col] = 1
        pen_x += slot.advance.x >> 6
    return rows, pen_x


def main() -> int:
    print(f"{'font':<16} {'px':>3} {'ink(thr)':>9} {'ink(mono)':>10} {'ratio':>6}  stems(mono) 1:2:3+")
    for name, path in FONTS:
        for size in SIZES:
            gray_ink, mass, advance = ink_of_gray(path, size)
            mono_ink, mono_advance = ink_of_mono(path, size)
            ratio = gray_ink / mono_ink if mono_ink else 0.0
            rows, _ = mono_bitmap(path, size, "нрао")
            hist = stems(rows)
            one = hist.get(1, 0)
            two = hist.get(2, 0)
            three = sum(count for width, count in hist.items() if width >= 3)
            print(f"{name:<16} {size:>3} {gray_ink:>9} {mono_ink:>10} {ratio:>6.2f}  {one:>4}:{two:<4}:{three:<4} "
                  f"adv {advance:.1f}/{mono_advance}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
