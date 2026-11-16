// Host test for the EPUB parser (ereader/firmware/main/formats/epub.cpp).
//
// Builds a small EPUB in memory (ZIP written by hand, CRC32 included, one
// deflated chapter and one stored chapter) and checks metadata, spine order,
// inline styling, lists, entities, images, seeking and progress reporting.
//
// Exit code 0 means every check passed.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/byte_source.h"
#include "formats/formats.h"
#include "text/doc.h"
#include "text/types.h"

// miniz is linked in for the test's deflate compressor (raw deflate stream).
extern "C" void* tdefl_compress_mem_to_heap(const void* pSrc, size_t src_len, size_t* pOut_len,
                                            int flags);
extern "C" void lgfx_mz_free(void* p);

namespace {

int g_checks = 0;
int g_failures = 0;

void check_failed(const char* file, int line, const char* expr) {
    std::printf("FAIL %s:%d: %s\n", file, line, expr);
    ++g_failures;
}

void check_text(const char* file, int line, const char* what, const std::string& actual,
                const std::string& expected) {
    ++g_checks;
    if (actual == expected) return;
    ++g_failures;
    std::printf("FAIL %s:%d: %s\n  expected: %s\n  actual:   %s\n", file, line, what,
                expected.c_str(), actual.c_str());
}

#define CHECK(expr)                                                                              \
    do {                                                                                         \
        ++g_checks;                                                                              \
        if (!(expr)) check_failed(__FILE__, __LINE__, #expr);                                    \
    } while (false)

#define CHECK_TEXT(actual, expected) check_text(__FILE__, __LINE__, #actual, (actual), (expected))

// ------------------------------------------------------------------------ ZIP

void put16(std::string& out, uint16_t value) {
    out.push_back(static_cast<char>(value & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
}

void put32(std::string& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
}

uint32_t crc32_of(const std::string& data) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1u) ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
            }
            table[i] = value;
        }
        ready = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (char c : data) crc = table[(crc ^ static_cast<uint8_t>(c)) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

class ZipWriter {
  public:
    void add(const std::string& name, const std::string& content, bool deflate) {
        Record record;
        record.name = name;
        record.crc = crc32_of(content);
        record.raw_size = content.size();
        record.method = 0;
        record.payload = content;
        if (deflate && !content.empty()) {
            size_t packed_size = 0;
            void* packed = tdefl_compress_mem_to_heap(content.data(), content.size(), &packed_size, 128);
            if (packed != nullptr && packed_size != 0) {
                record.payload.assign(static_cast<const char*>(packed), packed_size);
                record.method = 8;
                lgfx_mz_free(packed);
            }
        }
        record.offset = static_cast<uint32_t>(_local.size());
        put32(_local, 0x04034b50u);
        put16(_local, 20); // version needed
        put16(_local, 0);  // flags
        put16(_local, record.method);
        put16(_local, 0); // time
        put16(_local, 0); // date
        put32(_local, record.crc);
        put32(_local, static_cast<uint32_t>(record.payload.size()));
        put32(_local, static_cast<uint32_t>(record.raw_size));
        put16(_local, static_cast<uint16_t>(name.size()));
        put16(_local, 0); // extra length
        _local += name;
        _local += record.payload;
        _records.push_back(std::move(record));
    }

    std::string finish() const {
        std::string out = _local;
        const uint32_t central_offset = static_cast<uint32_t>(out.size());
        std::string central;
        for (const Record& record : _records) {
            put32(central, 0x02014b50u);
            put16(central, 20); // version made by
            put16(central, 20); // version needed
            put16(central, 0);  // flags
            put16(central, record.method);
            put16(central, 0); // time
            put16(central, 0); // date
            put32(central, record.crc);
            put32(central, static_cast<uint32_t>(record.payload.size()));
            put32(central, static_cast<uint32_t>(record.raw_size));
            put16(central, static_cast<uint16_t>(record.name.size()));
            put16(central, 0); // extra
            put16(central, 0); // comment
            put16(central, 0); // disk start
            put16(central, 0); // internal attributes
            put32(central, 0); // external attributes
            put32(central, record.offset);
            central += record.name;
        }
        out += central;
        put32(out, 0x06054b50u);
        put16(out, 0); // this disk
        put16(out, 0); // disk with central directory
        put16(out, static_cast<uint16_t>(_records.size()));
        put16(out, static_cast<uint16_t>(_records.size()));
        put32(out, static_cast<uint32_t>(central.size()));
        put32(out, central_offset);
        put16(out, 0); // comment length
        return out;
    }

  private:
    struct Record {
        std::string name;
        std::string payload;
        uint32_t crc = 0;
        size_t raw_size = 0;
        uint32_t offset = 0;
        uint16_t method = 0;
    };

    std::string _local;
    std::vector<Record> _records;
};

// -------------------------------------------------------------------- fixtures

const char* const kContainerXml = R"(<?xml version="1.0" encoding="utf-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>
)";

const char* const kPackageXml = R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="bookid">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:title>Тестовая книга</dc:title>
    <dc:creator>Иван Петров</dc:creator>
    <dc:language>ru</dc:language>
  </metadata>
  <manifest>
    <item id="ch1" href="text/chapter1.xhtml" media-type="application/xhtml+xml"/>
    <item id="ch2" href="text/chapter%202.xhtml" media-type="application/xhtml+xml"/>
    <item id="css" href="style.css" media-type="text/css"/>
    <item id="gone" href="missing.xhtml" media-type="application/xhtml+xml"/>
  </manifest>
  <spine>
    <itemref idref="ch1"/>
    <itemref idref="css"/>
    <itemref idref="ch2"/>
    <itemref idref="gone"/>
  </spine>
</package>
)";

