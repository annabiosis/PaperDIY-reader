#include "text/layout.h"

#include "text/hyphenate.h"

#include <algorithm>
#include <cstddef>

#include "core/utf8.h"

namespace reader {

namespace {

constexpr int kQuoteIndentLeft = 20;
constexpr int kQuoteIndentRight = 8;
constexpr int kListStep = 18;
constexpr int kListMarkerWidth = 16;
constexpr int kCodeIndent = 10;
constexpr size_t kNoSpace = static_cast<size_t>(-1);

constexpr uint32_t kUnresolvedLink = 0xFFFFFFFFu;

struct CharRef {
    uint32_t cp = 0;
    uint16_t run = 0;
    uint32_t begin = 0;
    uint32_t end = 0;
    uint32_t cp_index = 0; // codepoint offset inside the paragraph (invisibles counted)
    uint8_t size_index = 1;
    Face face = Face::Regular;
    uint16_t advance = 0;
    bool space = false;
    bool hyphen_break = false; // a hyphen may be inserted before this char
    uint32_t link = 0;        // note number the character belongs to, 0 = plain text
};

struct ParaStyle {
    int left = 0;
    int right = 0;
    int first_line_indent = 0;
    int size_index = 1;
    Face face = Face::Regular;
    int space_before = 0;
    int space_after = 0;
    int line_height = 0;
    int ascent = 0;
    bool preserve_spaces = false;
    int hyphen_advance = 0; // 0 disables hyphenation for this paragraph
};

ParaStyle style_for(const Paragraph& paragraph, const LayoutMetrics& metrics) {
    ParaStyle style;
    style.size_index = metrics.size_index;
    const int body_size = find_font(metrics.size_index, Face::Regular).pixel_size;

    switch (paragraph.kind) {
    case ParaKind::Heading: {
        style.size_index = std::min(2, metrics.size_index + (paragraph.level <= 1 ? 2 : (paragraph.level == 2 ? 1 : 0)));
        style.face = Face::Bold;
        style.space_before = std::max(10, body_size * 3 / 4);
        style.space_after = std::max(6, body_size / 2);
        break;
    }
    case ParaKind::Quote:
        style.face = Face::Italic;
        style.left = kQuoteIndentLeft;
        style.right = kQuoteIndentRight;
        style.space_before = body_size / 3;
        style.space_after = body_size / 3;
        break;
    case ParaKind::ListItem: {
        const int level = paragraph.level > 0 ? paragraph.level : 1;
        style.left = kListMarkerWidth + kListStep * (level - 1) + 4;
        style.first_line_indent = -kListMarkerWidth;
        style.space_after = body_size / 6;
        break;
    }
    case ParaKind::Code:
        style.face = Face::Mono;
        style.left = kCodeIndent;
        style.right = kCodeIndent;
        style.preserve_spaces = true;
        break;
    default:
        style.space_after = std::max(4, body_size * 9 / 20);
        break;
    }

    const FontFaceData& face_data = find_font(style.size_index, style.face);
    style.line_height = face_data.line_height;
    style.ascent = face_data.ascent;
    // Hyphenate body text only: headings, quotes, lists and code stay ragged.
    if (paragraph.kind == ParaKind::Text || paragraph.kind == ParaKind::Quote) {
        const Glyph* hyphen = find_glyph(face_data, '-');
        if (hyphen != nullptr) style.hyphen_advance = hyphen->advance;
    }
    return style;
}

// Paragraph kind forces a base face; inline markup upgrades it.
Face combine_face(ParaKind kind, Face run_face) {
    Face base = Face::Regular;
    switch (kind) {
    case ParaKind::Heading:
        base = Face::Bold;
        break;
    case ParaKind::Quote:
        base = Face::Italic;
        break;
    case ParaKind::Code:
        return Face::Mono;
    default:
        base = Face::Regular;
        break;
    }
    if (base == Face::Regular) return run_face;
    if (base == Face::Bold) {
        if (run_face == Face::Italic || run_face == Face::BoldItalic) return Face::BoldItalic;
        return Face::Bold;
    }
    if (base == Face::Italic) {
        if (run_face == Face::Bold || run_face == Face::BoldItalic) return Face::BoldItalic;
        return Face::Italic;
    }
    return base;
}

// Marks hyphenation points for the words of one run. A point is stored on the
// char that would start the next line, so the wrapper can cut the word there.
void mark_hyphenation_points(const std::string& text, size_t run_begin, std::vector<CharRef>& chars);

void build_chars(const Paragraph& paragraph, uint32_t skip_chars, int size_index, std::vector<CharRef>& out) {
    uint32_t cp_index = 0; // counts every codepoint, including skipped ones
    out.clear();
    for (size_t run_index = 0; run_index < paragraph.runs.size(); ++run_index) {
        const Run& run = paragraph.runs[run_index];
        const Face face = combine_face(paragraph.kind, run.style.face);
        const FontFaceData& font = find_font(size_index, face);
        const size_t run_begin = out.size();
        size_t pos = 0;
        while (pos < run.text.size()) {
            size_t used = 0;
            const uint32_t cp = utf8_decode(run.text.data() + pos, run.text.size() - pos, &used);
            if (used == 0) break;
            if (cp_index < skip_chars) {
                ++cp_index;
                pos += used;
                continue;
            }
            if (is_invisible_codepoint(cp)) {
                ++cp_index;
                pos += used;
                continue;
            }
            CharRef ref;
            ref.cp = cp;
            ref.run = static_cast<uint16_t>(run_index);
            ref.begin = static_cast<uint32_t>(pos);
            ref.end = static_cast<uint32_t>(pos + used);
            ref.cp_index = cp_index++;
            ref.size_index = static_cast<uint8_t>(size_index);
            ref.face = face;
            // Byte range of this codepoint inside the run: a link that covers it
            // marks the char, so the renderer can underline the whole reference.
            for (const Link& link : paragraph.links) {
                if (link.run == run_index && pos >= link.begin && pos < link.end) {
                    // Unresolved references are still focusable (the panel reports the
                    // missing text), so they get a non-zero sentinel.
                    ref.link = link.note == 0 ? kUnresolvedLink : link.note;
                    break;
                }
            }
            if (cp == '\t') {
                const Glyph* space = find_glyph(font, ' ');
                ref.advance = static_cast<uint16_t>((space != nullptr ? space->advance : font.pixel_size / 2) * 4);
                ref.space = true;
            } else if (cp == '\n' || cp == '\r') {
                ref.advance = 0;
                ref.space = true;
            } else {
                int fallback = 0;
                const Glyph* glyph = glyph_for_display(font, cp, &fallback);
                ref.advance = static_cast<uint16_t>(glyph != nullptr ? glyph->advance : fallback);
                ref.space = cp == ' ' || is_space_codepoint(cp);
            }
            out.push_back(ref);
            pos += used;
        }
        mark_hyphenation_points(run.text, run_begin, out);
    }
}

// Marks hyphenation points for the words of one run. A point is stored on the
// char that would start the next line, so the wrapper can cut the word there.
void mark_hyphenation_points(const std::string& text, size_t run_begin, std::vector<CharRef>& chars) {
    uint32_t points[24];
    size_t index = run_begin;
    while (index < chars.size()) {
        // Skip to the start of a word.
        while (index < chars.size() && (chars[index].space || chars[index].cp == 0x2014 || chars[index].cp == 0x2013)) {
            ++index;
        }
        if (index >= chars.size()) break;

        const size_t word_start = index;
        while (index < chars.size() && !chars[index].space) ++index;
        const size_t word_end = index;
        if (word_end - word_start < 4) continue;

        const size_t byte_begin = chars[word_start].begin;
        const size_t byte_end = chars[word_end - 1].end;
        if (byte_end > text.size() || byte_begin >= byte_end) continue;

        const size_t count =
            hyphenation_points(text.data() + byte_begin, byte_end - byte_begin, points, sizeof(points) / sizeof(points[0]));
        for (size_t p = 0; p < count; ++p) {
            const uint32_t offset = static_cast<uint32_t>(byte_begin + points[p]);
            for (size_t c = word_start + 1; c < word_end; ++c) {
                if (chars[c].begin == offset) {
                    chars[c].hyphen_break = true;
                    break;
                }
            }
        }
    }
}

// Splits one display line out of `chars` starting at `pos`. Returns the index of
// the first char of the next line, fills `line` and its pixel width.
size_t build_line(const std::vector<CharRef>& chars, size_t pos, int available_width, bool preserve_spaces,
                  std::vector<CharRef>& line, int* line_width, bool* hyphenated, int hyphen_advance) {
    if (!preserve_spaces) {
        while (pos < chars.size() && (chars[pos].cp == ' ' || chars[pos].cp == 0xA0 || chars[pos].cp == '\n' ||
                                      chars[pos].cp == '\r')) {
            ++pos;
        }
    }

    size_t index = pos;
    size_t last_space = kNoSpace;
    int last_space_width = 0; // width of the text before that space
    int width = 0;
    bool newline = false;
    while (index < chars.size()) {
        if (chars[index].cp == '\n' || chars[index].cp == '\r') {
            newline = true;
            ++index;
            break;
        }
        const int advance = chars[index].advance;
        if (index > pos && width + advance > available_width) {
            if (last_space != kNoSpace && last_space > pos) {
                // Try to keep the whole word by hyphenating it instead of leaving
                // a ragged gap: pick the longest allowed prefix that still fits.
                if (!newline && hyphen_advance > 0) {
                    size_t best = kNoSpace;
                    int prefix_width = last_space_width;
                    for (size_t c = last_space + 1; c < index; ++c) {
                        if (chars[c].hyphen_break && prefix_width + hyphen_advance <= available_width) {
                            best = c;
                        }
                        prefix_width += chars[c].advance;
                    }
                    if (best != kNoSpace) {
                        line.assign(chars.begin() + static_cast<std::ptrdiff_t>(pos),
                                    chars.begin() + static_cast<std::ptrdiff_t>(best));
                        *line_width = 0;
                        for (const CharRef& ref : line) *line_width += ref.advance;
                        *line_width += hyphen_advance;
                        *hyphenated = true;
                        return best;
                    }
                }
                index = last_space;
            }
            break;
        }
        if (chars[index].space) {
            last_space = index;
            last_space_width = width;
        }
        width += advance;
        ++index;
    }

    size_t end = newline && index > pos ? index - 1 : index;
    if (!preserve_spaces) {
        while (end > pos && (chars[end - 1].cp == ' ' || chars[end - 1].cp == 0xA0)) {
            width -= chars[end - 1].advance;
            --end;
        }
    }
    line.assign(chars.begin() + static_cast<std::ptrdiff_t>(pos), chars.begin() + static_cast<std::ptrdiff_t>(end));
    *line_width = width;

    size_t next = end;
    if (next < chars.size() && (chars[next].cp == ' ' || chars[next].cp == 0xA0 || chars[next].cp == '\n' ||
                                chars[next].cp == '\r')) {
        ++next;
    }
    return next;
}

} // namespace

Paginator::Paginator(Document& doc, LayoutMetrics metrics) : _doc(doc), _metrics(metrics) {}

void Paginator::set_metrics(const LayoutMetrics& metrics) {
    _metrics = metrics;
}

void Paginator::reset_state() {
    _queue.clear();
    _exhausted = false;
    _placed_pages = 0;
    _next_position = PagePos{};
}

uint32_t Paginator::paragraph_chars(const Paragraph& paragraph) const {
    uint32_t count = 0;
    for (const Run& run : paragraph.runs) {
        count += utf8_length(run.text);
    }
    return count;
}

bool Paginator::fetch_paragraph() {
    Paragraph paragraph;
    if (!_doc.next(paragraph)) return false;
    QueuedParagraph queued;
    queued.paragraph = std::move(paragraph);
    _queue.push_back(std::move(queued));
    return true;
}

bool Paginator::restart() {
    reset_state();
    _doc.rewind();
    return true;
}

bool Paginator::seek(PagePos position) {
    reset_state();
    if (position.token == 0 && position.char_offset == 0) {
        _doc.rewind();
    } else if (!_doc.seek(position.token)) {
        _doc.rewind();
        position = PagePos{};
    }
    if (!fetch_paragraph()) {
        _exhausted = true;
        return false;
    }
    _queue.front().char_offset = position.char_offset;
    _queue.front().started = position.char_offset > 0;
    _next_position = PagePos{_queue.front().paragraph.token, position.char_offset};
    return true;
}

bool Paginator::next(PageView& page) {
    page = PageView();
    const int page_top = _metrics.y;
    const int page_bottom = _metrics.y + _metrics.height;
    int y = page_top;
    bool placed_any = false;
    bool started = false;

    while (true) {
        if (_queue.empty()) {
            if (_exhausted) break;
            if (!fetch_paragraph()) {
                _exhausted = true;
                break;
            }
            continue;
        }

        QueuedParagraph& queued = _queue.front();
        const Paragraph& paragraph = queued.paragraph;

        if (paragraph.kind == ParaKind::Blank) {
            const int gap = find_font(_metrics.size_index, Face::Regular).pixel_size;
            if (placed_any && y + gap > page_bottom) break;
            if (!started) {
                page.start = PagePos{paragraph.token, 0};
                started = true;
            }
            y += gap;
            _queue.pop_front();
            if (!_queue.empty()) {
                _next_position = PagePos{_queue.front().paragraph.token, _queue.front().char_offset};
            } else if (!_exhausted && fetch_paragraph()) {
                _next_position = PagePos{_queue.front().paragraph.token, 0};
            } else {
                _next_position = PagePos{paragraph.token, 0};
            }
            continue;
        }

        if (paragraph.kind == ParaKind::Separator) {
            const int height = std::max(14, find_font(_metrics.size_index, Face::Regular).pixel_size / 2);
            if (placed_any && y + height > page_bottom) break;
            if (!started) {
                page.start = PagePos{paragraph.token, 0};
                started = true;
            }
            LineView line;
            line.kind = ParaKind::Separator;
            line.x = static_cast<int16_t>(_metrics.x);
            line.y = static_cast<int16_t>(y);
            line.height = static_cast<int16_t>(height);
            line.baseline = static_cast<int16_t>(y + height / 2);
            page.lines.push_back(std::move(line));
            y += height + 6;
            placed_any = true;
            _queue.pop_front();
            if (!_queue.empty()) {
                _next_position = PagePos{_queue.front().paragraph.token, _queue.front().char_offset};
            } else if (!_exhausted && fetch_paragraph()) {
                _next_position = PagePos{_queue.front().paragraph.token, 0};
            } else {
                _next_position = PagePos{paragraph.token, 0};
            }
            continue;
        }

        if (paragraph.runs.empty()) {
            _queue.pop_front();
            continue;
        }

        const ParaStyle style = style_for(paragraph, _metrics);
        if (!started) {
            page.start = PagePos{paragraph.token, queued.char_offset};
            started = true;
        }

        const uint32_t base_offset = queued.char_offset;
        std::vector<CharRef> chars;
        build_chars(paragraph, base_offset, style.size_index, chars);
        if (chars.empty()) {
            _queue.pop_front();
            continue;
        }

        const uint16_t page_paragraph = static_cast<uint16_t>(page.paragraphs.size());
        page.paragraphs.push_back(paragraph);

        if (placed_any || y > page_top) {
            y += style.space_before;
        }

        size_t pos = 0;
        bool first_line = true;
        bool paragraph_finished = false;
        uint32_t consumed_cp = base_offset;
        int placed_in_paragraph = 0;

        // Orphan control: do not leave a single line of a paragraph at the bottom
        // of the page. If the paragraph needs two lines and only one fits, start
        // it on the next page instead (cheap: one width sum per paragraph).
        if (placed_any && queued.char_offset == 0 && y > page_top) {
            int total = 0;
            for (const CharRef& ref : chars) total += ref.advance;
            const int first_available = _metrics.width - (style.left + style.first_line_indent) - style.right;
            const bool needs_two_lines =
                total > (first_available > 40 ? first_available : 40) && total > 40;
            if (needs_two_lines && y + 2 * style.line_height > page_bottom) {
                _next_position = PagePos{paragraph.token, queued.char_offset};
                break;
            }
        }

        while (pos < chars.size()) {
            if (y + style.line_height > page_bottom && placed_any) break;

            const int line_left = style.left + (first_line ? style.first_line_indent : 0);
            int available = _metrics.width - line_left - style.right;
            if (available < 40) available = 40;

            const size_t line_start = pos;
            const int line_top = y;
            std::vector<CharRef> line_chars;
            int line_width = 0;
            bool hyphenated = false;
            size_t next = build_line(chars, pos, available, style.preserve_spaces, line_chars, &line_width,
                                     &hyphenated, style.hyphen_advance);
            if (next <= pos) next = pos + 1;
            consumed_cp = chars[next - 1].cp_index + 1;

            if (!line_chars.empty()) {
                LineView line;
                line.kind = paragraph.kind;
                line.level = paragraph.level;
                line.code = paragraph.kind == ParaKind::Code;
                line.quote = paragraph.kind == ParaKind::Quote;
                line.quote_rule_x = static_cast<int16_t>(_metrics.x + 4);
                line.y = static_cast<int16_t>(y);
                line.height = static_cast<int16_t>(style.line_height);
                line.baseline = static_cast<int16_t>(y + style.ascent);
                line.x = static_cast<int16_t>(_metrics.x + line_left);
                if (paragraph.kind == ParaKind::ListItem && first_line) {
                    line.show_marker = !paragraph.marker.empty();
                    line.marker_x = static_cast<int16_t>(_metrics.x + style.left - kListMarkerWidth);
                }

                int span_x = line.x;
                SpanView span;
                bool open = false;
                // Reference rectangles: a link may continue on the next line, in which
                // case each line gets its own segment.
                LinkView current_link;
                bool link_open = false;
                const int link_base = span_x;
                for (const CharRef& ref : line_chars) {
                    if (link_open && ref.link != 0 && current_link.run == ref.run &&
                        ref.begin == current_link.end) {
                        current_link.end = ref.end; // same reference continues
                    } else if (!link_open || current_link.note != ref.link) {
                        if (link_open) {
                            current_link.width = static_cast<int16_t>(span_x - current_link.x);
                            if (current_link.width > 0) line.links.push_back(current_link);
                            link_open = false;
                        }
                        if (ref.link != 0) {
                            current_link = LinkView{};
                            current_link.note = ref.link == kUnresolvedLink ? 0 : ref.link;
                            current_link.x = static_cast<int16_t>(span_x);
                            current_link.para = page_paragraph;
                            current_link.run = ref.run;
                            current_link.begin = ref.begin;
                            current_link.end = ref.end;
                            link_open = true;
                        }
                    }
                    if (!open || span.run != ref.run || span.end != ref.begin) {
                        if (open) {
                            span.width = static_cast<int16_t>(span_x - span.x);
                            line.spans.push_back(span);
                        }
                        span = SpanView{};
                        span.run = ref.run;
                        span.para = page_paragraph;
                        span.begin = ref.begin;
                        span.end = ref.end;
                        span.x = static_cast<int16_t>(span_x);
                        span.size_index = ref.size_index;
                        span.face = ref.face;
                        open = true;
                    } else {
                        span.end = ref.end;
                    }
                    span_x += ref.advance;
                }
                if (link_open) {
                    current_link.width = static_cast<int16_t>(span_x - current_link.x);
                    if (current_link.width > 0) line.links.push_back(current_link);
                }
                if (open) {
                    span.width = static_cast<int16_t>(span_x - span.x);
                    line.spans.push_back(span);
                }
                (void)link_base;

                page.lines.push_back(std::move(line));
                page.lines.back().hyphenated = hyphenated;
                y += style.line_height;
                placed_any = true;
                ++placed_in_paragraph;
            }

            pos = next;
            first_line = false;
            if (pos >= chars.size()) {
                paragraph_finished = true;
                break;
            }

            // Widow control: if the rest of the paragraph fits on a single line,
            // move the line we just placed to the next page as well, so that page
            // does not start with one lonely line.
            if (placed_in_paragraph >= 1 && y + style.line_height > page_bottom) {
                int rest = 0;
                for (size_t c = pos; c < chars.size(); ++c) rest += chars[c].advance;
                const int rest_available = _metrics.width - style.left - style.right;
                if (rest <= (rest_available > 40 ? rest_available : 40) && rest > 0) {
                    page.lines.pop_back();
                    y = line_top;
                    pos = line_start;
                    // The line moves to the next page: the paragraph must resume
                    // exactly at its first char.
                    consumed_cp = chars[pos].cp_index;
                    --placed_in_paragraph;
                    break;
                }
            }
        }

        queued.char_offset = consumed_cp;
        if (paragraph_finished && queued.char_offset >= paragraph_chars(paragraph)) {
            _queue.pop_front();
            if (!_queue.empty()) {
                _next_position = PagePos{_queue.front().paragraph.token, _queue.front().char_offset};
            } else if (!_exhausted && fetch_paragraph()) {
                _next_position = PagePos{_queue.front().paragraph.token, 0};
            } else {
                _next_position = PagePos{paragraph.token, paragraph_chars(paragraph)};
            }
        } else {
            _next_position = PagePos{paragraph.token, queued.char_offset};
            break; // page is full
        }

        y += style.space_after;
    }

    if (!started) {
        page.at_end = true;
        return false;
    }

    page.end = _next_position;
    page.permille = _doc.permille(page.start.token);
    page.at_end = _exhausted && _queue.empty() && page.end == PagePos{};
    if (page.at_end) {
        page.permille = 1000;
    }
    if (page.permille > 1000) page.permille = 1000;
    ++_placed_pages;
    return !page.lines.empty();
}

} // namespace reader
