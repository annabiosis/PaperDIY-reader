// Icon drawing: 1-bit masks tinted at draw time, so the same sprite works for
// black text in DU and for gray decorations in GC16.
#pragma once

#include <M5GFX.h>

#include <cstdint>

#include "ui/icons.h"

namespace reader::ui {

// Draws the icon with its top-left corner at (x, y). Returns false when the name
// is unknown (nothing is drawn then).
bool draw_icon(M5GFX& gfx, const char* name, int x, int y, uint16_t color, uint16_t background = TFT_WHITE);

// Icon size, or 0 when unknown.
int icon_width(const char* name);
int icon_height(const char* name);

} // namespace reader::ui
