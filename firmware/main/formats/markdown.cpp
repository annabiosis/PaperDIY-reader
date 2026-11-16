// Markdown parser for the e-ink reader.
//
// Design notes:
//  * The file is read entirely (8 MiB cap), converted to UTF-8 with the shared
//    encoding helpers and split into SourceLine records. Every record keeps the
//    byte offset of the line start; that offset becomes Paragraph::token.
//  * Fenced code blocks are emitted as one Code paragraph per source line (the
//    layout expects one line per paragraph). Blank code lines are kept as empty
//    Code paragraphs so vertical spacing survives.
//  * Block quotes are parsed recursively: one leading '>' is stripped from each
//    line and the inner lines are parsed as a complete block sequence. Every
//    resulting paragraph becomes Quote with level = quote nesting depth.
//  * List nesting is derived from indentation with a small indent stack, so
//    2-, 3- or 4-space steps all work. The marker is stored separately ("•" or
//    the original "N."), and indented continuation lines are appended to the
//    item text with a space.
//  * Setext underlines turn the preceding paragraph into a Heading 1/2, which
//    matches normal Markdown (including the "---" case).
//  * All parsing is linear in the input size and every recursion is depth
//    limited, so malformed or hostile input cannot loop or blow the stack.

#include "formats/formats.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/encoding.h"
#include "core/utf8.h"
#include "text/types.h"

namespace reader {
namespace {

constexpr size_t kMaxMarkdownBytes = 8u * 1024u * 1024u;
constexpr int kMaxQuoteDepth = 16;
constexpr int kMaxInlineDepth = 24;

// ---------------------------------------------------------------------------
// Small string helpers (ASCII-only checks, no locale).
// ---------------------------------------------------------------------------

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

bool is_blank(const std::string& text) {
    for (char c : text) {
        if (!is_space(c)) return false;
    }
    return true;
}

std::string trim(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && is_space(text[begin])) ++begin;
    while (end > begin && is_space(text[end - 1])) --end;
    return text.substr(begin, end - begin);
}

bool is_ascii_alnum(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

bool is_word_char(char c) {
    return is_ascii_alnum(c) || c == '_';
}

bool is_escapable(char c) {
    const unsigned char byte = static_cast<unsigned char>(c);
    if (byte >= 0x80 || byte == 0) return false;
    if (is_ascii_alnum(c) || is_space(c)) return false;
    return true;
}

std::string lower_ascii(std::string value) {
    for (char& c : value) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return value;
}

// ---------------------------------------------------------------------------
// Source lines: text plus byte offset in the UTF-8 document.
// ---------------------------------------------------------------------------

struct SourceLine {
    std::string text;
    uint64_t token = 0;
};

std::vector<SourceLine> split_lines(const std::string& text) {
    std::vector<SourceLine> lines;
    lines.reserve(text.size() / 32 + 1);
    size_t start = 0;
    size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (c == '\n') {
            lines.push_back(SourceLine{text.substr(start, i - start), start});
            ++i;
            start = i;
        } else if (c == '\r') {
            lines.push_back(SourceLine{text.substr(start, i - start), start});
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                i += 2;
            } else {
                ++i;
            }
            start = i;
        } else {
            ++i;
        }
    }
    lines.push_back(SourceLine{text.substr(start), start});
    return lines;
}

// Indentation in columns; a tab advances to the next multiple of four.
size_t leading_indent(const std::string& text) {
    size_t column = 0;
    for (char c : text) {
        if (c == ' ') {
            ++column;
        } else if (c == '\t') {
            column += 4 - (column % 4);
        } else {
            break;
        }
    }
    return column;
}

// ---------------------------------------------------------------------------
// Block markers.
// ---------------------------------------------------------------------------

struct FenceInfo {
    bool ok = false;
    char ch = 0;
    size_t len = 0;
    size_t content = 0;
};

FenceInfo fence_start(const std::string& text) {
    FenceInfo info;
    size_t i = 0;
    while (i < text.size() && i < 3 && text[i] == ' ') ++i;
    if (i >= text.size()) return info;
    const char c = text[i];
    if (c != '`' && c != '~') return info;
    size_t end = i;
    while (end < text.size() && text[end] == c) ++end;
    if (end - i < 3) return info;
    // Backtick info strings may not contain another backtick.
    if (c == '`') {
        for (size_t k = end; k < text.size(); ++k) {
            if (text[k] == '`') return info;
        }
    }
    info.ok = true;
    info.ch = c;
    info.len = end - i;
    info.content = end;
    return info;
}