const char* const kChapterOne = R"(<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml">
<head><title>Глава 1</title><style>.x { color: red; }</style></head>
<body>
<h1>Глава первая</h1>
<p>Первый абзац с <em>курсивом</em> и <strong>жирным</strong> словом.</p>
<p>Строка один<br/>строка два</p>
<p>Спецсимволы: &amp; &lt; &gt; &quot; &mdash; &nbsp; &#1055; &#x456;.</p>
<ul>
  <li>Пункт списка
    <ul>
      <li>Вложенный пункт</li>
    </ul>
  </li>
</ul>
<ol>
  <li>Первый</li>
  <li>Второй</li>
</ol>
<p>Картинка: <img src="pic.png" alt="pic"/></p>
<hr/>
<div>Текст <a href="https://example.com">ссылки</a> в контейнере</div>
<p>E = mc<sup>2</sup> и <sub>N</sub>.</p>
<blockquote><p>Цитата</p></blockquote>
<pre>line1
line2</pre>
<script>var bad = "<p>script</p>";</script>
</body>
</html>
)";

const char* const kChapterTwo = R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Глава 2</title></head>
<body>
<h2>Глава вторая</h2>
<p>Текст второй главы.</p>
</body></html>
)";

const char* const kFallbackPackage = R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:title>Без контейнера</dc:title>
  </metadata>
  <manifest>
    <item id="only" href="../text/only.xhtml" media-type="application/xhtml+xml"/>
  </manifest>
  <spine><itemref idref="only"/></spine>
</package>
)";

std::string make_main_epub() {
    ZipWriter zip;
    zip.add("mimetype", "application/epub+zip", false);
    zip.add("META-INF/container.xml", kContainerXml, false);
    zip.add("OEBPS/content.opf", kPackageXml, false);
    zip.add("OEBPS/text/chapter1.xhtml", kChapterOne, true); // deflated
    zip.add("OEBPS/text/chapter 2.xhtml", kChapterTwo, false); // stored
    zip.add("OEBPS/style.css", "p { margin: 0; }\n", false);
    return zip.finish();
}

