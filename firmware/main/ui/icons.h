// Icon sprites exported from the Figma set (24x24, 1-bit masks).
#pragma once

#include <cstddef>
#include <cstdint>

namespace reader {

struct IconData {
    const char* name;
    uint8_t width;
    uint8_t height;
    const uint8_t* pixels; // 0 or 255 per pixel, row-major
};

extern const IconData kIcons[];
extern const size_t kIconCount;

// Looks up an icon by name; returns nullptr when the name is unknown.
const IconData* find_icon(const char* name);

} // namespace reader