bool fence_closes(const std::string& text, char ch, size_t open_len) {
    size_t i = 0;
    while (i < text.size() && i < 3 && text[i] == ' ') ++i;
    size_t end = i;
    while (end < text.size() && text[end] == ch) ++end;
    if (end - i < open_len) return false;
    for (size_t k = end; k < text.size(); ++k) {
        if (!is_space(text[k])) return false;
    }
    return true;
}

struct HeadingInfo {
    bool ok = false;
    int level = 0;
    std::string content;
};

HeadingInfo atx_heading(const std::string& text) {
    HeadingInfo info;
    size_t i = 0;
    while (i < text.size() && i < 3 && text[i] == ' ') ++i;
    size_t hashes = 0;
    while (i + hashes < text.size() && text[i + hashes] == '#') ++hashes;
    if (hashes < 1 || hashes > 6) return info;
    size_t pos = i + hashes;
    if (pos < text.size() && text[pos] != ' ' && text[pos] != '\t') return info;
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) ++pos;
    std::string content = trim(text.substr(pos));

    // Drop a trailing run of '#' when it is separated by whitespace.
    size_t end = content.size();
    size_t closing = 0;
    while (closing < end && content[end - 1 - closing] == '#') ++closing;
    if (closing > 0) {
        const size_t before = end - closing;
        if (before == 0 || content[before - 1] == ' ' || content[before - 1] == '\t') {
            content = trim(content.substr(0, before));
        }
    }

    info.ok = true;
    info.level = static_cast<int>(hashes);
    info.content = std::move(content);
    return info;
}

bool is_hr(const std::string& text) {
    size_t i = 0;
    while (i < text.size() && i < 3 && text[i] == ' ') ++i;
    if (i >= text.size()) return false;
    const char c = text[i];
    if (c != '-' && c != '*' && c != '_') return false;
    size_t count = 0;
    for (; i < text.size(); ++i) {
        if (text[i] == c) {
            ++count;
        } else if (text[i] == ' ' || text[i] == '\t') {
            continue;
        } else {
            return false;
        }
    }
    return count >= 3;
}

// Returns 1 for '=' (Heading 1), 2 for '-' (Heading 2), 0 otherwise.
int setext_level(const std::string& text) {
    const std::string value = trim(text);
    if (value.empty()) return 0;
    const char c = value[0];
    if (c != '=' && c != '-') return 0;
    for (char ch : value) {
        if (ch != c) return 0;
    }
    if (c == '-' && value.size() < 2) return 0;
    return c == '=' ? 1 : 2;
}

// Recognizes an optional up-to-3-space indented '>' and returns the offset of
// the text after the marker (and at most one following space).
bool quote_prefix(const std::string& text, size_t* rest) {
    size_t i = 0;
    while (i < text.size() && i < 3 && text[i] == ' ') ++i;
    if (i >= text.size() || text[i] != '>') return false;
    ++i;
    if (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
    *rest = i;
    return true;
}

struct ListInfo {
    bool ok = false;
    size_t indent = 0;
    bool ordered = false;
    unsigned long number = 0;
    char bullet = 0;
    size_t content = 0;
};

ListInfo list_marker(const std::string& text) {
    ListInfo info;
    size_t i = 0;
    size_t column = 0;
    while (i < text.size()) {
        if (text[i] == ' ') {
            ++column;
            ++i;
        } else if (text[i] == '\t') {
            column += 4 - (column % 4);
            ++i;
        } else {
            break;
        }
    }
    if (i >= text.size()) return info;

    const char first = text[i];
    if (first == '-' || first == '*' || first == '+') {
        if (i + 1 < text.size() && text[i + 1] != ' ' && text[i + 1] != '\t') return info;
        size_t pos = i + 1;
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) ++pos;
        info.ok = true;
        info.indent = column;
        info.ordered = false;
        info.bullet = first;
        info.content = pos;
        return info;
    }

    if (first >= '0' && first <= '9') {
        size_t pos = i;
        unsigned long number = 0;
        size_t digits = 0;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9' && digits < 9) {
            number = number * 10 + static_cast<unsigned long>(text[pos] - '0');
            ++pos;
            ++digits;
        }
        if (pos < text.size() && (text[pos] == '.' || text[pos] == ')')) {
            size_t content = pos + 1;
            if (content >= text.size() || text[content] == ' ' || text[content] == '\t') {
                while (content < text.size() && (text[content] == ' ' || text[content] == '\t')) {
                    ++content;
                }
                info.ok = true;
                info.indent = column;
                info.ordered = true;
                info.number = number;
                info.content = content;
                return info;
            }
        }
    }
    return info;
}

