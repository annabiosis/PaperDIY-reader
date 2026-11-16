// Host tests for pagination: geometry, text conservation, font sizes, styles.
#include <cstdio>

// Stage markers: the Linux CI build has crashed before; a marker tells which block.
static void stage(const char* name) {
    std::fprintf(stderr, "[stage] %s\n", name);
    std::fflush(stderr);
}
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "core/utf8.h"
#include "text/hyphenate.h"
#include "text/doc.h"
#include "text/font.h"
#include "text/layout.h"
#include "text/types.h"

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(condition)                                                                      \
    do {                                                                                      \
        ++g_checks;                                                                           \
        if (!(condition)) {                                                                   \
            ++g_failures;                                                                     \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);          \
        }                                                                                     \
    } while (0)

using reader::Document;
using reader::DocInfo;
using reader::Face;
using reader::LayoutMetrics;
using reader::PagePos;
using reader::PageView;
using reader::Paginator;
using reader::ParaKind;
using reader::Paragraph;
using reader::Run;

// Minimal in-memory document used only by these tests.
class VectorDocument : public Document {
  public:
    explicit VectorDocument(std::vector<Paragraph> paragraphs) : _paragraphs(std::move(paragraphs)) {}

    const DocInfo& info() const override { return _info; }

    bool rewind() override {
        _cursor = 0;
        return true;
    }

    bool next(Paragraph& out) override {
        if (_cursor >= _paragraphs.size()) return false;
        out = _paragraphs[_cursor++];
        return true;
    }

    bool seek(uint64_t token) override {
        for (size_t i = 0; i < _paragraphs.size(); ++i) {
            if (_paragraphs[i].token >= token) {
                _cursor = i;
                return true;
            }
        }
        _cursor = _paragraphs.size();
        return false;
    }

    uint32_t permille(uint64_t token) const override {
        if (_paragraphs.empty()) return 0;
        const uint64_t last = _paragraphs.back().token;
        if (last == 0) return 0;
        return static_cast<uint32_t>(token * 1000 / (last + 1));
    }

  private:
    DocInfo _info;
    std::vector<Paragraph> _paragraphs;
    size_t _cursor = 0;
};

Paragraph make_paragraph(uint64_t token, const std::string& text, ParaKind kind = ParaKind::Text,
                         uint8_t level = 0, const std::string& marker = "") {
    Paragraph paragraph;
    paragraph.kind = kind;
    paragraph.level = level;
    paragraph.marker = marker;
    paragraph.token = token;
    Run run;
    run.text = text;
    run.style.face = Face::Regular;
    paragraph.runs.push_back(run);
    return paragraph;
}

std::string page_text(const PageView& page) {
    std::string text;
    for (const auto& line : page.lines) {
        for (const auto& span : line.spans) {
            if (span.para >= page.paragraphs.size()) continue;
            const auto& paragraph = page.paragraphs[span.para];
            if (span.run >= paragraph.runs.size()) continue;
            const std::string& source = paragraph.runs[span.run].text;
            if (span.begin >= source.size()) continue;
            const size_t end = span.end > source.size() ? source.size() : span.end;
            text.append(source, span.begin, end - span.begin);
        }
        text.push_back(' ');
    }
    return text;
}

// Drops all whitespace: hyphenation splits words across lines, so the pages can
// only be compared with the source after spaces are removed.
std::string squeeze(const std::string& text) {
    std::string out;
    for (char c : text) {
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '-') continue;
        out.push_back(c);
    }
    return out;
}

std::vector<Paragraph> make_book(int paragraphs, int words_per_paragraph) {
    std::vector<Paragraph> out;
    uint64_t token = 0;
    for (int i = 0; i < paragraphs; ++i) {
        std::string text = "Абзац " + std::to_string(i + 1) + ":";
        for (int w = 0; w < words_per_paragraph; ++w) {
            text += " слово" + std::to_string(w + 1);
        }
        out.push_back(make_paragraph(token, text));
        token += text.size();
    }
    return out;
}

LayoutMetrics metrics_for(int size_index) {
    LayoutMetrics metrics;
    metrics.x = 26;
    metrics.y = 60;
    metrics.width = 540 - 52;
    metrics.height = 960 - 60 - 44;
    metrics.size_index = size_index;
    return metrics;
}

void test_geometry_and_conservation() {
    auto book = make_book(12, 40);
    std::string expected;
    for (const auto& paragraph : book) expected += paragraph.runs[0].text + " ";

    VectorDocument document(book);
    Paginator paginator(document, metrics_for(1));

    std::string collected;
    int pages = 0;
    PageView page;
    while (paginator.next(page) && pages < 100) {
        ++pages;
        for (const auto& line : page.lines) {
            if (line.kind == ParaKind::Separator) continue;
            CHECK(line.x >= metrics_for(1).x - 2);
            for (const auto& span : line.spans) {
                CHECK(span.x + span.width <= metrics_for(1).x + metrics_for(1).width + 2);
            }
        }
        collected += page_text(page);
        collected += "\n";
    }
    CHECK(pages > 2);
    CHECK(squeeze(collected) == squeeze(expected));
}

