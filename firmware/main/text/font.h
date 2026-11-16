// Bitmap font atlas: flash-resident glyphs rendered as 8-bit alpha.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "text/types.h"

namespace reader {

struct Glyph {
    uint32_t codepoint;
    uint32_t bitmap_offset; // into FontFaceData::bitmap
    uint16_t width;
    uint16_t height;
    int16_t bearing_x; // pen x -> bitmap left
    int16_t bearing_y; // pen y (baseline) -> bitmap top (negative = above)
    uint8_t advance;
};

struct FontFaceData {
    const char* name;
    uint8_t pixel_size;
    int8_t ascent;  // px above baseline
    int8_t descent; // px below baseline
    uint8_t line_height;
    uint16_t glyph_count;
    const Glyph* glyphs;   // sorted by codepoint
    const uint8_t* bitmap; // width*height bytes per glyph, 8-bit coverage
};

// Number of body sizes exposed to the user (S/M/L). Headings use a larger face
// when available.
int font_size_count();

// Face for a body size index (0..font_size_count()-1) and style. Never null.
const FontFaceData& find_font(int size_index, Face face);

// Monospace face used by UI chrome (file manager rows, status bars).
const FontFaceData& find_ui_font(int size_index, bool bold = false,
                                 bool italic = false);

// Face for a heading level (1..6) at a body size; clamped to available sizes.
const FontFaceData& find_heading_font(int size_index, int level);

const Glyph* find_glyph(const FontFaceData& font, uint32_t codepoint);

// Formatting characters that must never be rendered (soft hyphen, ZWSP, BOM…).
bool is_invisible_codepoint(uint32_t codepoint);

// Space-like characters (NBSP, thin space, ideographic space…).
bool is_space_codepoint(uint32_t codepoint);

// Glyph to draw for a codepoint: the real glyph, a space, or U+FFFD. Returns
// nullptr only when the font has no replacement either (see `fallback_advance`).
const Glyph* glyph_for_display(const FontFaceData& font, uint32_t codepoint, int* fallback_advance);

// Advance width of UTF-8 text in pixels (no kerning).
int measure_text(const FontFaceData& font, const char* text, size_t len);
inline int measure_text(const FontFaceData& font, const std::string& text) {
    return measure_text(font, text.data(), text.size());
}

} // namespace reader
