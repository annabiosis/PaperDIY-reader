#include "text/hyphenate.h"

#include "core/utf8.h"

namespace reader {
namespace {

bool is_russian_vowel(uint32_t cp) {
    switch (cp) {
    case 0x0430: // a
    case 0x0435: // e
    case 0x0451: // yo
    case 0x0438: // i
    case 0x043E: // o
    case 0x0443: // u
    case 0x044B: // y
    case 0x044D: // e (hard)
    case 0x044E: // yu
    case 0x044F: // ya
        return true;
    default:
        return false;
    }
}

// Letters that belong to the previous syllable: a break next to them is invalid.
bool sticks_to_previous(uint32_t cp) {
    switch (cp) {
    case 0x0439: // i kratkoye (semivowel)
    case 0x044C: // soft sign
    case 0x044A: // hard sign
    case 0x044B: // y
        return true;
    default:
        return false;
    }
}

bool is_letter(uint32_t cp) {
    return (cp >= 0x0410 && cp <= 0x044F) || cp == 0x0401 || cp == 0x0451;
}

// Soft hyphen and zero-width marks: not letters, but a soft hyphen is an explicit
// break opportunity supplied by the book itself.
bool is_invisible(uint32_t cp) {
    switch (cp) {
    case 0x00AD: // soft hyphen
    case 0x200B:
    case 0x200C:
    case 0x200D:
    case 0x200E:
    case 0x200F:
    case 0xFEFF:
        return true;
    default:
        return false;
    }
}

} // namespace

bool is_cyrillic_word(const char* word, size_t length) {
    size_t pos = 0;
    bool saw_cyrillic = false;
    while (pos < length) {
        size_t used = 0;
        const uint32_t cp = utf8_decode(word + pos, length - pos, &used);
        pos += used == 0 ? 1 : used;
        if (is_invisible(cp)) continue;
        if (cp >= 0x0400 && cp <= 0x04FF) {
            saw_cyrillic = true;
        } else if (cp < 0x80) {
            continue;
        } else {
            return false;
        }
    }
    return saw_cyrillic;
}

size_t hyphenation_points(const char* word, size_t length, uint32_t* out, size_t max_points) {
    if (max_points == 0 || length < 8) return 0;
    if (!is_cyrillic_word(word, length)) return 0;

    uint32_t codepoints[96];
    uint32_t offsets[96];
    bool explicit_break[96];
    size_t count = 0;
    bool pending_soft_hyphen = false;
    size_t pos = 0;
    while (pos < length && count < 96) {
        size_t used = 0;
        const uint32_t cp = utf8_decode(word + pos, length - pos, &used);
        if (used == 0) break;
        if (cp == 0x00AD) {
            pending_soft_hyphen = true; // the book already says where it may break
            pos += used;
            continue;
        }
        if (is_invisible(cp)) {
            pos += used;
            continue;
        }
        codepoints[count] = cp;
        offsets[count] = static_cast<uint32_t>(pos);
        explicit_break[count] = pending_soft_hyphen;
        pending_soft_hyphen = false;
        ++count;
        pos += used;
    }
    if (count < 6 || pos != length) return 0;

    size_t written = 0;
    for (size_t i = 2; i + 2 < count; ++i) {
        const uint32_t previous = codepoints[i - 1];
        const uint32_t next = codepoints[i];
        if (!is_letter(previous) || !is_letter(next)) continue;
        if (sticks_to_previous(next) || sticks_to_previous(previous)) continue;

        bool allowed = explicit_break[i];
        if (!allowed) {
            // School hyphenation rules:
            //   * one consonant between vowels goes to the next syllable (vo-da);
            //   * a consonant cluster between vowels breaks before its last
            //     consonant (kuv-shin, ob-ser-va-to-riya).
            if (is_russian_vowel(previous) && !is_russian_vowel(next)) {
                allowed = (i + 1 >= count) || is_russian_vowel(codepoints[i + 1]);
            } else if (!is_russian_vowel(previous) && !is_russian_vowel(next)) {
                allowed = (i + 1 < count) && is_russian_vowel(codepoints[i + 1]);
            }
        }

        if (!allowed) continue;
        if (written < max_points) out[written] = offsets[i];
        ++written;
    }
    return written < max_points ? written : max_points;
}

} // namespace reader