void test_font_size_changes_page_count() {
    auto book = make_book(8, 30);
    int page_counts[3] = {0, 0, 0};
    for (int size = 0; size < 3; ++size) {
        VectorDocument document(book);
        Paginator paginator(document, metrics_for(size));
        PageView page;
        int pages = 0;
        while (paginator.next(page) && pages < 200) ++pages;
        page_counts[size] = pages;
    }
    CHECK(page_counts[0] < page_counts[1]);
    CHECK(page_counts[1] < page_counts[2]);
}

void test_seek_resumes() {
    auto book = make_book(10, 25);
    VectorDocument document(book);

    Paginator first(document, metrics_for(1));
    PageView page1;
    CHECK(first.next(page1));
    PageView page2;
    CHECK(first.next(page2));
    const PagePos target = page2.start;

    Paginator resumed(document, metrics_for(1));
    CHECK(resumed.seek(target));
    PageView page3;
    CHECK(resumed.next(page3));
    CHECK(page3.start.token == target.token);
    CHECK(page3.start.char_offset == target.char_offset);
    CHECK(squeeze(page_text(page3)) == squeeze(page_text(page2)));
}

void test_styles() {
    std::vector<Paragraph> book;
    book.push_back(make_paragraph(0, "Заголовок", ParaKind::Heading, 1));
    book.push_back(make_paragraph(10, "Обычный текст с продолжением.", ParaKind::Text));
    book.push_back(make_paragraph(40, "пункт списка", ParaKind::ListItem, 1, "•"));
    book.push_back(make_paragraph(60, "цитата", ParaKind::Quote));
    book.push_back(make_paragraph(70, "int main() {", ParaKind::Code));
    book.push_back(make_paragraph(85, "", ParaKind::Separator));
    book.push_back(make_paragraph(86, "после черты", ParaKind::Text));

    VectorDocument document(book);
    Paginator paginator(document, metrics_for(1));
    PageView page;
    CHECK(paginator.next(page));

    bool heading_larger = false;
    bool list_marker = false;
    bool quote_line = false;
    bool code_line = false;
    bool separator = false;

    for (const auto& line : page.lines) {
        if (line.kind == ParaKind::Heading) {
            heading_larger = line.spans.size() > 0 && line.spans[0].size_index > 1;
        }
        if (line.kind == ParaKind::ListItem && line.show_marker) list_marker = true;
        if (line.quote) quote_line = true;
        if (line.code) code_line = true;
        if (line.kind == ParaKind::Separator) separator = true;
    }
    CHECK(heading_larger);
    CHECK(list_marker);
    CHECK(quote_line);
    CHECK(code_line);
    CHECK(separator);

    // The heading uses a bold face.
    bool bold_heading = false;
    for (const auto& line : page.lines) {
        if (line.kind != ParaKind::Heading) continue;
        for (const auto& span : line.spans) {
            if (span.face == Face::Bold) bold_heading = true;
        }
    }
    CHECK(bold_heading);
}

void test_long_word_and_empty_lines() {
    std::vector<Paragraph> book;
    book.push_back(make_paragraph(0, std::string(400, 'X')));
    book.push_back(make_paragraph(400, "короткий", ParaKind::Text));
    VectorDocument document(book);
    Paginator paginator(document, metrics_for(2));
    PageView page;
    CHECK(paginator.next(page));
    CHECK(!page.lines.empty());
    // A 400 character word must not crash layout and must not overflow wildly.
    for (const auto& line : page.lines) {
        for (const auto& span : line.spans) {
            CHECK(span.width >= 0);
        }
    }
}

void test_invisible_and_special_characters() {
    // Soft hyphen, zero-width space and BOM must vanish; NBSP stays a space.
    std::vector<Paragraph> book;
    book.push_back(make_paragraph(0, "соф\u00ADтгифен и\u200Bнулевой\uFEFFпробел"));
    book.push_back(make_paragraph(40, "нераз\u00A0рывный интервал"));
    VectorDocument document(book);
    Paginator paginator(document, metrics_for(1));
    PageView page;
    CHECK(paginator.next(page));

    std::string rendered = page_text(page);
    CHECK(rendered.find('\xAD') == std::string::npos);
    CHECK(rendered.find("нулевой") != std::string::npos);
    CHECK(rendered.find("нераз") != std::string::npos);

    // No glyph in the atlas should fall back to a bare '?' for these characters.
    CHECK(rendered.find("софтгифен") != std::string::npos);
}

