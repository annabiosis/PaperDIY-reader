// Russian hyphenation rules and split points for line breaking.
//
// A full TeX-pattern hyphenator costs ~20 KB of tables and a dictionary walk for
// every word. For an e-reader a compact rule set is enough: it only runs when a
// word does not fit into the rest of a line, and the rules below cover the
// common Russian cases (school hyphenation).
#pragma once

#include <cstddef>
#include <cstdint>

namespace reader {

// Fills `out` with up to `max_points` byte offsets inside `word` (UTF-8) where a
// hyphen may be inserted. Returns the number of points written.
//
// `word` must contain only letters (no spaces, no punctuation); offsets are byte
// positions, so `word[0..point)` is the part that stays on the line.
size_t hyphenation_points(const char* word, size_t length, uint32_t* out, size_t max_points);

// True when the text looks like Cyrillic (words in other scripts are not hyphenated).
bool is_cyrillic_word(const char* word, size_t length);

} // namespace reader
