#!/usr/bin/env python3
"""Generate 8-bit alpha bitmap font atlases for the e-reader.

Source: PT Serif (ParaType Free Font License) Regular / Bold / Italic / Bold Italic.
Output: firmware/main/text/atlas/atlas_<size>.cpp + atlas_catalog.cpp

Run:
  python tools/fonts/gen_fonts.py
"""

from __future__ import annotations

import argparse
import os
import sys

from PIL import Image, ImageDraw, ImageFont

try:
    import freetype
except ImportError:  # the mono renderer is optional, Pillow remains the fallback
    freetype = None

FACES = [
    ("regular", "PTF55F.ttf"),
    ("bold", "PTF75F.ttf"),
    ("italic", "PTF56F.ttf"),
    ("bold_italic", "PTF76F.ttf"),
    # UI chrome (file manager, status bars) uses a monospace face.
    ("mono", "JetBrainsMono-Regular.ttf"),
    ("mono_bold", "JetBrainsMono-Bold.ttf"),
]
# Atlas rows: UI sizes (14/18/24) first, then the three reading sizes (24/29/35).
SIZES = [14, 18, 24, 28, 32]
LINE_HEIGHT_FACTOR = 1.12

# Punctuation and symbols that appear in real books, plus ASCII and Cyrillic.
EXTRA = [
    0x00A0, 0x00A9, 0x00AB, 0x00AE, 0x00B0, 0x00B7, 0x00BB, 0x2013, 0x2014,
    0x2018, 0x2019, 0x201A, 0x201C, 0x201D, 0x201E, 0x2020, 0x2021, 0x2022,
    0x2026, 0x2030, 0x2039, 0x203A, 0x2103, 0x2116, 0x2122, 0x2190, 0x2191,
    0x2192, 0x2193, 0x2713, 0x2717, 0xFFFD,
]


def charset() -> list[int]:
    chars = set(range(0x20, 0x7F))          # ASCII
    chars.update(range(0x00A0, 0x0100))     # Latin-1: NBSP, dashes, quotes, ©, «», °
    chars.update(range(0x0400, 0x0460))     # Cyrillic
    chars.update(range(0x2010, 0x2070))     # general punctuation (dashes, quotes, bullets)
    chars.update(range(0x2190, 0x21A0))     # arrows
    chars.update(range(0x2500, 0x2580))     # box drawing / block elements
    chars.update(EXTRA)
    # Soft hyphen, zero-width marks and BOMs are handled by the text engine and
    # never reach the rasterizer.
    for invisible in (0x00AD, 0x200B, 0x200C, 0x200D, 0x200E, 0x200F, 0xFEFF):
        chars.discard(invisible)
    return sorted(chars)


def format_bytes(data: bytes, per_line: int = 24) -> str:
    lines = []
    for i in range(0, len(data), per_line):
        lines.append("    " + ",".join(str(b) for b in data[i : i + per_line]) + ",")
    return "\n".join(lines)


def render_face(font_path: str, size: int) -> tuple[list[tuple], bytes, int, int, int]:
    """Returns (glyph tuples, bitmap, ascent, descent, line_height).

    Glyphs are rasterised with FreeType's monochrome hinting target when
    available: stems snap to whole pixels, so the mask is already 1-bit and the
    e-ink panel gets pixel-perfect shapes instead of thresholded antialiasing
    (which loses up to 13% of the ink at some sizes).
    """
    if freetype is not None:
        return render_face_mono(font_path, size)
    return render_face_gray(font_path, size)


def render_face_mono(font_path: str, size: int) -> tuple[list[tuple], bytes, int, int, int]:
    face = freetype.Face(font_path)
    face.set_pixel_sizes(0, size)
    ascent = face.size.ascender >> 6
    descent = -(face.size.descender >> 6)
    line_height = int(round((ascent + descent) * LINE_HEIGHT_FACTOR))
    glyphs: list[tuple] = []
    bitmap = bytearray()
    for cp in charset():
        try:
            face.load_char(chr(cp), freetype.FT_LOAD_TARGET_MONO | freetype.FT_LOAD_RENDER)
        except Exception:
            continue
        slot = face.glyph
        bitmap_data = slot.bitmap
        width = bitmap_data.width
        rows = bitmap_data.rows
        advance = max(0, slot.advance.x >> 6)
        if width == 0 or rows == 0:
            glyphs.append((cp, len(bitmap), 0, 0, 0, 0, advance))
            continue
        pitch = bitmap_data.pitch
        buffer = bitmap_data.buffer
        for row in range(rows):
            for col in range(width):
                if buffer[row * pitch + (col >> 3)] & (0x80 >> (col & 7)):
                    bitmap.append(255)
                else:
                    bitmap.append(0)
        glyphs.append((cp, len(bitmap) - width * rows, width, rows, slot.bitmap_left,
                       -slot.bitmap_top, advance))
    return glyphs, bytes(bitmap), ascent, descent, line_height


