// Legacy encoding detection and conversion to UTF-8 (TXT/FB2 from Russian books).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace reader {

enum class TextEncoding : uint8_t {
    Utf8,
    Cp1251,
    Utf16Le,
    Utf16Be,
};

// Detects encoding by BOM first, then by UTF-8 validation (CP1251 fallback).
// Only needs a prefix of the data; 4 KB is enough in practice.
TextEncoding detect_encoding(const uint8_t* data, size_t size);

// Converts to UTF-8, strips BOM and replaces undecodable bytes.
std::string to_utf8(const uint8_t* data, size_t size, TextEncoding encoding);

std::string cp1251_to_utf8(const uint8_t* data, size_t size);
std::string utf16_to_utf8(const uint8_t* data, size_t size, bool little_endian);

const char* encoding_name(TextEncoding encoding);

} // namespace reader