bool has_unescaped_pipe(const std::string& text) {
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\') {
            ++i;
            continue;
        }
        if (text[i] == '|') return true;
    }
    return false;
}

std::vector<std::string> split_table_cells(const std::string& text) {
    std::vector<std::string> cells;
    std::string current;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\\' && i + 1 < text.size()) {
            // Keep the escape for the inline parser; it must not split the cell.
            current.push_back(c);
            current.push_back(text[i + 1]);
            ++i;
        } else if (c == '|') {
            cells.push_back(trim(current));
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    cells.push_back(trim(current));

    // Leading/trailing pipes produce empty edge cells.
    while (!cells.empty() && cells.front().empty()) cells.erase(cells.begin());
    while (!cells.empty() && cells.back().empty()) cells.pop_back();
    return cells;
}

bool is_table_separator(const std::vector<std::string>& cells) {
    if (cells.empty()) return false;
    for (const std::string& raw : cells) {
        const std::string cell = trim(raw);
        if (cell.empty()) return false;
        size_t i = 0;
        if (cell[i] == ':') ++i;
        size_t dashes = 0;
        while (i < cell.size() && cell[i] == '-') {
            ++i;
            ++dashes;
        }
        if (dashes == 0) return false;
        if (i < cell.size() && cell[i] == ':') ++i;
        if (i != cell.size()) return false;
    }
    return true;
}

bool is_block_start(const std::string& text) {
    if (is_blank(text)) return true;
    if (fence_start(text).ok) return true;
    if (atx_heading(text).ok) return true;
    if (is_hr(text)) return true;
    size_t quote_rest = 0;
    if (quote_prefix(text, &quote_rest)) return true;
    if (list_marker(text).ok) return true;
    if (has_unescaped_pipe(text)) return true;
    return false;
}

// Indent stack -> list nesting level 1..3. A new deeper indent is one level
// deeper, a dedent returns to the matching level. This accepts 2-, 3- or
// 4-space nesting steps.
uint8_t list_level(std::vector<size_t>& stack, size_t indent) {
    while (!stack.empty() && stack.back() > indent) stack.pop_back();
    if (stack.empty()) {
        stack.push_back(indent);
        return 1;
    }
    if (stack.back() == indent) {
        const size_t level = stack.size();
        return static_cast<uint8_t>(level > 3 ? 3 : level);
    }
    // Deeper than the current item.
    if (stack.size() >= 3) return 3;
    stack.push_back(indent);
    return static_cast<uint8_t>(stack.size());
}

// ---------------------------------------------------------------------------
// Inline helpers.
// ---------------------------------------------------------------------------

size_t find_backtick_close(const std::string& s, size_t from, size_t end, size_t run) {
    size_t i = from;
    while (i < end) {
        if (s[i] == '`') {
            size_t r = 1;
            while (i + r < end && s[i + r] == '`') ++r;
            if (r == run) return i;
            i += r;
        } else {
            ++i;
        }
    }
    return std::string::npos;
}

size_t find_emphasis_close(const std::string& s, size_t from, size_t end, char ch, size_t run) {
    size_t i = from;
    while (i < end) {
        if (s[i] == '\\' && i + 1 < end && is_escapable(s[i + 1])) {
            i += 2;
            continue;
        }
        if (s[i] == '`') {
            size_t r = 1;
            while (i + r < end && s[i + r] == '`') ++r;
            const size_t close = find_backtick_close(s, i + r, end, r);
            if (close != std::string::npos) {
                i = close + r;
            } else {
                i += r;
            }
            continue;
        }
        if (s[i] == ch) {
            size_t r = 1;
            while (i + r < end && s[i + r] == ch) ++r;
            if (r == run && i > from && !is_space(s[i - 1])) {
                if (ch == '_' && i + run < end && is_word_char(s[i + run])) {
                    i += r;
                    continue;
                }
                return i;
            }
            i += r;
            continue;
        }
        ++i;
    }
    return std::string::npos;
}