std::string make_fallback_epub() {
    // No META-INF/container.xml: the first *.opf in the archive must be found.
    // The chapter starts with a UTF-8 BOM and leaves <p> unclosed on purpose.
    const std::string chapter = std::string("\xEF\xBB\xBF") +
                                "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                                "<html xmlns=\"http://www.w3.org/1999/xhtml\">"
                                "<head><title>t</title></head><body>"
                                "<h2>Второй заголовок</h2>"
                                "<p>Абзац с <b>жирным</b> и <i>курсивом</i>"
                                "</body></html>";
    ZipWriter zip;
    zip.add("mimetype", "application/epub+zip", false);
    zip.add("OPS/book.opf", kFallbackPackage, false);
    zip.add("text/only.xhtml", chapter, false);
    return zip.finish();
}

std::string with_zip_comment(const std::string& zip, const std::string& comment) {
    // The EOCD sits at the very end, so its 16-bit comment length is at size-2.
    std::string out = zip;
    const size_t length_at = out.size() - 2;
    const uint16_t length = static_cast<uint16_t>(comment.size());
    out[length_at] = static_cast<char>(length & 0xFF);
    out[length_at + 1] = static_cast<char>((length >> 8) & 0xFF);
    out += comment;
    return out;
}

std::string make_broken_package_epub() {
    ZipWriter zip;
    zip.add("mimetype", "application/epub+zip", false);
    zip.add("META-INF/container.xml", kContainerXml, false);
    zip.add("OEBPS/content.opf", "<package><metadata><dc:title>oops</dc:title><spine>", false);
    return zip.finish();
}

std::string make_missing_package_epub() {
    ZipWriter zip;
    zip.add("mimetype", "application/epub+zip", false);
    zip.add("META-INF/container.xml", kContainerXml, false);
    zip.add("OEBPS/chapter1.xhtml", kChapterOne, false);
    return zip.finish();
}

// -------------------------------------------------------------------- helpers

std::string paragraph_text(const reader::Paragraph& paragraph) {
    std::string text;
    for (const reader::Run& run : paragraph.runs) text += run.text;
    return text;
}

const reader::Paragraph* find_paragraph(const std::vector<reader::Paragraph>& paragraphs,
                                        const std::string& text) {
    for (const reader::Paragraph& paragraph : paragraphs) {
        if (paragraph_text(paragraph) == text) return &paragraph;
    }
    return nullptr;
}

const reader::Run* find_run(const reader::Paragraph& paragraph, const std::string& text) {
    for (const reader::Run& run : paragraph.runs) {
        if (run.text == text) return &run;
    }
    return nullptr;
}

const char* kind_name(reader::ParaKind kind) {
    switch (kind) {
    case reader::ParaKind::Text: return "Text";
    case reader::ParaKind::Heading: return "Heading";
    case reader::ParaKind::Quote: return "Quote";
    case reader::ParaKind::ListItem: return "ListItem";
    case reader::ParaKind::Code: return "Code";
    case reader::ParaKind::Separator: return "Separator";
    case reader::ParaKind::Blank: return "Blank";
    }
    return "?";
}

std::unique_ptr<reader::Document> open_memory(const std::string& bytes, std::string* error) {
    auto source = std::make_unique<reader::MemoryByteSource>(bytes);
    return reader::open_epub(std::move(source), error);
}

// ----------------------------------------------------------------------- main