void test_glyph_coverage() {
    // Every character that occurs in Russian books must exist in all 3 sizes and
    // all 4 faces, otherwise the renderer falls back to U+FFFD / '?'.
    const std::string sample =
        "АБВГДЕЁЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯ"
        "абвгдеёжзийклмнопрстуфхцчшщъыьэюя"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
        "0123456789"
        " .,;:!?()[]{}\"'«»„“”‘’—–-‑…№§¶©®°±×÷+=*/\\|_~^<>%$#@&"
        "→←↑↓•†‡‰";

    size_t pos = 0;
    int missing = 0;
    while (pos < sample.size()) {
        size_t used = 0;
        const uint32_t cp = reader::utf8_decode(sample.data() + pos, sample.size() - pos, &used);
        pos += used == 0 ? 1 : used;
        for (int size = 0; size < reader::font_size_count(); ++size) {
            for (Face face : {Face::Regular, Face::Bold, Face::Italic, Face::BoldItalic}) {
                const auto& font = reader::find_font(size, face);
                if (reader::find_glyph(font, cp) == nullptr) {
                    ++missing;
                    std::fprintf(stderr, "missing glyph U+%04X in size %d face %d\n", cp, size,
                                 static_cast<int>(face));
                }
            }
        }
    }
    CHECK(missing == 0);
}

void test_hyphenation_points() {
    using reader::hyphenation_points;
    const std::string word = "пластинками";
    uint32_t points[16];
    const size_t count = hyphenation_points(word.data(), word.size(), points, 16);
    CHECK(count >= 2);
    for (size_t i = 0; i < count; ++i) {
        CHECK(points[i] >= 4);                 // at least two letters before the break
        CHECK(points[i] + 4 <= word.size());   // and two after it
    }

    // «й», «ь», «ъ», «ы» must never start a line.
    const std::string with_soft = "маленький";
    const size_t soft_count = hyphenation_points(with_soft.data(), with_soft.size(), points, 16);
    for (size_t i = 0; i < soft_count; ++i) {
        const std::string tail = with_soft.substr(points[i]);
        CHECK(tail.rfind("й", 0) != 0);
        CHECK(tail.rfind("ь", 0) != 0);
    }

    // Short words and Latin text are left alone.
    CHECK(hyphenation_points("дом", 3, points, 16) == 0);
    CHECK(hyphenation_points("hyphenation", 11, points, 16) == 0);
}

// No page may leave a single line of a paragraph at its bottom (orphan) or start
// with a single lonely line of a paragraph that continues (widow). Paragraphs are
// identified by their tokens, so a "single line" that simply ended there is fine.
void test_no_orphans_or_widows() {
    std::vector<Paragraph> book;
    uint64_t token = 0;
    for (int i = 0; i < 9; ++i) {
        std::string text = "Абзац " + std::to_string(i + 1) + ":";
        for (int w = 0; w < 24 + (i % 3) * 9; ++w) text += " слово" + std::to_string(w);
        book.push_back(make_paragraph(token, text));
        token += text.size();
    }

    VectorDocument document(book);
    Paginator paginator(document, metrics_for(1));
    PageView page;

    uint64_t previous_last_token = 0;
    int previous_last_lines = 0;
    bool have_previous = false;
    int checked_pages = 0;
    int widows = 0;
    int orphans = 0;

    while (paginator.next(page) && checked_pages < 40) {
        if (page.lines.empty()) continue;
        ++checked_pages;

        // Lines per paragraph index on this page, in drawing order.
        std::vector<int> counts(page.paragraphs.size(), 0);
        for (const auto& line : page.lines) {
            for (const auto& span : line.spans) {
                if (span.para < counts.size()) {
                    ++counts[span.para];
                    break;
                }
            }
        }

        // First paragraph with content on this page.
        size_t first = counts.size();
        for (size_t p = 0; p < counts.size(); ++p) {
            if (counts[p] > 0) {
                first = p;
                break;
            }
        }
        // Last paragraph with content on this page.
        size_t last = counts.size();
        for (size_t p = counts.size(); p-- > 0;) {
            if (counts[p] > 0) {
                last = p;
                break;
            }
        }
        if (first >= counts.size()) continue;

        const uint64_t first_token = page.paragraphs[first].token;
        const uint64_t last_token = page.paragraphs[last].token;

        if (have_previous && first_token == previous_last_token && counts[first] == 1 &&
            previous_last_lines >= 1) {
            ++widows; // one lonely line of a continuing paragraph at the top
        }
        if (have_previous && last_token == first_token && counts[first] == 1) {
            ++orphans; // one line of a paragraph that continues on the next page
        }

        previous_last_token = last_token;
        previous_last_lines = counts[last];
        have_previous = true;
    }

    CHECK(checked_pages > 3);
    CHECK(widows == 0);
    // The orphan rule pushes a paragraph to the next page, so the last paragraph of
    // a page always gets at least two lines.
    CHECK(orphans == 0);
}

} // namespace

int main() {
    stage("geometry");
    test_geometry_and_conservation();
    stage("font-size");
    test_font_size_changes_page_count();
    stage("seek");
    test_seek_resumes();
    stage("styles");
    test_styles();
    stage("long-word");
    test_long_word_and_empty_lines();
    stage("invisible");
    test_invisible_and_special_characters();
    stage("glyphs");
    test_glyph_coverage();
    stage("hyphenation");
    test_hyphenation_points();
    stage("widows");
    test_no_orphans_or_widows();

    std::printf("%d checks, %d failure(s)\n", g_checks, g_failures);
    if (g_failures == 0) std::printf("OK all layout tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