size_t find_strike_close(const std::string& s, size_t from, size_t end) {
    for (size_t i = from; i + 1 < end; ++i) {
        if (s[i] == '\\' && i + 1 < end) {
            ++i;
            continue;
        }
        if (s[i] == '~' && s[i + 1] == '~') return i;
    }
    return std::string::npos;
}

// Finds "[label](target)" starting at the '[' at `open_bracket`.
bool parse_link_target(const std::string& s, size_t open_bracket, size_t end, size_t* close_bracket,
                       size_t* close_paren) {
    size_t depth = 0;
    size_t i = open_bracket;
    for (; i < end; ++i) {
        const char c = s[i];
        if (c == '\\' && i + 1 < end) {
            ++i;
            continue;
        }
        if (c == '[') {
            ++depth;
        } else if (c == ']') {
            if (depth == 0) break;
            --depth;
            if (depth == 0) break;
        }
    }
    if (i >= end) return false;
    *close_bracket = i;

    size_t pos = i + 1;
    if (pos >= end || s[pos] != '(') return false;
    size_t parens = 1;
    bool in_angle = false;
    size_t j = pos + 1;
    for (; j < end; ++j) {
        const char c = s[j];
        if (c == '\\' && j + 1 < end) {
            ++j;
            continue;
        }
        if (c == '<') {
            in_angle = true;
        } else if (c == '>') {
            in_angle = false;
        } else if (c == '(' && !in_angle) {
            ++parens;
        } else if (c == ')' && !in_angle) {
            --parens;
            if (parens == 0) break;
        }
    }
    if (j >= end) return false;
    *close_paren = j;
    return true;
}

// Recognizes an HTML tag. Comments are dropped; `<br>` reports `is_br`.
bool html_tag(const std::string& s, size_t pos, size_t end, size_t* consumed, bool* is_br) {
    *is_br = false;
    if (pos + 4 <= end && s.compare(pos, 4, "<!--") == 0) {
        const size_t close = s.find("-->", pos + 4);
        if (close != std::string::npos && close + 3 <= end) {
            *consumed = close + 3;
            return true;
        }
        return false;
    }

    const size_t close = s.find('>', pos + 1);
    if (close == std::string::npos || close >= end) return false;
    const std::string inner = trim(s.substr(pos + 1, close - pos - 1));
    if (inner.empty()) return false;
    // Autolinks such as <https://example.com> are text, not HTML tags.
    if (inner.find("://") != std::string::npos) return false;
    if (inner[0] == '!' || inner[0] == '?') {
        *consumed = close + 1;
        return true;
    }

    size_t i = 0;
    bool closing = false;
    if (inner[i] == '/') {
        closing = true;
        ++i;
    }
    if (i >= inner.size()) return false;
    const char first = inner[i];
    if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z'))) return false;
    const size_t name_start = i;
    while (i < inner.size() && (is_ascii_alnum(inner[i]) || inner[i] == '-' || inner[i] == ':')) {
        ++i;
    }
    const std::string name = lower_ascii(inner.substr(name_start, i - name_start));
    if (i < inner.size() && !is_space(inner[i]) && inner[i] != '/') return false;
    if (!closing && name == "br") *is_br = true;
    *consumed = close + 1;
    return true;
}

class InlineParser {
  public:
    explicit InlineParser(std::vector<Run>& out) : _out(out) {}

    void parse(const std::string& text) { parse_range(text, 0, text.size(), false, false, false, 0); }

  private:
    void push(bool bold, bool italic, bool mono, std::string text) {
        if (text.empty()) return;
        Face face = Face::Regular;
        if (mono) {
            face = Face::Mono;
        } else if (bold && italic) {
            face = Face::BoldItalic;
        } else if (bold) {
            face = Face::Bold;
        } else if (italic) {
            face = Face::Italic;
        }
        if (!_out.empty() && _out.back().style.face == face && !_out.back().style.small) {
            _out.back().text += text;
            return;
        }
        Run run;
        run.style.face = face;
        run.text = std::move(text);
        _out.push_back(std::move(run));
    }

