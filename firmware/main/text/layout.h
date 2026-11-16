// Page layout: turns paragraphs into positioned lines for the current screen.
#pragma once

#include <cstdint>
#include <deque>
#include <vector>

#include "text/doc.h"
#include "text/font.h"

namespace reader {

struct LayoutMetrics {
    int x = 20;
    int y = 8;
    int width = 500;
    int height = 640;
    int size_index = 1; // 0..2
};

struct SpanView {
    uint16_t run = 0;   // index into PageView::paragraphs[para].runs
    uint16_t para = 0;  // index into PageView::paragraphs
    uint32_t begin = 0; // byte offset inside run text
    uint32_t end = 0;   // byte offset (exclusive)
    int16_t x = 0;      // absolute pixel x
    int16_t width = 0;  // measured width
    uint8_t size_index = 1;
    Face face = Face::Regular;
};

// One reference found on a line: its note number, the pixel range it occupies and
// the source bytes, so the UI can show the reference label exactly as printed
// ("[1]") instead of the internal note index.
struct LinkView {
    uint32_t note = 0;
    int16_t x = 0;
    int16_t width = 0;
    uint16_t para = 0;
    uint16_t run = 0;
    uint32_t begin = 0;
    uint32_t end = 0;
};

struct LineView {
    std::vector<SpanView> spans;
    std::vector<LinkView> links;
    int16_t x = 0; // absolute left edge of content
    int16_t y = 0; // absolute top edge of the line box
    int16_t baseline = 0;
    int16_t height = 0;
    ParaKind kind = ParaKind::Text;
    uint8_t level = 0;
    bool code = false;  // draw code background
    bool quote = false; // draw quote rule at quote_rule_x
    int16_t quote_rule_x = 0;
    bool show_marker = false; // first line of a list item
    int16_t marker_x = 0;
    bool hyphenated = false; // the line ends with a hyphenation break
};

struct PageView {
    std::vector<Paragraph> paragraphs;
    std::vector<LineView> lines;
    PagePos start;
    PagePos end;
    uint32_t permille = 0;
    bool at_end = false;
};

// Layouts one page at a time from a document stream, keeping the partial
// paragraph between pages so no text is lost or repeated.
class Paginator {
  public:
    Paginator(Document& doc, LayoutMetrics metrics);

    void set_metrics(const LayoutMetrics& metrics);
    const LayoutMetrics& metrics() const { return _metrics; }

    // Starts at the beginning of the document.
    bool restart();

    // Starts at an arbitrary position (saved progress).
    bool seek(PagePos position);

    // Layouts the next page. Returns false when the document is exhausted.
    bool next(PageView& page);

    // Position where the next page will start (valid after next()).
    PagePos next_position() const { return _next_position; }
    bool exhausted() const { return _exhausted && _queue.empty(); }

  private:
    struct QueuedParagraph {
        Paragraph paragraph;
        uint32_t char_offset = 0; // consumed codepoints
        bool started = false;     // some chars already placed on a previous page
    };

    bool fetch_paragraph();
    void reset_state();
    uint32_t paragraph_chars(const Paragraph& paragraph) const;

    Document& _doc;
    LayoutMetrics _metrics;
    std::deque<QueuedParagraph> _queue;
    PagePos _next_position;
    bool _exhausted = false;
    uint32_t _placed_pages = 0;
};

} // namespace reader
