#include "core/encoding.h"

#include "core/utf8.h"

namespace reader {

namespace {

// CP1251 0x80..0xFF -> Unicode. 0x98 is undefined and maps to itself.
const uint16_t kCp1251High[128] = {
    0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021, 0x20AC, 0x2030, 0x0409, 0x2039,
    0x040A, 0x040C, 0x040B, 0x040F, 0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x0098, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F, 0x00A0, 0x040E, 0x045E, 0x0408,
    0x00A4, 0x0490, 0x00A6, 0x00A7, 0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407,
    0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7, 0x0451, 0x2116, 0x0454, 0x00BB,
    0x0458, 0x0405, 0x0455, 0x0457, 0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417,
    0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E, 0x041F, 0x0420, 0x0421, 0x0422, 0x0423,
    0x0424, 0x0425, 0x0426, 0x0427, 0x0428, 0x0429, 0x042A, 0x042B, 0x042C, 0x042D, 0x042E, 0x042F,
    0x0430, 0x0431, 0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437, 0x0438, 0x0439, 0x043A, 0x043B,
    0x043C, 0x043D, 0x043E, 0x043F, 0x0440, 0x0441, 0x0442, 0x0443, 0x0444, 0x0445, 0x0446, 0x0447,
    0x0448, 0x0449, 0x044A, 0x044B, 0x044C, 0x044D, 0x044E, 0x044F,
};

uint16_t read_u16(const uint8_t* p, bool little_endian) {
    return little_endian ? static_cast<uint16_t>(p[0] | (p[1] << 8))
                         : static_cast<uint16_t>((p[0] << 8) | p[1]);
}

} // namespace

TextEncoding detect_encoding(const uint8_t* data, size_t size) {
    if (size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) return TextEncoding::Utf8;
    if (size >= 2 && data[0] == 0xFF && data[1] == 0xFE) return TextEncoding::Utf16Le;
    if (size >= 2 && data[0] == 0xFE && data[1] == 0xFF) return TextEncoding::Utf16Be;
    if (size == 0) return TextEncoding::Utf8;

    // XML declaration is a strong hint for FB2/EPUB sources.
    if (size >= 4) {
        size_t limit = size < 512 ? size : 512;
        std::string head(reinterpret_cast<const char*>(data), limit);
        if (head.find("encoding=\"windows-1251\"") != std::string::npos ||
            head.find("encoding='windows-1251'") != std::string::npos) {
            return TextEncoding::Cp1251;
        }
    }

    if (utf8_valid(data, size)) return TextEncoding::Utf8;
    return TextEncoding::Cp1251;
}

std::string cp1251_to_utf8(const uint8_t* data, size_t size) {
    std::string out;
    // CP1251 Cyrillic becomes two UTF-8 bytes, so reserve the worst case up front:
    // growing this buffer reallocates multi-megabyte strings and can abort.
    out.reserve(size * 2 + 16);
    for (size_t i = 0; i < size; ++i) {
        const uint8_t b = data[i];
        if (b < 0x80) {
            out.push_back(static_cast<char>(b));
        } else {
            utf8_append(out, kCp1251High[b - 0x80]);
        }
    }
    return out;
}

std::string utf16_to_utf8(const uint8_t* data, size_t size, bool little_endian) {
    std::string out;
    out.reserve(size);
    size_t pos = 0;
    if (size >= 2) {
        const uint16_t bom = read_u16(data, little_endian);
        if (bom == 0xFEFF) pos = 2;
    }
    while (pos + 1 < size) {
        uint32_t cp = read_u16(data + pos, little_endian);
        pos += 2;
        if (cp >= 0xD800 && cp <= 0xDBFF && pos + 1 < size) {
            const uint32_t low = read_u16(data + pos, little_endian);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                pos += 2;
            }
        }
        utf8_append(out, cp);
    }
    return out;
}

std::string to_utf8(const uint8_t* data, size_t size, TextEncoding encoding) {
    switch (encoding) {
    case TextEncoding::Utf8: {
        size_t skip = utf8_skip_bom(data, size);
        return std::string(reinterpret_cast<const char*>(data + skip), size - skip);
    }
    case TextEncoding::Cp1251:
        return cp1251_to_utf8(data, size);
    case TextEncoding::Utf16Le:
        return utf16_to_utf8(data, size, true);
    case TextEncoding::Utf16Be:
        return utf16_to_utf8(data, size, false);
    }
    return {};
}

const char* encoding_name(TextEncoding encoding) {
    switch (encoding) {
    case TextEncoding::Utf8:
        return "UTF-8";
    case TextEncoding::Cp1251:
        return "CP1251";
    case TextEncoding::Utf16Le:
        return "UTF-16LE";
    case TextEncoding::Utf16Be:
        return "UTF-16BE";
    }
    return "?";
}

} // namespace reader