    void parse_range(const std::string& s, size_t begin, size_t end, bool bold, bool italic, bool mono,
                     int depth) {
        if (begin >= end) return;
        if (depth > kMaxInlineDepth) {
            push(bold, italic, mono, s.substr(begin, end - begin));
            return;
        }

        std::string buffer;
        auto flush = [&]() {
            if (!buffer.empty()) {
                push(bold, italic, mono, std::move(buffer));
                buffer.clear();
            }
        };

        bool bracket_possible = true;
        bool tag_possible = true;

        size_t i = begin;
        while (i < end) {
            const char c = s[i];

            // Backslash escapes: the character becomes plain text.
            if (c == '\\' && i + 1 < end && is_escapable(s[i + 1])) {
                buffer.push_back(s[i + 1]);
                i += 2;
                continue;
            }

            // `code`
            if (c == '`') {
                size_t run = 1;
                while (i + run < end && s[i + run] == '`') ++run;
                const size_t close = find_backtick_close(s, i + run, end, run);
                if (close != std::string::npos) {
                    flush();
                    push(bold, italic, true, s.substr(i + run, close - (i + run)));
                    i = close + run;
                } else {
                    buffer.append(run, '`');
                    i += run;
                }
                continue;
            }

            // HTML tags are dropped, <br> becomes a space.
            if (c == '<' && tag_possible) {
                size_t consumed = 0;
                bool br = false;
                if (html_tag(s, i, end, &consumed, &br)) {
                    if (br) buffer.push_back(' ');
                    i = consumed;
                    continue;
                }
                tag_possible = s.find('>', i + 1) != std::string::npos;
                buffer.push_back(c);
                ++i;
                continue;
            }

            // ~~strikethrough~~ -> ordinary text (nested markup still applies).
            if (c == '~' && i + 1 < end && s[i + 1] == '~') {
                const size_t close = find_strike_close(s, i + 2, end);
                if (close != std::string::npos) {
                    flush();
                    parse_range(s, i + 2, close, bold, italic, mono, depth + 1);
                    i = close + 2;
                    continue;
                }
                buffer.append("~~");
                i += 2;
                continue;
            }

            // ![alt](src) -> [image: alt]
            if (c == '!' && i + 1 < end && s[i + 1] == '[' && bracket_possible) {
                size_t close_bracket = 0;
                size_t close_paren = 0;
                if (parse_link_target(s, i + 1, end, &close_bracket, &close_paren)) {
                    flush();
                    const std::string alt = trim(s.substr(i + 2, close_bracket - (i + 2)));
                    if (alt.empty()) {
                        buffer.append("[image]");
                    } else {
                        buffer.append("[image: ");
                        buffer.append(alt);
                        buffer.push_back(']');
                    }
                    i = close_paren + 1;
                    continue;
                }
                bracket_possible = s.find(']', i + 2) != std::string::npos;
            }

            // [text](url) -> text
            if (c == '[' && bracket_possible) {
                size_t close_bracket = 0;
                size_t close_paren = 0;
                if (parse_link_target(s, i, end, &close_bracket, &close_paren)) {
                    flush();
                    parse_range(s, i + 1, close_bracket, bold, italic, mono, depth + 1);
                    i = close_paren + 1;
                    continue;
                }
                bracket_possible = s.find(']', i + 1) != std::string::npos;
                buffer.push_back('[');
                ++i;
                continue;
            }

            // Emphasis: *, **, ***, _ and __.
            if (c == '*' || c == '_') {
                size_t run = 1;
                while (i + run < end && s[i + run] == c) ++run;
                bool can_open = (i + run < end) && !is_space(s[i + run]);
                if (c == '_' && i > begin && is_word_char(s[i - 1])) can_open = false;
                if (can_open && run <= 3) {
                    const size_t close = find_emphasis_close(s, i + run, end, c, run);
                    if (close != std::string::npos) {
                        flush();
                        const bool child_bold = bold || run >= 2;
                        const bool child_italic = italic || run == 1 || run == 3;
                        parse_range(s, i + run, close, child_bold, child_italic, mono, depth + 1);
                        i = close + run;
                        continue;
                    }
                }
                buffer.append(run, c);
                i += run;
                continue;
            }

            buffer.push_back(c);
            ++i;
        }
        flush();
    }

    std::vector<Run>& _out;
};

std::vector<Run> inline_runs(const std::string& text) {
    std::vector<Run> runs;
    InlineParser parser(runs);
    parser.parse(text);
    return runs;
}

Paragraph make_paragraph(ParaKind kind, uint8_t level, std::string marker, uint64_t token,
                         const std::string& text) {
    Paragraph paragraph;
    paragraph.kind = kind;
    paragraph.level = level;
    paragraph.marker = std::move(marker);
    paragraph.token = token;
    paragraph.runs = inline_runs(text);
    return paragraph;
}

