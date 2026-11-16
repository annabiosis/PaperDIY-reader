#include "ui/draw.h"

#include <algorithm>

#include "core/utf8.h"

namespace reader::ui {

namespace {

const Glyph* glyph_for(const FontFaceData& font, uint32_t codepoint, int* fallback_advance) {
    return glyph_for_display(font, codepoint, fallback_advance);
}


} // namespace

uint16_t level_color(uint8_t level) {
    const uint8_t value = static_cast<uint8_t>((level > 15 ? 15 : level) * 16 + 8);
    return static_cast<uint16_t>(((value >> 3) << 11) | ((value >> 2) << 5) | (value >> 3));
}


void fill_dither(M5GFX& gfx, int x, int y, int w, int h, int period) {
    if (w <= 0 || h <= 0) return;
    if (period < 2) period = 2;
    constexpr int kRows = 16;
    uint8_t buffer[540 * kRows];
    const int width = w > 540 ? 540 : w;
    for (int row = 0; row < kRows; ++row) {
        for (int col = 0; col < width; ++col) {
            // Single dot in each tile keeps the band light and the text readable.
            const bool dot = ((col % period) == 0) && ((row % period) == 0);
            // 255 maps to the *foreground* colour, which is black here.
            buffer[row * width + col] = dot ? 255 : 0;
        }
    }
    int done = 0;
    while (done < h) {
        const int rows = (h - done) < kRows ? (h - done) : kRows;
        gfx.pushGrayscaleImage(x, y + done, width, rows, buffer, m5gfx::grayscale_8bit, TFT_BLACK,
                               TFT_WHITE);
        done += rows;
    }
}

void fill_level(M5GFX& gfx, int x, int y, int w, int h, uint8_t level) {
    if (w <= 0 || h <= 0) return;
    const uint8_t value = static_cast<uint8_t>((level > 15 ? 15 : level) * 16 + 8);
    // The grayscale push treats the buffer as coverage between black and white,
    // which lands exactly on a nibble (fillRect would dither between two levels).
    constexpr int kRows = 8;
    uint8_t buffer[540 * kRows];
    const int width = w > 540 ? 540 : w;
    for (int i = 0; i < width * kRows; ++i) buffer[i] = value;
    int done = 0;
    while (done < h) {
        const int rows = (h - done) < kRows ? (h - done) : kRows;
        gfx.pushGrayscaleImage(x, y + done, width, rows, buffer, m5gfx::grayscale_8bit, TFT_WHITE, TFT_BLACK);
        done += rows;
    }
}

int draw_text(M5GFX& gfx, int x, int baseline_y, const std::string& text, const FontFaceData& font,
              const TextOptions& options) {
    int pen = x;
    const int limit = options.max_width >= 0 ? x + options.max_width : INT32_MAX;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t used = 0;
        const uint32_t cp = utf8_decode(text.data() + pos, text.size() - pos, &used);
        pos += used == 0 ? 1 : used;
        if (cp == '\n' || cp == '\r') continue;
        if (is_invisible_codepoint(cp)) continue;
        if (cp == '\t') {
            const Glyph* space = find_glyph(font, ' ');
            pen += 4 * (space != nullptr ? space->advance : font.pixel_size / 2);
            continue;
        }
        int fallback = 0;
        const Glyph* glyph = glyph_for(font, cp, &fallback);
        const int advance = glyph != nullptr ? glyph->advance : fallback;
        if (pen + advance > limit) break;
        if (glyph != nullptr && glyph->width > 0 && glyph->height > 0) {
            // The atlas is generated with FreeType's mono hinting target, so the masks
            // are already 0/255; the old per-glyph threshold copy is gone.
            const uint8_t* pixels = font.bitmap + glyph->bitmap_offset;
            gfx.pushGrayscaleImage(pen + glyph->bearing_x, baseline_y + glyph->bearing_y, glyph->width,
                                   glyph->height, pixels, m5gfx::grayscale_8bit, options.fg, options.bg);
        }
        pen += advance;
    }
    return pen - x;
}

std::string elide(const FontFaceData& font, const std::string& text, int max_width) {
    if (measure_text(font, text) <= max_width) return text;

    const std::string dots = "…";
    const int dots_width = measure_text(font, dots);
    std::string result;
    int width = 0;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t used = 0;
        const uint32_t cp = utf8_decode(text.data() + pos, text.size() - pos, &used);
        if (is_invisible_codepoint(cp)) {
            pos += used == 0 ? 1 : used;
            continue;
        }
        int fallback = 0;
        const Glyph* glyph = glyph_for(font, cp, &fallback);
        const int advance = glyph != nullptr ? glyph->advance : fallback;
        if (width + advance + dots_width > max_width) break;
        result.append(text, pos, used);
        width += advance;
        pos += used == 0 ? 1 : used;
    }
    result += dots;
    return result;
}

void draw_battery(M5GFX& gfx, int right_x, int baseline_y, int percent, uint16_t color, uint16_t bg) {
    const int width = 34;
    const int height = 16;
    const int x = right_x - width;
    const int y = baseline_y - height + 3;

    gfx.fillRect(x, y, width, height, bg);
    gfx.drawRect(x, y, width, height, color);
    gfx.fillRect(x + width, y + 4, 3, height - 8, color);

    if (percent < 0) {
        gfx.drawFastHLine(x + 4, y + height / 2, width - 8, color);
        return;
    }
    const int inner = width - 6;
    const int filled = std::max(0, std::min(inner, inner * percent / 100));
    if (filled > 0) gfx.fillRect(x + 3, y + 3, filled, height - 6, color);
    if (percent <= 15) {
        // Low battery: draw a small cross so it is visible at a glance.
        gfx.drawLine(x + width / 2 - 3, y + 4, x + width / 2 + 3, y + height - 4, color);
        gfx.drawLine(x + width / 2 + 3, y + 4, x + width / 2 - 3, y + height - 4, color);
    }
}

} // namespace reader::ui
