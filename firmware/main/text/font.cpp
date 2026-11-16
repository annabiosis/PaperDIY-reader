#include "text/font.h"

#include <cstring>

#include "core/utf8.h"

namespace reader {

extern const FontFaceData* const kFontAtlas[5][6];

namespace {

// Atlas columns, matching tools/fonts/gen_fonts.py.
constexpr int kColumnRegular = 0;
constexpr int kColumnBold = 1;
constexpr int kColumnItalic = 2;
constexpr int kColumnBoldItalic = 3;
constexpr int kColumnMono = 4;
constexpr int kColumnMonoBold = 5;

// Atlas rows: 0..2 are the UI sizes (14/18/24), 2..4 are the reading sizes
// (24/28/32) — the 24 px row is shared by both ladders.
// UI ladder: 14, 18, 24 px from the UI rows, plus the 28 and 32 px mono faces that
// already exist in the reading rows (used by dialogs and the loading screen).
constexpr int kUiToAtlas[5] = {0, 1, 2, 3, 4};
constexpr int kReadingToAtlas[3] = {2, 3, 4};

int clamp_reading(int size_index) {
    if (size_index < 0) return 0;
    if (size_index >= 3) return 2;
    return size_index;
}

} // namespace

bool is_invisible_codepoint(uint32_t cp) {
    switch (cp) {
    case 0x00AD: // soft hyphen
    case 0x200B: // zero width space
    case 0x200C: // zero width non-joiner
    case 0x200D: // zero width joiner
    case 0x200E: // LRM
    case 0x200F: // RLM
    case 0x2060: // word joiner
    case 0xFEFF: // BOM / zero width no-break space
        return true;
    default:
        return cp >= 0x202A && cp <= 0x202E;
    }
}

bool is_space_codepoint(uint32_t cp) {
    switch (cp) {
    case ' ':
    case '\t':
    case 0x00A0:
    case 0x1680:
    case 0x2000:
    case 0x2001:
    case 0x2002:
    case 0x2003:
    case 0x2004:
    case 0x2005:
    case 0x2006:
    case 0x2007:
    case 0x2008:
    case 0x2009:
    case 0x200A:
    case 0x202F:
    case 0x205F:
    case 0x3000:
        return true;
    default:
        return false;
    }
}

int font_size_count() { return 3; }

const FontFaceData& find_font(int size_index, Face face) {
    const int size = kReadingToAtlas[clamp_reading(size_index)];
    int column = kColumnRegular;
    switch (face) {
    case Face::Bold:
        column = kColumnBold;
        break;
    case Face::Italic:
        column = kColumnItalic;
        break;
    case Face::BoldItalic:
        column = kColumnBoldItalic;
        break;
    case Face::Mono:
        column = kColumnMono;
        break;
    default:
        column = kColumnRegular;
        break;
    }
    const FontFaceData* face_data = kFontAtlas[size][column];
    return face_data != nullptr ? *face_data : *kFontAtlas[0][kColumnRegular];
}

const FontFaceData& find_ui_font(int size_index, bool bold) {
    const int index = size_index < 0 ? 0 : (size_index > 4 ? 4 : size_index);
    const int size = kUiToAtlas[index];
    const FontFaceData* face_data = kFontAtlas[size][bold ? kColumnMonoBold : kColumnMono];
    return face_data != nullptr ? *face_data : *kFontAtlas[0][kColumnMono];
}

const FontFaceData& find_heading_font(int size_index, int level) {
    int index = clamp_reading(size_index) + (level <= 1 ? 2 : (level == 2 ? 1 : 0));
    if (index > 2) index = 2;
    return *kFontAtlas[kReadingToAtlas[index]][kColumnBold];
}

const Glyph* find_glyph(const FontFaceData& font, uint32_t codepoint) {
    int low = 0;
    int high = static_cast<int>(font.glyph_count) - 1;
    while (low <= high) {
        const int mid = (low + high) / 2;
        const Glyph& glyph = font.glyphs[mid];
        if (glyph.codepoint == codepoint) return &glyph;
        if (glyph.codepoint < codepoint) {
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }
    return nullptr;
}

namespace {

bool is_letter_codepoint(uint32_t cp) {
    if (cp < 0x80) return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
    if (cp >= 0x00C0 && cp <= 0x024F) return true; // Latin-1 supplement / extended
    if (cp >= 0x0370 && cp <= 0x03FF) return true; // Greek
    if (cp >= 0x0400 && cp <= 0x04FF) return true; // Cyrillic
    if (cp >= 0x0500 && cp <= 0x052F) return true; // Cyrillic supplement
    return false;
}

} // namespace

// Glyph used for a codepoint the atlas does not cover. Invisible formatting
// characters have no visual fallback; spaces use the space advance; missing
// letters become U+FFFD so a gap is visible; other symbols are drawn as a thin
// blank (PT Serif has no €/₽, and a row of "?" would be worse than whitespace).
const Glyph* glyph_for_display(const FontFaceData& font, uint32_t codepoint, int* fallback_advance) {
    if (const Glyph* glyph = find_glyph(font, codepoint)) return glyph;

    if (is_space_codepoint(codepoint)) {
        if (const Glyph* space = find_glyph(font, ' ')) return space;
        *fallback_advance = font.pixel_size / 2;
        return nullptr;
    }
    if (is_letter_codepoint(codepoint)) {
        if (const Glyph* replacement = find_glyph(font, 0xFFFD)) return replacement;
        if (const Glyph* question = find_glyph(font, '?')) return question;
    }
    *fallback_advance = font.pixel_size / 3;
    return nullptr;
}

int measure_text(const FontFaceData& font, const char* text, size_t len) {
    if (text == nullptr) return 0;
    int width = 0;
    size_t pos = 0;

    while (pos < len) {
        size_t used = 0;
        const uint32_t cp = utf8_decode(text + pos, len - pos, &used);
        pos += used == 0 ? 1 : used;
        if (cp == '\n' || cp == '\r') continue;
        if (cp == '\t') {
            const Glyph* space = find_glyph(font, ' ');
            width += 4 * (space != nullptr ? space->advance : font.pixel_size / 2);
            continue;
        }
        if (is_invisible_codepoint(cp)) continue;

        int fallback = 0;
        const Glyph* glyph = glyph_for_display(font, cp, &fallback);
        width += glyph != nullptr ? glyph->advance : fallback;
    }
    return width;
}

} // namespace reader