Paragraph make_code_line(const SourceLine& line) {
    Paragraph paragraph;
    paragraph.kind = ParaKind::Code;
    paragraph.token = line.token;
    if (!line.text.empty()) {
        Run run;
        run.style.face = Face::Mono;
        run.text = line.text;
        paragraph.runs.push_back(std::move(run));
    }
    return paragraph;
}

// ---------------------------------------------------------------------------
// Block parser.
// ---------------------------------------------------------------------------

void parse_blocks(const std::vector<SourceLine>& lines, int depth, std::vector<Paragraph>& out) {
    std::vector<size_t> list_stack;
    size_t i = 0;
    while (i < lines.size()) {
        const SourceLine& line = lines[i];
        if (is_blank(line.text)) {
            ++i;
            continue;
        }

        // Fenced code block: one Code paragraph per source line.
        const FenceInfo fence = fence_start(line.text);
        if (fence.ok) {
            size_t j = i + 1;
            while (j < lines.size() && !fence_closes(lines[j].text, fence.ch, fence.len)) ++j;
            for (size_t k = i + 1; k < j; ++k) out.push_back(make_code_line(lines[k]));
            i = (j < lines.size()) ? j + 1 : j;
            list_stack.clear();
            continue;
        }

        const HeadingInfo heading = atx_heading(line.text);
        if (heading.ok) {
            out.push_back(make_paragraph(ParaKind::Heading, static_cast<uint8_t>(heading.level), {},
                                         line.token, heading.content));
            ++i;
            list_stack.clear();
            continue;
        }

        if (is_hr(line.text)) {
            Paragraph paragraph;
            paragraph.kind = ParaKind::Separator;
            paragraph.token = line.token;
            out.push_back(std::move(paragraph));
            ++i;
            list_stack.clear();
            continue;
        }

        size_t quote_rest = 0;
        if (quote_prefix(line.text, &quote_rest)) {
            std::vector<SourceLine> inner;
            size_t j = i;
            while (j < lines.size()) {
                size_t rest = 0;
                if (!quote_prefix(lines[j].text, &rest)) break;
                inner.push_back(SourceLine{lines[j].text.substr(rest), lines[j].token});
                ++j;
            }

            if (depth >= kMaxQuoteDepth) {
                // Depth guard: flatten the remaining quote text.
                std::string joined;
                for (const SourceLine& inner_line : inner) {
                    const std::string part = trim(inner_line.text);
                    if (part.empty()) continue;
                    if (!joined.empty()) joined.push_back(' ');
                    joined += part;
                }
                const int level = std::min(255, depth + 1);
                Paragraph paragraph = make_paragraph(ParaKind::Quote, static_cast<uint8_t>(level), {},
                                                     line.token, joined);
                if (!paragraph.runs.empty()) out.push_back(std::move(paragraph));
            } else {
                std::vector<Paragraph> children;
                parse_blocks(inner, depth + 1, children);
                for (Paragraph& child : children) {
                    if (child.kind == ParaKind::Quote) {
                        const unsigned level = static_cast<unsigned>(child.level) + 1u;
                        child.level = static_cast<uint8_t>(level > 255u ? 255u : level);
                    } else if (child.kind != ParaKind::Separator) {
                        child.kind = ParaKind::Quote;
                        child.level = 1;
                    }
                    out.push_back(std::move(child));
                }
            }
            i = j;
            list_stack.clear();
            continue;
        }

        const ListInfo list = list_marker(line.text);
        if (list.ok) {
            const uint8_t level = list_level(list_stack, list.indent);
            std::string text = trim(line.text.substr(list.content));
            size_t j = i + 1;
            while (j < lines.size() && !is_blank(lines[j].text)) {
                if (is_block_start(lines[j].text)) break;
                if (leading_indent(lines[j].text) < list.indent + 2) break;
                const std::string continuation = trim(lines[j].text);
                if (!continuation.empty()) {
                    if (!text.empty()) text.push_back(' ');
                    text += continuation;
                }
                ++j;
            }
            std::string marker;
            if (list.ordered) {
                marker = std::to_string(list.number) + ".";
            } else {
                marker = "\xE2\x80\xA2"; // UTF-8 bullet
            }
            out.push_back(
                make_paragraph(ParaKind::ListItem, level, std::move(marker), line.token, text));
            i = j;
            continue;
        }

        // Table row: one Text paragraph per row, separator row skipped.
        if (has_unescaped_pipe(line.text)) {
            const std::vector<std::string> cells = split_table_cells(line.text);
            list_stack.clear();
            ++i;
            if (is_table_separator(cells)) continue;
            std::string joined;
            for (const std::string& cell : cells) {
                if (!joined.empty()) joined.append(" | ");
                joined += cell;
            }
            if (joined.empty()) continue;
            out.push_back(make_paragraph(ParaKind::Text, 0, {}, line.token, joined));
            continue;
        }

        // Paragraph (possibly a Setext heading when the next line is an
        // underline made of '=' or '-').
        std::string text = trim(line.text);
        const uint64_t token = line.token;
        bool is_heading = false;
        int heading_level = 0;
        ++i;
        while (i < lines.size() && !is_blank(lines[i].text)) {
            const int underline = setext_level(lines[i].text);
            if (underline > 0) {
                is_heading = true;
                heading_level = underline;
                ++i;
                break;
            }
            if (is_block_start(lines[i].text)) break;
            const std::string addition = trim(lines[i].text);
            if (!addition.empty()) {
                if (!text.empty()) text.push_back(' ');
                text += addition;
            }
            ++i;
        }
        list_stack.clear();

        Paragraph paragraph =
            make_paragraph(is_heading ? ParaKind::Heading : ParaKind::Text,
                           static_cast<uint8_t>(heading_level), {}, token, text);
        if (!paragraph.runs.empty()) out.push_back(std::move(paragraph));
    }
}

