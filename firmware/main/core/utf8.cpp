#include "core/utf8.h"

#include <cstring>

namespace reader {

uint32_t utf8_decode(const char* s, size_t len, size_t* consumed) {
    if (len == 0 || s == nullptr) {
        if (consumed) *consumed = 0;
        return 0;
    }
    const auto* p = reinterpret_cast<const uint8_t*>(s);
    const uint8_t b0 = p[0];
    *consumed = 1;
    if (b0 < 0x80) return b0;

    size_t need = 0;
    uint32_t cp = 0;
    if ((b0 & 0xE0) == 0xC0) {
        need = 1;
        cp = b0 & 0x1F;
    } else if ((b0 & 0xF0) == 0xE0) {
        need = 2;
        cp = b0 & 0x0F;
    } else if ((b0 & 0xF8) == 0xF0) {
        need = 3;
        cp = b0 & 0x07;
    } else {
        return kReplacementChar;
    }
    if (len < need + 1) return kReplacementChar;

    for (size_t i = 1; i <= need; ++i) {
        const uint8_t b = p[i];
        if ((b & 0xC0) != 0x80) return kReplacementChar;
        cp = (cp << 6) | (b & 0x3F);
    }
    // Reject overlong encodings and out-of-range values.
    if ((need == 1 && cp < 0x80) || (need == 2 && cp < 0x800) || (need == 3 && cp < 0x10000) ||
        cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        return kReplacementChar;
    }
    *consumed = need + 1;
    return cp;
}

void utf8_append(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

uint32_t utf8_length(const std::string& text) {
    uint32_t count = 0;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t used = 0;
        utf8_decode(text.data() + pos, text.size() - pos, &used);
        pos += used == 0 ? 1 : used;
        ++count;
    }
    return count;
}

size_t utf8_offset_of(const std::string& text, uint32_t chars) {
    size_t pos = 0;
    uint32_t seen = 0;
    while (pos < text.size() && seen < chars) {
        size_t used = 0;
        utf8_decode(text.data() + pos, text.size() - pos, &used);
        pos += used == 0 ? 1 : used;
        ++seen;
    }
    return pos;
}

bool utf8_valid(const uint8_t* data, size_t size) {
    size_t pos = 0;
    while (pos < size) {
        const uint8_t b = data[pos];
        size_t need = 0;
        if (b < 0x80) {
            need = 0;
        } else if ((b & 0xE0) == 0xC0) {
            need = 1;
        } else if ((b & 0xF0) == 0xE0) {
            need = 2;
        } else if ((b & 0xF8) == 0xF0) {
            need = 3;
        } else {
            return false;
        }
        if (pos + need >= size) return false;
        for (size_t i = 1; i <= need; ++i) {
            if ((data[pos + i] & 0xC0) != 0x80) return false;
        }
        pos += need + 1;
    }
    return true;
}

size_t utf8_skip_bom(const uint8_t* data, size_t size) {
    if (size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) return 3;
    return 0;
}

} // namespace reader
