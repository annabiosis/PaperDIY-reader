// Minimal UTF-8 helpers (no locale, no exceptions).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace reader {

constexpr uint32_t kReplacementChar = 0xFFFD;

// Decodes one codepoint from `s[0..len)`. Returns codepoint, stores byte length
// in `consumed`. Invalid sequences return kReplacementChar with consumed == 1.
uint32_t utf8_decode(const char* s, size_t len, size_t* consumed);

inline uint32_t utf8_decode(const std::string& s, size_t pos, size_t* consumed) {
    return utf8_decode(s.data() + pos, s.size() - pos, consumed);
}

// Appends codepoint encoded as UTF-8.
void utf8_append(std::string& out, uint32_t codepoint);

// Number of codepoints; invalid bytes count as one.
uint32_t utf8_length(const std::string& text);

// Byte offset of the `chars`-th codepoint (clamped to size()).
size_t utf8_offset_of(const std::string& text, uint32_t chars);

// True when the buffer is valid UTF-8 (allows ASCII).
bool utf8_valid(const uint8_t* data, size_t size);

// Drops a leading UTF-8 BOM if present.
size_t utf8_skip_bom(const uint8_t* data, size_t size);

} // namespace reader