std::string first_h1(const std::vector<Paragraph>& paragraphs) {
    for (const Paragraph& paragraph : paragraphs) {
        if (paragraph.kind == ParaKind::Heading && paragraph.level == 1) {
            std::string title;
            for (const Run& run : paragraph.runs) title += run.text;
            return trim(title);
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// Document implementation.
// ---------------------------------------------------------------------------

class MarkdownDocument final : public Document {
  public:
    MarkdownDocument(DocInfo info, std::vector<Paragraph> paragraphs, uint64_t text_size,
                     uint32_t total_chars)
        : _info(std::move(info)), _paragraphs(std::move(paragraphs)), _text_size(text_size),
          _total_chars(total_chars) {}

    const DocInfo& info() const override { return _info; }

    bool rewind() override {
        _index = 0;
        return true;
    }

    bool next(Paragraph& out) override {
        if (_index >= _paragraphs.size()) return false;
        out = _paragraphs[_index++];
        return true;
    }

    bool seek(uint64_t token) override {
        const auto it = std::lower_bound(
            _paragraphs.begin(), _paragraphs.end(), token,
            [](const Paragraph& paragraph, uint64_t value) { return paragraph.token < value; });
        _index = static_cast<size_t>(it - _paragraphs.begin());
        return it != _paragraphs.end();
    }

    uint32_t permille(uint64_t token) const override {
        if (_text_size == 0) return 0;
        if (token >= _text_size) return 1000;
        const uint64_t value = token * 1000u / _text_size;
        return static_cast<uint32_t>(std::min<uint64_t>(value, 1000u));
    }

    uint64_t total_chars() const override { return _total_chars; }

  private:
    DocInfo _info;
    std::vector<Paragraph> _paragraphs;
    uint64_t _text_size = 0;
    uint32_t _total_chars = 0;
    size_t _index = 0;
};

} // namespace

std::unique_ptr<Document> open_markdown(std::unique_ptr<ByteSource> source, std::string* error) {
    if (error) error->clear();
    if (!source) {
        if (error) *error = "no source";
        return nullptr;
    }

    std::string raw;
    if (!read_all(*source, kMaxMarkdownBytes, &raw, error)) return nullptr;

    const auto* bytes = reinterpret_cast<const uint8_t*>(raw.data());
    const TextEncoding encoding = detect_encoding(bytes, raw.size());
    std::string text = to_utf8(bytes, raw.size(), encoding);
    std::string().swap(raw);

    const uint64_t text_size = static_cast<uint64_t>(text.size());
    const uint32_t total_chars = utf8_length(text);
    const std::vector<SourceLine> lines = split_lines(text);

    std::vector<Paragraph> paragraphs;
    parse_blocks(lines, 0, paragraphs);

    DocInfo info;
    info.title = first_h1(paragraphs);

    std::string().swap(text);

    return std::make_unique<MarkdownDocument>(std::move(info), std::move(paragraphs), text_size,
                                              total_chars);
}

} // namespace reader