def render_face_gray(font_path: str, size: int) -> tuple[list[tuple], bytes, int, int, int]:
    font = ImageFont.truetype(font_path, size)
    ascent, descent = font.getmetrics()
    line_height = int(round((ascent + descent) * LINE_HEIGHT_FACTOR))
    glyphs: list[tuple] = []
    bitmap = bytearray()
    pad = size * 2
    canvas_size = size * 4
    for cp in charset():
        ch = chr(cp)
        try:
            advance = max(0, int(round(font.getlength(ch))))
        except Exception:
            continue
        image = Image.new("L", (canvas_size, canvas_size), 0)
        draw = ImageDraw.Draw(image)
        draw.text((pad, pad), ch, font=font, fill=255, anchor="ls")
        bbox = image.getbbox()
        if bbox is None:
            glyphs.append((cp, len(bitmap), 0, 0, 0, 0, advance))
            continue
        left, top, right, bottom = bbox
        glyph = image.crop(bbox)
        glyphs.append(
            (cp, len(bitmap), right - left, bottom - top, left - pad, top - pad, advance)
        )
        bitmap.extend(glyph.tobytes())
    return glyphs, bytes(bitmap), ascent, descent, line_height


def main() -> int:
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    parser = argparse.ArgumentParser()
    parser.add_argument("--src", default=os.path.join(root, "tools", "fonts", "src"))
    parser.add_argument("--out", default=os.path.join(root, "firmware", "main", "text", "atlas"))
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)

    rendered: dict[tuple[int, str], tuple] = {}
    for size in SIZES:
        for family, filename in FACES:
            path = os.path.join(args.src, filename)
            if not os.path.exists(path):
                print(f"missing font: {path}", file=sys.stderr)
                return 1
            rendered[(size, family)] = render_face(path, size)

    total_bitmap = 0
    for size in SIZES:
        lines = [
            "// Generated by tools/fonts/gen_fonts.py - do not edit.",
            '#include "text/font.h"',
            "",
            "namespace reader {",
            "",
        ]
        for family, _ in FACES:
            glyphs, bitmap, ascent, descent, line_height = rendered[(size, family)]
            total_bitmap += len(bitmap)
            suffix = f"{size}_{family}"
            lines.append(f'const char kName_{suffix}[] = "PT Serif {size} {family}";')
            lines.append(f"const Glyph kGlyphs_{suffix}[] = {{")
            lines.extend("    {0x%04X, %d, %d, %d, %d, %d, %d}," % g for g in glyphs)
            lines.append("};")
            lines.append(f"const unsigned char kBitmap_{suffix}[] = {{")
            lines.append(format_bytes(bitmap) if bitmap else "    0,")
            lines.append("};")
            lines.append(
                f"extern const FontFaceData font_atlas_{suffix} = {{kName_{suffix}, {size}, "
                f"{ascent}, {descent}, {line_height}, {len(glyphs)}, "
                f"kGlyphs_{suffix}, kBitmap_{suffix}}};"
            )
            lines.append("")
            print(
                f"{size:>2} {family:<12} glyphs={len(glyphs):>3} "
                f"bitmap={len(bitmap)/1024:7.1f} KB ascent={ascent} descent={descent} line={line_height}"
            )
        lines.append("}  // namespace reader")
        lines.append("")
        with open(os.path.join(args.out, f"atlas_{size}.cpp"), "w", encoding="utf-8", newline="\n") as handle:
            handle.write("\n".join(lines))

    catalog = [
        "// Generated by tools/fonts/gen_fonts.py - do not edit.",
        '#include "text/font.h"',
        "",
        "namespace reader {",
        "",
    ]
    for size in SIZES:
        for family, _ in FACES:
            catalog.append(f"extern const FontFaceData font_atlas_{size}_{family};")
    catalog += [
        "",
        "// Face order: Regular, Bold, Italic, BoldItalic, Mono, MonoBold.",
        "extern const FontFaceData* const kFontAtlas[5][6] = {",
    ]
    for size in SIZES:
        catalog.append(
            "    {&font_atlas_%d_regular, &font_atlas_%d_bold, &font_atlas_%d_italic, "
            "&font_atlas_%d_bold_italic, &font_atlas_%d_mono, &font_atlas_%d_mono_bold},"
            % (size, size, size, size, size, size)
        )
    catalog += ["};", "", "}  // namespace reader", ""]
    with open(os.path.join(args.out, "atlas_catalog.cpp"), "w", encoding="utf-8", newline="\n") as handle:
        handle.write("\n".join(catalog))
    print(f"wrote {args.out} (3 sizes x 4 faces, total bitmap {total_bitmap/1024/1024:.2f} MB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
