#include "ui/icon_draw.h"

#include <cstring>

namespace reader {

const IconData* find_icon(const char* name) {
    if (name == nullptr) return nullptr;
    for (size_t i = 0; i < kIconCount; ++i) {
        if (std::strcmp(kIcons[i].name, name) == 0) return &kIcons[i];
    }
    return nullptr;
}

} // namespace reader

namespace reader::ui {

bool draw_icon(M5GFX& gfx, const char* name, int x, int y, uint16_t color, uint16_t background) {
    const IconData* icon = find_icon(name);
    if (icon == nullptr) return false;
    gfx.pushGrayscaleImage(x, y, icon->width, icon->height, icon->pixels, m5gfx::grayscale_8bit, color,
                           background);
    return true;
}

int icon_width(const char* name) {
    const IconData* icon = find_icon(name);
    return icon != nullptr ? icon->width : 0;
}

int icon_height(const char* name) {
    const IconData* icon = find_icon(name);
    return icon != nullptr ? icon->height : 0;
}

} // namespace reader::ui
