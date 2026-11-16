// Low-level drawing helpers for the reader UI.
#pragma once

#include <M5GFX.h>

#include <string>

#include "text/font.h"

namespace reader::ui {

struct TextOptions {
    uint16_t fg = TFT_BLACK;
    uint16_t bg = TFT_WHITE;
    int max_width = -1; // -1 = unlimited
};


// Fills a rectangle with an exact panel level (0..15). fillRect() goes through
// RGB565 and lands on a neighbouring level, which would show as a different gray
// than the mockup's #DDDDDD (level 13).
void fill_level(M5GFX& gfx, int x, int y, int w, int h, uint8_t level);

// RGB565 colour of one panel level, for drawing text on a shaded strip.
uint16_t level_color(uint8_t level);

// Dithered band: one black pixel per `period`-pixel tile. DU supports only black and
// white, so a "translucent" shade is a dot pattern: drawn over text it lets the text
// show through, which solid grey cannot do on this panel.
void fill_dither(M5GFX& gfx, int x, int y, int w, int h, int period);

// Draws UTF-8 text with its baseline at `baseline_y`; returns the width used.
int draw_text(M5GFX& gfx, int x, int baseline_y, const std::string& text, const FontFaceData& font,
              const TextOptions& options = {});

// Cuts text with an ellipsis so it fits `max_width` pixels.
std::string elide(const FontFaceData& font, const std::string& text, int max_width);

// Battery indicator: small frame with a fill level; `right_x` is the right edge.
void draw_battery(M5GFX& gfx, int right_x, int baseline_y, int percent, uint16_t color, uint16_t bg);

} // namespace reader::ui