int run_all_checks() {
    const std::string main_epub = make_main_epub();
    std::printf("built in-memory EPUB: %u bytes\n", static_cast<unsigned>(main_epub.size()));

    std::string error;
    auto document = open_memory(main_epub, &error);
    CHECK(document != nullptr);
    if (!document) {
        std::printf("open failed: %s\n", error.c_str());
        std::printf("\n%d checks, %d failures\nTEST_EPUB: FAIL\n", g_checks, g_failures);
        return 1;
    }

    CHECK_TEXT(document->info().title, "Тестовая книга");
    CHECK_TEXT(document->info().author, "Иван Петров");

    std::vector<reader::Paragraph> paragraphs;
    reader::Paragraph paragraph;
    while (document->next(paragraph)) paragraphs.push_back(paragraph);
    CHECK(!paragraphs.empty());
    CHECK(paragraphs.size() >= 14);

    std::printf("--- paragraphs (spine/offset, kind, level, marker, text) ---\n");
    for (const reader::Paragraph& item : paragraphs) {
        std::string text = paragraph_text(item);
        if (text.size() > 64) text.resize(64);
        std::printf("[%2u] spine=%llu off=%-4llu %-9s lvl=%u marker=%-3s | %s\n",
                    static_cast<unsigned>(&item - paragraphs.data()),
                    static_cast<unsigned long long>(item.token >> 32),
                    static_cast<unsigned long long>(item.token & 0xFFFFFFFFull), kind_name(item.kind),
                    static_cast<unsigned>(item.level), item.marker.c_str(), text.c_str());
    }

    // Headings and spine order.
    const reader::Paragraph* chapter_one = find_paragraph(paragraphs, "Глава первая");
    const reader::Paragraph* chapter_two = find_paragraph(paragraphs, "Глава вторая");
    CHECK(chapter_one != nullptr);
    CHECK(chapter_two != nullptr);
    if (chapter_one != nullptr) {
        CHECK(chapter_one->kind == reader::ParaKind::Heading);
        CHECK(chapter_one->level == 1);
    }
    if (chapter_two != nullptr) {
        CHECK(chapter_two->kind == reader::ParaKind::Heading);
        CHECK(chapter_two->level == 2);
    }
    if (chapter_one != nullptr && chapter_two != nullptr) {
        const size_t index_one = static_cast<size_t>(chapter_one - paragraphs.data());
        const size_t index_two = static_cast<size_t>(chapter_two - paragraphs.data());
        CHECK(index_two > index_one);                    // spine order 1 -> 2
        CHECK((chapter_one->token >> 32) == 0);          // first spine document
        CHECK((chapter_two->token >> 32) == 1);          // second spine document
        CHECK(chapter_two->token > chapter_one->token);  // strictly increasing tokens
    }

    // Inline runs: <em> -> Italic, <strong> -> Bold.
    const reader::Paragraph* styled = find_paragraph(paragraphs, "Первый абзац с курсивом и жирным словом.");
    CHECK(styled != nullptr);
    if (styled != nullptr) {
        const reader::Run* italic = find_run(*styled, "курсивом");
        const reader::Run* bold = find_run(*styled, "жирным");
        CHECK(italic != nullptr);
        CHECK(bold != nullptr);
        if (italic != nullptr) CHECK(italic->style.face == reader::Face::Italic);
        if (bold != nullptr) CHECK(bold->style.face == reader::Face::Bold);
    }

    // <br> keeps one paragraph but separates words.
    CHECK(find_paragraph(paragraphs, "Строка один строка два") != nullptr);

    // Entities, entities with numbers, whitespace collapsing.
    const reader::Paragraph* entities = find_paragraph(paragraphs, "Спецсимволы: & < > \" — П і.");
    CHECK(entities != nullptr);
    if (entities != nullptr) {
        CHECK(paragraph_text(*entities).find("&amp;") == std::string::npos);
        CHECK(paragraph_text(*entities).find("&nbsp;") == std::string::npos);
    }

    // Lists: bullets, numbering, nesting.
    const reader::Paragraph* bullet = find_paragraph(paragraphs, "Пункт списка");
    const reader::Paragraph* nested = find_paragraph(paragraphs, "Вложенный пункт");
    const reader::Paragraph* first_number = find_paragraph(paragraphs, "Первый");
    const reader::Paragraph* second_number = find_paragraph(paragraphs, "Второй");
    CHECK(bullet != nullptr);
    CHECK(nested != nullptr);
    CHECK(first_number != nullptr);
    CHECK(second_number != nullptr);
    if (bullet != nullptr) {
        CHECK(bullet->kind == reader::ParaKind::ListItem);
        CHECK(bullet->level == 1);
        CHECK_TEXT(bullet->marker, "•");
    }
    if (nested != nullptr) {
        CHECK(nested->kind == reader::ParaKind::ListItem);
        CHECK(nested->level == 2);
        CHECK_TEXT(nested->marker, "•");
    }
    if (first_number != nullptr) {
        CHECK(first_number->kind == reader::ParaKind::ListItem);
        CHECK_TEXT(first_number->marker, "1.");
    }
    if (second_number != nullptr) {
        CHECK(second_number->kind == reader::ParaKind::ListItem);
        CHECK_TEXT(second_number->marker, "2.");
    }

    // <img> becomes its own paragraph with a placeholder text.
    CHECK(find_paragraph(paragraphs, "Картинка:") != nullptr);
    CHECK(find_paragraph(paragraphs, "[изображение]") != nullptr);

    // <hr> turns into a separator paragraph.
    bool has_separator = false;
    for (const reader::Paragraph& item : paragraphs) {
        if (item.kind == reader::ParaKind::Separator) has_separator = true;
    }
    CHECK(has_separator);

    // <div> is a container, <a> contributes only its text, <sup>/<sub> too.
    CHECK(find_paragraph(paragraphs, "Текст ссылки в контейнере") != nullptr);
    CHECK(find_paragraph(paragraphs, "E = mc2 и N.") != nullptr);

    // <blockquote> and <pre>.
    const reader::Paragraph* quote = find_paragraph(paragraphs, "Цитата");
    CHECK(quote != nullptr);
    if (quote != nullptr) CHECK(quote->kind == reader::ParaKind::Quote);
    const reader::Paragraph* code_one = find_paragraph(paragraphs, "line1");
    const reader::Paragraph* code_two = find_paragraph(paragraphs, "line2");
    CHECK(code_one != nullptr);
    CHECK(code_two != nullptr);
    if (code_one != nullptr) CHECK(code_one->kind == reader::ParaKind::Code);
    if (code_two != nullptr) CHECK(code_two->kind == reader::ParaKind::Code);

    // <script> content must not leak into the text.
    for (const reader::Paragraph& item : paragraphs) {
        CHECK(paragraph_text(item).find("var bad") == std::string::npos);
        CHECK(paragraph_text(item).find("script") == std::string::npos);
    }

    // Progress: start of the second chapter is 50% of a two-chapter book.
    CHECK(document->permille(0) == 0);
    if (chapter_two != nullptr) CHECK(document->permille(chapter_two->token) == 500);
    CHECK(document->permille(paragraphs.back().token) <= 1000);
    CHECK(document->permille(paragraphs.back().token) >= 500);

    // seek() to the second chapter, then read forward.
    if (chapter_two != nullptr) {
        CHECK(document->seek(chapter_two->token));
        reader::Paragraph sought;
        CHECK(document->next(sought));
        CHECK_TEXT(paragraph_text(sought), "Глава вторая");
        CHECK(document->next(sought));
        CHECK_TEXT(paragraph_text(sought), "Текст второй главы.");
        CHECK(!document->next(sought));
    }

    // seek() to an arbitrary offset inside chapter one lands on a paragraph
    // start (the parser's token, since fixtures use exact paragraph tokens).
    if (chapter_one != nullptr) {
        CHECK(document->seek(chapter_one->token));
        reader::Paragraph sought;
        CHECK(document->next(sought));
        CHECK_TEXT(paragraph_text(sought), "Глава первая");
    }

    // Horizontal rules are not exercised by this fixture, but a seek past the
    // last paragraph means "end of document".
    CHECK(document->seek(paragraphs.back().token));
    {
        reader::Paragraph last;
        CHECK(document->next(last));
        CHECK_TEXT(paragraph_text(last), "Текст второй главы.");
        CHECK(!document->next(last));
    }

    // rewind() restarts at the first paragraph and next() walks everything.
    CHECK(document->rewind());
    {
        reader::Paragraph first;
        CHECK(document->next(first));
        CHECK_TEXT(paragraph_text(first), "Глава первая");
        size_t count = 1;
        reader::Paragraph walk;
        while (document->next(walk)) ++count;
        CHECK(count == paragraphs.size());
    }

    // total_chars() is the sum of the extracted chapter texts (+ one newline per
    // paragraph); both chapters are parsed by now, so it must be exact.
    uint64_t expected_chars = 0;
    for (const reader::Paragraph& item : paragraphs) expected_chars += paragraph_text(item).size() + 1;
    CHECK(document->total_chars() == expected_chars);
    CHECK(document->total_chars() > 0);

    // Broken input never crashes: garbage, truncated archive, empty buffer.
    {
        std::string broken_error;
        auto broken = open_memory("definitely not a zip archive", &broken_error);
        CHECK(broken == nullptr);
        CHECK(!broken_error.empty());
        std::printf("garbage input -> %s\n", broken_error.c_str());

        broken_error.clear();
        auto truncated = open_memory(main_epub.substr(0, main_epub.size() / 2), &broken_error);
        CHECK(truncated == nullptr);
        CHECK(!broken_error.empty());
        std::printf("truncated zip -> %s\n", broken_error.c_str());

        broken_error.clear();
        auto empty = open_memory(std::string(), &broken_error);
        CHECK(empty == nullptr);
        CHECK(!broken_error.empty());
        std::printf("empty buffer  -> %s\n", broken_error.c_str());
    }

    // EOCD lookup must tolerate a ZIP archive comment (up to 64 KiB).
    {
        std::string commented_error;
        auto commented = open_memory(with_zip_comment(main_epub, "made by test_epub"), &commented_error);
        CHECK(commented != nullptr);
        if (commented) {
            CHECK_TEXT(commented->info().title, "Тестовая книга");
            reader::Paragraph first;
            CHECK(commented->next(first));
            CHECK_TEXT(paragraph_text(first), "Глава первая");
        } else {
            std::printf("commented zip failed: %s\n", commented_error.c_str());
        }
    }

    // Broken package document and container.xml pointing at a missing OPF.
    {
        std::string broken_opf_error;
        auto broken_opf = open_memory(make_broken_package_epub(), &broken_opf_error);
        CHECK(broken_opf == nullptr);
        CHECK(!broken_opf_error.empty());
        std::printf("broken OPF    -> %s\n", broken_opf_error.c_str());

        std::string missing_opf_error;
        auto missing_opf = open_memory(make_missing_package_epub(), &missing_opf_error);
        CHECK(missing_opf == nullptr);
        CHECK(!missing_opf_error.empty());
        std::printf("missing OPF   -> %s\n", missing_opf_error.c_str());
    }

    // Fallback path: no container.xml, OPF found by extension, ../ href,
    // percent-free path, UTF-8 BOM and an unclosed <p>.
    {
        const std::string fallback_epub = make_fallback_epub();
        std::string fallback_error;
        auto fallback = open_memory(fallback_epub, &fallback_error);
        CHECK(fallback != nullptr);
        if (fallback) {
            CHECK_TEXT(fallback->info().title, "Без контейнера");
            CHECK(fallback->info().author.empty());
            reader::Paragraph first;
            CHECK(fallback->next(first));
            CHECK(first.kind == reader::ParaKind::Heading);
            CHECK(first.level == 2);
            CHECK_TEXT(paragraph_text(first), "Второй заголовок");
            reader::Paragraph second;
            CHECK(fallback->next(second));
            CHECK_TEXT(paragraph_text(second), "Абзац с жирным и курсивом");
            reader::Paragraph extra;
            CHECK(!fallback->next(extra));
        } else {
            std::printf("fallback open failed: %s\n", fallback_error.c_str());
        }
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::printf("TEST_EPUB: PASS\n");
        return 0;
    }
    std::printf("TEST_EPUB: FAIL\n");
    return 1;
}

} // namespace

int main() {
    return run_all_checks();
}
