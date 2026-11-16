#!/usr/bin/env python3
"""Builds a 16-step grayscale palette for design mockups.

The M5PaperDIY panel frame buffer is 4 bits per pixel: exactly 16 levels.
This script writes the same 16 steps in the formats design tools expect:

  gray16.gpl  GIMP / Krita / Aseprite palette
  gray16.act  Photoshop / Affinity swatch table
  gray16.png  labelled swatch strip (540x960, the panel size)
  gray16-hex.txt  plain list for copy-paste into Figma tokens / CSS
"""

from __future__ import annotations

import os

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "docs", "palette")
LEVELS = 16


def value(level: int) -> int:
    """8-bit linear gray for a level: 0, 17, 34 ... 255 (0x00, 0x11, ... 0xFF)."""
    return round(level * 255 / (LEVELS - 1))


def driver_value(level: int) -> int:
    """Value to push through the grayscale API so the nibble is exactly `level`."""
    return level * 16 + 8


def rgb565(level: int) -> tuple[int, int]:
    v = value(level)
    r5 = round(v * 31 / 255)
    g6 = round(v * 63 / 255)
    b5 = r5
    return ((r5 << 11) | (g6 << 5) | b5), v


def write_gpl(path: str) -> None:
    lines = ["GIMP Palette", "Name: PaperDIY Gray16", "Columns: 4", "#"]
    for level in range(LEVELS):
        v = value(level)
        lines.append(f"{v:3d} {v:3d} {v:3d}\tGray {level:02d} ({v:02X})")
    with open(path, "w", encoding="ascii", newline="\n") as handle:
        handle.write("\n".join(lines) + "\n")


def write_act(path: str) -> None:
    # Adobe Color Table: 256 RGB triples; we fill 16 grays and leave the rest black.
    data = bytearray()
    for level in range(LEVELS):
        v = value(level)
        data += bytes((v, v, v))
    data += bytes((256 - LEVELS) * 3)
    data += (LEVELS).to_bytes(2, "big") + b"\x00\x00"
    with open(path, "wb") as handle:
        handle.write(data)


def write_hex(path: str) -> None:
    rows = ["level  hex      css                driver(16n+8)"]
    for level in range(LEVELS):
        v = value(level)
        rows.append(
            f"{level:5d}  #{v:02X}{v:02X}{v:02X}  rgb({v}, {v}, {v})  {driver_value(level):3d}"
        )
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("\n".join(rows) + "\n")


def write_png(path: str) -> None:
    width, height = 540, 960
    band = height // LEVELS
    image = Image.new("L", (width, height), 255)
    draw = ImageDraw.Draw(image)
    try:
        font = ImageFont.truetype(os.path.join(ROOT, "tools", "fonts", "src", "JetBrainsMono-Regular.ttf"), 20)
    except Exception:
        font = ImageFont.load_default()

    for level in range(LEVELS):
        v = value(level)
        top = level * band
        draw.rectangle((0, top, width - 1, top + band - 1), fill=v)
        text_color = 0 if v > 127 else 255
        draw.text((12, top + band // 2 - 10), f"{level:2d}  #{v:02X}{v:02X}{v:02X}  {level * 100 // 15:3d}%",
                  fill=text_color, font=font)
    image.save(path)


def main() -> int:
    os.makedirs(OUT, exist_ok=True)
    write_gpl(os.path.join(OUT, "gray16.gpl"))
    write_act(os.path.join(OUT, "gray16.act"))
    write_hex(os.path.join(OUT, "gray16-hex.txt"))
    write_png(os.path.join(OUT, "gray16.png"))
    for name in sorted(os.listdir(OUT)):
        print(f"wrote {os.path.join(OUT, name)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
