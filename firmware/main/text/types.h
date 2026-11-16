// Text model shared by formats, layout and UI.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace reader {

// Font face selected by markup (bold/italic) or by paragraph kind (code).
enum class Face : uint8_t { Regular, Bold, Italic, BoldItalic, Mono };

enum class ParaKind : uint8_t {
    Text,      // body paragraph
    Heading,   // heading, level 1..6
    Quote,     // block quote
    ListItem,  // list item, level 1..3, marker holds "•" or "1."
    Code,      // one line of a code block
    Separator, // horizontal rule
    Blank,     // explicit vertical gap
};

struct Style {
    Face face = Face::Regular;
    bool small = false; // reserved: footnotes / sub-captions
};

struct Run {
    Style style;
    std::string text; // UTF-8
};

// Inline reference to a note (footnote / endnote). Byte range inside one run.
struct Link {
    uint32_t run = 0;    // run index in Paragraph::runs
    uint32_t begin = 0;  // byte offset inside that run
    uint32_t end = 0;
    uint32_t note = 0;   // 1-based index in the document note table, 0 = unresolved
};

struct Paragraph {
    ParaKind kind = ParaKind::Text;
    uint8_t level = 0;     // heading level 1..6 or list nesting 1..3
    std::string marker;    // list bullet/number, drawn hanging
    std::vector<Run> runs;
    std::vector<Link> links;
    uint64_t token = 0;    // source position of the paragraph start
};

struct DocInfo {
    std::string title;
    std::string author;
};

// Position inside a document: paragraph token + character (codepoint) offset.
struct PagePos {
    uint64_t token = 0;
    uint32_t char_offset = 0;

    bool operator==(const PagePos& other) const {
        return token == other.token && char_offset == other.char_offset;
    }
    bool operator!=(const PagePos& other) const { return !(*this == other); }
};

// Number of Unicode codepoints in UTF-8 text (invalid bytes count as one).
uint32_t utf8_length(const std::string& text);

} // namespace reader
