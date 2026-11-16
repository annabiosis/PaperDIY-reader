// Host-side tests for the Markdown parser.
// Build (from the repository root):
//   g++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -I ereader/firmware/main
//       -o ereader/tests/build/test_md.exe
//       ereader/firmware/main/core/utf8.cpp ereader/firmware/main/core/encoding.cpp
//       ereader/firmware/main/core/byte_source.cpp ereader/firmware/main/formats/markdown.cpp
//       ereader/tests/test_md.cpp

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/byte_source.h"
#include "core/utf8.h"
#include "formats/formats.h"
#include "text/doc.h"
#include "text/types.h"

namespace {

int g_checks = 0;
int g_failures = 0;

} // namespace

#define CHECK(cond)                                                                               \
    do {                                                                                          \
        ++g_checks;                                                                               \
        if (!(cond)) {                                                                            \
            ++g_failures;                                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                  \
        }                                                                                         \
    } while (0)

#define CHECK_STR(got, want)                                                                      \
    do {                                                                                          \
        ++g_checks;                                                                               \
        const std::string check_got = (got);                                                      \
        const std::string check_want = (want);                                                    \
        if (check_got != check_want) {                                                            \
            ++g_failures;                                                                         \
            std::fprintf(stderr, "FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__,             \
                         check_got.c_str(), check_want.c_str());                                  \
        }                                                                                         \
    } while (0)

#define CHECK_INT(got, want)                                                                      \
    do {                                                                                          \
        ++g_checks;                                                                               \
        const long long check_got = static_cast<long long>(got);                                  \
        const long long check_want = static_cast<long long>(want);                                \
        if (check_got != check_want) {                                                            \
            ++g_failures;                                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %lld != %lld\n", __FILE__, __LINE__, check_got,      \
                         check_want);                                                             \
        }                                                                                         \
    } while (0)

namespace {

using reader::Document;
using reader::Face;
using reader::MemoryByteSource;
using reader::ParaKind;
using reader::Paragraph;

std::unique_ptr<Document> open_text(const std::string& text) {
    std::string error;
    auto doc = reader::open_markdown(std::make_unique<MemoryByteSource>(text), &error);
    if (!doc) std::fprintf(stderr, "open_markdown failed: %s\n", error.c_str());
    CHECK(doc != nullptr);
    return doc;
}

std::vector<Paragraph> collect(Document& doc) {
    doc.rewind();
    std::vector<Paragraph> paragraphs;
    Paragraph paragraph;
    while (doc.next(paragraph)) paragraphs.push_back(paragraph);
    return paragraphs;
}

std::string para_text(const Paragraph& paragraph) {
    std::string text;
    for (const reader::Run& run : paragraph.runs) text += run.text;
    return text;
}

bool has_run(const Paragraph& paragraph, const std::string& text, Face face) {
    for (const reader::Run& run : paragraph.runs) {
        if (run.text == text && run.style.face == face) return true;
    }
    return false;
}

const Paragraph* find_para(const std::vector<Paragraph>& paragraphs, const std::string& text,
                           ParaKind kind) {
    for (const Paragraph& paragraph : paragraphs) {
        if (paragraph.kind == kind && para_text(paragraph) == text) return &paragraph;
    }
    return nullptr;
}

const Paragraph* find_heading(const std::vector<Paragraph>& paragraphs, const std::string& text) {
    return find_para(paragraphs, text, ParaKind::Heading);
}

const std::string kFixture = R"MD(# Main Title

Intro line one
continues here.

## Section Two ###

### Third

Setext One
==========

Setext Two
----------

Some **bold** and *italic* and ***both*** and `code` and ~~gone~~.

A [link text](https://example.com) and ![alt text](img.png) and ![](x.png).

Escaped \*not italic\* and \_not\_ and \[brackets\].

<div>HTML here</div><br>after break.

---

- first
  - nested two
    - nested three
- second
  continued text

1. one
2) two

> quoted line
> second line
>
> > inner quote

```cpp
int x = 1;
return x;
```

| A | B |
|---|---|
| 1 | 2 |

trailing paragraph
)MD";

void check_main_fixture() {
    auto doc = open_text(kFixture);
    if (!doc) return;

    CHECK_STR(doc->info().title, "Main Title");
    CHECK_INT(doc->total_chars(), reader::utf8_length(kFixture));

    std::vector<Paragraph> paragraphs = collect(*doc);
    CHECK_INT(paragraphs.size(), 24);
    CHECK(!paragraphs.empty());
    if (paragraphs.empty()) return;

    // --- headings ---------------------------------------------------------
    const Paragraph* h1 = find_heading(paragraphs, "Main Title");
    CHECK(h1 != nullptr);
    if (h1) CHECK_INT(h1->level, 1);

    const Paragraph* h2 = find_heading(paragraphs, "Section Two");
    CHECK(h2 != nullptr);
    if (h2) CHECK_INT(h2->level, 2);

    const Paragraph* h3 = find_heading(paragraphs, "Third");
    CHECK(h3 != nullptr);
    if (h3) CHECK_INT(h3->level, 3);

    const Paragraph* setext1 = find_heading(paragraphs, "Setext One");
    CHECK(setext1 != nullptr);
    if (setext1) CHECK_INT(setext1->level, 1);

    const Paragraph* setext2 = find_heading(paragraphs, "Setext Two");
    CHECK(setext2 != nullptr);
    if (setext2) CHECK_INT(setext2->level, 2);

    // --- paragraph joining and blank lines -------------------------------
    CHECK(find_para(paragraphs, "Intro line one continues here.", ParaKind::Text) != nullptr);
    CHECK(find_para(paragraphs, "trailing paragraph", ParaKind::Text) != nullptr);

    // --- inline markup ----------------------------------------------------
    const Paragraph* inline_para =
        find_para(paragraphs, "Some bold and italic and both and code and gone.", ParaKind::Text);
    CHECK(inline_para != nullptr);
    if (inline_para) {
        CHECK(has_run(*inline_para, "bold", Face::Bold));
        CHECK(has_run(*inline_para, "italic", Face::Italic));
        CHECK(has_run(*inline_para, "both", Face::BoldItalic));
        CHECK(has_run(*inline_para, "code", Face::Mono));
        CHECK(para_text(*inline_para).find('~') == std::string::npos);
    }

    const Paragraph* links = find_para(
        paragraphs, "A link text and [изображение: alt text] and [изображение].", ParaKind::Text);
    CHECK(links != nullptr);

    const Paragraph* escapes =
        find_para(paragraphs, "Escaped *not italic* and _not_ and [brackets].", ParaKind::Text);
    CHECK(escapes != nullptr);

    const Paragraph* html =
        find_para(paragraphs, "HTML here after break.", ParaKind::Text);
    CHECK(html != nullptr);

    // --- horizontal rule --------------------------------------------------
    int separators = 0;
    int blanks = 0;
    for (const Paragraph& paragraph : paragraphs) {
        if (paragraph.kind == ParaKind::Separator) ++separators;
        if (paragraph.kind == ParaKind::Blank) ++blanks;
    }
    CHECK_INT(separators, 1);
    CHECK_INT(blanks, 0);

    // --- lists ------------------------------------------------------------
    const Paragraph* first = find_para(paragraphs, "first", ParaKind::ListItem);
    CHECK(first != nullptr);
    if (first) {
        CHECK_INT(first->level, 1);
        CHECK_STR(first->marker, "•");
    }

    const Paragraph* nested2 = find_para(paragraphs, "nested two", ParaKind::ListItem);
    CHECK(nested2 != nullptr);
    if (nested2) {
        CHECK_INT(nested2->level, 2);
        CHECK_STR(nested2->marker, "•");
    }

    const Paragraph* nested3 = find_para(paragraphs, "nested three", ParaKind::ListItem);
    CHECK(nested3 != nullptr);
    if (nested3) {
        CHECK_INT(nested3->level, 3);
        CHECK_STR(nested3->marker, "•");
    }

    const Paragraph* second = find_para(paragraphs, "second continued text", ParaKind::ListItem);
    CHECK(second != nullptr);
    if (second) {
        CHECK_INT(second->level, 1);
        CHECK_STR(second->marker, "•");
    }

    const Paragraph* ordered1 = find_para(paragraphs, "one", ParaKind::ListItem);
    CHECK(ordered1 != nullptr);
    if (ordered1) {
        CHECK_INT(ordered1->level, 1);
        CHECK_STR(ordered1->marker, "1.");
    }

    const Paragraph* ordered2 = find_para(paragraphs, "two", ParaKind::ListItem);
    CHECK(ordered2 != nullptr);
    if (ordered2) {
        CHECK_INT(ordered2->level, 1);
        CHECK_STR(ordered2->marker, "2.");
    }

    // --- quotes -----------------------------------------------------------
    const Paragraph* quote = find_para(paragraphs, "quoted line second line", ParaKind::Quote);
    CHECK(quote != nullptr);
    if (quote) CHECK_INT(quote->level, 1);

    const Paragraph* inner_quote = find_para(paragraphs, "inner quote", ParaKind::Quote);
    CHECK(inner_quote != nullptr);
    if (inner_quote) CHECK_INT(inner_quote->level, 2);

    // --- fenced code ------------------------------------------------------
    std::vector<const Paragraph*> code;
    for (const Paragraph& paragraph : paragraphs) {
        if (paragraph.kind == ParaKind::Code) code.push_back(&paragraph);
    }
    CHECK_INT(code.size(), 2);
    if (code.size() == 2) {
        CHECK_STR(para_text(*code[0]), "int x = 1;");
        CHECK_STR(para_text(*code[1]), "return x;");
        CHECK(has_run(*code[0], "int x = 1;", Face::Mono));
        CHECK(has_run(*code[1], "return x;", Face::Mono));
    }

    // --- tables -----------------------------------------------------------
    CHECK(find_para(paragraphs, "A | B", ParaKind::Text) != nullptr);
    CHECK(find_para(paragraphs, "1 | 2", ParaKind::Text) != nullptr);
    for (const Paragraph& paragraph : paragraphs) {
        CHECK(para_text(paragraph).find("---") == std::string::npos);
    }

    // --- tokens, seek and permille ---------------------------------------
    for (size_t i = 0; i + 1 < paragraphs.size(); ++i) {
        CHECK(paragraphs[i].token < paragraphs[i + 1].token);
    }
    CHECK(paragraphs.front().token == 0);

    CHECK_INT(doc->permille(0), 0);
    CHECK_INT(doc->permille(kFixture.size()), 1000);
    for (size_t i = 0; i < paragraphs.size(); i += 5) {
        const uint64_t expected =
            static_cast<uint64_t>(paragraphs[i].token) * 1000u / kFixture.size();
        CHECK_INT(doc->permille(paragraphs[i].token), expected);
    }

    const size_t sample = paragraphs.size() / 2;
    const uint64_t sample_token = paragraphs[sample].token;
    CHECK(doc->seek(sample_token));
    Paragraph out;
    CHECK(doc->next(out));
    CHECK(out.token == sample_token);
    CHECK_STR(para_text(out), para_text(paragraphs[sample]));

    if (sample + 1 < paragraphs.size()) {
        CHECK(doc->seek(sample_token + 1));
        Paragraph next_para;
        CHECK(doc->next(next_para));
        CHECK(next_para.token == paragraphs[sample + 1].token);
    }

    CHECK(doc->rewind());
    Paragraph first_para;
    CHECK(doc->next(first_para));
    CHECK(first_para.token == paragraphs.front().token);
    CHECK_STR(para_text(first_para), para_text(paragraphs.front()));

    CHECK(!doc->seek(static_cast<uint64_t>(kFixture.size()) + 100u));
    Paragraph exhausted;
    CHECK(!doc->next(exhausted));
}

void check_cp1251() {
    std::string cp;
    const auto add = [&cp](std::initializer_list<unsigned char> bytes) {
        for (unsigned char byte : bytes) cp.push_back(static_cast<char>(byte));
    };

    cp += "# ";
    add({0xCF, 0xF0, 0xE8, 0xE2, 0xE5, 0xF2}); // Привет
    cp += "\n\n";
    add({0xD2, 0xE5, 0xEA, 0xF1, 0xF2}); // Текст
    cp += " **";
    add({0xE6, 0xE8, 0xF0, 0xED, 0xFB, 0xE9}); // жирный
    cp += "**.\n";

    auto doc = open_text(cp);
    if (!doc) return;

    CHECK_STR(doc->info().title, "Привет");
    const std::string expected = "# Привет\n\nТекст **жирный**.\n";
    CHECK_INT(doc->total_chars(), reader::utf8_length(expected));

    std::vector<Paragraph> paragraphs = collect(*doc);
    CHECK_INT(paragraphs.size(), 2);
    if (paragraphs.size() >= 2) {
        CHECK_INT(paragraphs[0].kind, static_cast<int>(ParaKind::Heading));
        CHECK_INT(paragraphs[0].level, 1);
        CHECK_STR(para_text(paragraphs[0]), "Привет");
        CHECK_INT(paragraphs[1].kind, static_cast<int>(ParaKind::Text));
        CHECK_STR(para_text(paragraphs[1]), "Текст жирный.");
        CHECK(has_run(paragraphs[1], "жирный", Face::Bold));
    }
}

void check_edges() {
    // Empty input yields a valid, empty document.
    {
        std::string error;
        auto doc = reader::open_markdown(std::make_unique<MemoryByteSource>(std::string()), &error);
        CHECK(doc != nullptr);
        if (doc) {
            CHECK(doc->info().title.empty());
            CHECK_INT(doc->total_chars(), 0);
            CHECK_INT(doc->permille(0), 0);
            Paragraph paragraph;
            CHECK(!doc->next(paragraph));
            CHECK(doc->rewind());
            CHECK(!doc->seek(0));
            CHECK(!doc->next(paragraph));
        }
    }

    // Null source is rejected without crashing.
    {
        std::string error;
        CHECK(reader::open_markdown(std::unique_ptr<reader::ByteSource>(), &error) == nullptr);
        CHECK(!error.empty());
    }

    // The 8 MiB cap is enforced.
    {
        std::string huge(8u * 1024u * 1024u + 1u, 'a');
        std::string error;
        auto doc = reader::open_markdown(std::make_unique<MemoryByteSource>(std::move(huge)), &error);
        CHECK(doc == nullptr);
        CHECK(!error.empty());
    }

    // UTF-8 BOM is stripped from the text and from the tokens.
    {
        const std::string data = "\xEF\xBB\xBF# BOM Title\n\nText.\n";
        auto doc = open_text(data);
        if (doc) {
            CHECK_STR(doc->info().title, "BOM Title");
            CHECK_INT(doc->total_chars(), reader::utf8_length("# BOM Title\n\nText.\n"));
            std::vector<Paragraph> paragraphs = collect(*doc);
            CHECK(!paragraphs.empty());
            if (!paragraphs.empty()) CHECK(paragraphs.front().token == 0);
        }
    }

    // Unclosed inline markup stays literal; an unclosed fence still emits code.
    {
        auto doc = open_text("**unclosed and *also\n\n```\nabc\n");
        if (doc) {
            std::vector<Paragraph> paragraphs = collect(*doc);
            CHECK(find_para(paragraphs, "**unclosed and *also", ParaKind::Text) != nullptr);
            CHECK(find_para(paragraphs, "abc", ParaKind::Code) != nullptr);
        }
    }

    // Hostile input: thousands of unmatched '[' and a very deep quote.
    {
        std::string junk(2000, '[');
        junk += "\n";
        junk += std::string(200, '>');
        junk += " deep\n";
        auto doc = open_text(junk);
        if (doc) {
            std::vector<Paragraph> paragraphs = collect(*doc);
            CHECK(!paragraphs.empty());
        }
    }

    // Heading levels 4..6.
    {
        auto doc = open_text("#### Four\n##### Five\n###### Six\n");
        if (doc) {
            std::vector<Paragraph> paragraphs = collect(*doc);
            CHECK_INT(paragraphs.size(), 3);
            if (paragraphs.size() == 3) {
                CHECK_INT(paragraphs[0].level, 4);
                CHECK_INT(paragraphs[1].level, 5);
                CHECK_INT(paragraphs[2].level, 6);
                CHECK_STR(para_text(paragraphs[0]), "Four");
            }
        }
    }

    // Bold nested inside italic.
    {
        auto doc = open_text("*outer **inner** tail*\n");
        if (doc) {
            std::vector<Paragraph> paragraphs = collect(*doc);
            CHECK_INT(paragraphs.size(), 1);
            if (!paragraphs.empty()) {
                CHECK_STR(para_text(paragraphs[0]), "outer inner tail");
                CHECK(has_run(paragraphs[0], "outer ", Face::Italic));
                CHECK(has_run(paragraphs[0], "inner", Face::BoldItalic));
                CHECK(has_run(paragraphs[0], " tail", Face::Italic));
            }
        }
    }

    // Table separator row is skipped, other rows become Text paragraphs.
    {
        auto doc = open_text("| a | b |\n|---|---|\n| c | d |\n");
        if (doc) {
            std::vector<Paragraph> paragraphs = collect(*doc);
            CHECK_INT(paragraphs.size(), 2);
            if (paragraphs.size() == 2) {
                CHECK_STR(para_text(paragraphs[0]), "a | b");
                CHECK_STR(para_text(paragraphs[1]), "c | d");
            }
        }
    }

    // CRLF and CR-only line endings.
    {
        auto doc = open_text("# CRLF\r\n\r\nline one\r\nline two\r\n");
        if (doc) {
            std::vector<Paragraph> paragraphs = collect(*doc);
            CHECK_INT(paragraphs.size(), 2);
            if (paragraphs.size() == 2) {
                CHECK_INT(paragraphs[0].kind, static_cast<int>(ParaKind::Heading));
                CHECK_STR(para_text(paragraphs[1]), "line one line two");
            }
        }
        auto cr_doc = open_text("# CR\r\rline one\rline two\r");
        if (cr_doc) {
            std::vector<Paragraph> paragraphs = collect(*cr_doc);
            CHECK_INT(paragraphs.size(), 2);
            if (paragraphs.size() == 2) CHECK_STR(para_text(paragraphs[1]), "line one line two");
        }
    }
}

} // namespace

int main() {
    check_main_fixture();
    check_cp1251();
    check_edges();

    std::printf("checks=%d failures=%d\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::printf("test_md: PASS\n");
        return 0;
    }
    std::printf("test_md: FAIL\n");
    return 1;
}
