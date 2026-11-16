// Host tests for the PDF text-layer extractor.
// Build: see the comment in the task / repository docs.
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/byte_source.h"
#include "formats/formats.h"
#include "text/types.h"

// zlib-style entry points from the bundled miniz (compiled as C++).
extern "C" int lgfx_mz_compress(unsigned char* pDest, unsigned long* pDest_len,
                                 const unsigned char* pSource, unsigned long source_len);
extern "C" unsigned long lgfx_mz_compressBound(unsigned long source_len);

namespace {

using reader::Document;
using reader::MemoryByteSource;
using reader::open_pdf;
using reader::ParaKind;
using reader::Paragraph;
using reader::Run;

int g_failures = 0;

#define CHECK(condition)                                                                       \
    do {                                                                                       \
        if (!(condition)) {                                                                    \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);          \
            ++g_failures;                                                                      \
        }                                                                                      \
    } while (0)

#define CHECK_STR(actual, expected)                                                            \
    do {                                                                                       \
        const std::string& _a = (actual);                                                      \
        const std::string& _e = (expected);                                                    \
        if (_a != _e) {                                                                        \
            std::fprintf(stderr, "FAIL %s:%d: expected \"%s\", got \"%s\"\n", __FILE__,        \
                         __LINE__, _e.c_str(), _a.c_str());                                    \
            ++g_failures;                                                                      \
        }                                                                                      \
    } while (0)

std::string zlib_compress(const std::string& input) {
    unsigned long bound = lgfx_mz_compressBound(static_cast<unsigned long>(input.size()));
    std::string out;
    out.resize(bound);
    unsigned long length = bound;
    const int status = lgfx_mz_compress(reinterpret_cast<unsigned char*>(&out[0]), &length,
                                        reinterpret_cast<const unsigned char*>(input.data()),
                                        static_cast<unsigned long>(input.size()));
    if (status != 0) return std::string();
    out.resize(length);
    return out;
}

std::string stream_body(const std::string& extra_entries, const std::string& data) {
    return "<< /Length " + std::to_string(data.size()) + extra_entries + " >>\nstream\n" + data +
           "\nendstream";
}

std::string make_classic_pdf(const std::vector<std::string>& objects, int root, int info,
                             const std::string& trailer_extra = std::string()) {
    std::string out = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
    std::vector<size_t> offsets;
    offsets.reserve(objects.size());
    for (size_t i = 0; i < objects.size(); ++i) {
        offsets.push_back(out.size());
        out += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const size_t xref_offset = out.size();
    out += "xref\n0 " + std::to_string(objects.size() + 1) + "\n";
    out += "0000000000 65535 f \n";
    for (size_t offset : offsets) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%010llu 00000 n \n",
                      static_cast<unsigned long long>(offset));
        out += buffer;
    }
    out += "trailer\n<< /Size " + std::to_string(objects.size() + 1);
    out += " /Root " + std::to_string(root) + " 0 R";
    if (info > 0) out += " /Info " + std::to_string(info) + " 0 R";
    out += trailer_extra;
    out += " >>\nstartxref\n" + std::to_string(xref_offset) + "\n%%EOF\n";
    return out;
}

std::unique_ptr<Document> open_string(const std::string& pdf, std::string* error) {
    auto source = std::make_unique<MemoryByteSource>(pdf);
    return open_pdf(std::move(source), error);
}

std::vector<std::string> paragraph_texts(Document& document) {
    std::vector<std::string> texts;
    document.rewind();
    Paragraph paragraph;
    while (document.next(paragraph)) {
        std::string text;
        for (const Run& run : paragraph.runs) text += run.text;
        texts.push_back(std::move(text));
    }
    return texts;
}

// ---------------------------------------------------------------------------
// 1. Simplest classic PDF: Helvetica, one line, classic xref + trailer.
// ---------------------------------------------------------------------------

void test_simple_classic_pdf() {
    std::vector<std::string> objects = {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>",
        stream_body("", "BT /F1 24 Tf 72 720 Td (Hello, world!) Tj ET"),
        "<< /Title (Simple PDF Test) /Author (Test Suite) >>",
    };
    const std::string pdf = make_classic_pdf(objects, 1, 6);

    std::string error;
    auto document = open_string(pdf, &error);
    CHECK(document != nullptr);
    if (!document) {
        std::fprintf(stderr, "  error: %s\n", error.c_str());
        return;
    }
    CHECK_STR(document->info().title, "Simple PDF Test");
    CHECK(document->total_chars() == 13);
    CHECK(document->permille(0) == 0);

    document->rewind();
    Paragraph paragraph;
    CHECK(document->next(paragraph));
    CHECK(paragraph.token == 0);
    CHECK(paragraph.kind == ParaKind::Text);
    CHECK(paragraph.runs.size() == 1);
    if (!paragraph.runs.empty()) CHECK_STR(paragraph.runs[0].text, "Hello, world!");
    CHECK(!document->next(paragraph));

    CHECK(document->seek(0));
    CHECK(document->next(paragraph));
    if (!paragraph.runs.empty()) CHECK_STR(paragraph.runs[0].text, "Hello, world!");
}

// ---------------------------------------------------------------------------
// 2. FlateDecode content stream + Type0/Identity-H + ToUnicode CMap (Cyrillic).
// ---------------------------------------------------------------------------

const char* kCyrillicCmap = R"(/CIDInit /ProcSet findresource begin
12 dict begin
begincmap
/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def
/CMapName /Adobe-Identity-UCS def
/CMapType 2 def
1 begincodespacerange
<0000> <FFFF>
endcodespacerange
10 beginbfchar
<0001> <041F>
<0002> <0440>
<0003> <0438>
<0004> <0432>
<0005> <0435>
<0006> <0442>
<0007> <043C>
<0008> <0438>
<0009> <0440>
<000A> <0021>
endbfchar
endcmap
CMapName currentdict /CMap defineresource pop
end
end
)";

void test_flate_cyrillic_type0() {
    const std::string content =
        "BT /F1 24 Tf 72 720 Td <000100020003000400050006> Tj 0 -30 Td "
        "<000700080009000A> Tj ET";
    const std::string cmap_compressed = zlib_compress(kCyrillicCmap);
    const std::string content_compressed = zlib_compress(content);
    CHECK(!cmap_compressed.empty());
    CHECK(!content_compressed.empty());

    std::vector<std::string> objects = {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Resources << /Font << /F1 4 0 R >> >> /Contents 6 0 R >>",
        "<< /Type /Font /Subtype /Type0 /BaseFont /Cyrillic /Encoding /Identity-H "
        "/DescendantFonts [<< /Type /Font /Subtype /CIDFontType2 /BaseFont /Cyrillic "
        "/CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> "
        "/DW 1000 >>] /ToUnicode 5 0 R >>",
        stream_body(" /Filter /FlateDecode", cmap_compressed),
        stream_body(" /Filter /FlateDecode", content_compressed),
        "<< /Title <FEFF041F04400438043204350442> >>",
    };
    const std::string pdf = make_classic_pdf(objects, 1, 7);

    std::string error;
    auto document = open_string(pdf, &error);
    CHECK(document != nullptr);
    if (!document) {
        std::fprintf(stderr, "  error: %s\n", error.c_str());
        return;
    }
    CHECK_STR(document->info().title, "Привет");
    const auto texts = paragraph_texts(*document);
    CHECK(texts.size() == 1);
    if (!texts.empty()) CHECK_STR(texts[0], "Привет мир!");
    CHECK(document->total_chars() == 11);
}

// ---------------------------------------------------------------------------
// 3. Broken input.
// ---------------------------------------------------------------------------

void test_broken_pdf() {
    std::string error;
    auto document = open_string("this is definitely not a PDF file", &error);
    CHECK(document == nullptr);
    CHECK(!error.empty());
}

void test_pdf_without_text_layer() {
    std::vector<std::string> objects = {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R >>",
        stream_body("", "0 0 100 100 re f"),
    };
    const std::string pdf = make_classic_pdf(objects, 1, 0);
    std::string error;
    auto document = open_string(pdf, &error);
    CHECK(document == nullptr);
    CHECK_STR(error, "PDF has no text layer (scans are not supported)");
}

void test_encrypted_pdf() {
    std::vector<std::string> objects = {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R >>",
        stream_body("", "BT /F1 12 Tf 72 720 Td (secret) Tj ET"),
    };
    const std::string pdf = make_classic_pdf(objects, 1, 0, " /Encrypt 9 0 R");
    std::string error;
    auto document = open_string(pdf, &error);
    CHECK(document == nullptr);
    CHECK_STR(error, "PDF is encrypted");
}

// Type0 without /ToUnicode must not emit garbage; the page is skipped and the
// document reports the "no text layer" error.
void test_type0_without_tounicode() {
    std::vector<std::string> objects = {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>",
        "<< /Type /Font /Subtype /Type0 /BaseFont /Cyrillic /Encoding /Identity-H "
        "/DescendantFonts [<< /Type /Font /Subtype /CIDFontType2 /BaseFont /Cyrillic "
        "/CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> >>] >>",
        stream_body("", "BT /F1 12 Tf 72 720 Td <00010002> Tj ET"),
    };
    const std::string pdf = make_classic_pdf(objects, 1, 0);
    std::string error;
    auto document = open_string(pdf, &error);
    CHECK(document == nullptr);
    CHECK_STR(error, "PDF has no text layer (scans are not supported)");
}

// ---------------------------------------------------------------------------
// 4. Xref stream + object stream (all page-tree objects live in /ObjStm).
// ---------------------------------------------------------------------------

std::string build_xref_stream_pdf() {
    const std::string content = "BT /F1 16 Tf 72 720 Td (Object stream OK) Tj ET";
    const std::string content_compressed = zlib_compress(content);
    const std::string object1 = stream_body(" /Filter /FlateDecode", content_compressed);

    std::vector<std::pair<int, std::string>> inner = {
        {3, "<< /Type /Catalog /Pages 4 0 R >>"},
        {4, "<< /Type /Pages /Kids [5 0 R] /Count 1 >>"},
        {5, "<< /Type /Page /Parent 4 0 R /MediaBox [0 0 612 792] "
            "/Resources << /Font << /F1 6 0 R >> >> /Contents 1 0 R >>"},
        {6, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
            "/Encoding /WinAnsiEncoding >>"},
    };
    std::string header;
    std::string packed;
    for (const auto& item : inner) {
        header += std::to_string(item.first) + " " + std::to_string(packed.size()) + " ";
        packed += item.second;
        packed += "\n";
    }
    const std::string object_stream_data = header + packed;
    const std::string object_stream_compressed = zlib_compress(object_stream_data);
    const std::string object2 =
        stream_body(" /Type /ObjStm /N " + std::to_string(inner.size()) +
                        " /First " + std::to_string(header.size()) + " /Filter /FlateDecode",
                    object_stream_compressed);

    std::string out = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
    const size_t offset1 = out.size();
    out += "1 0 obj\n" + object1 + "\nendobj\n";
    const size_t offset2 = out.size();
    out += "2 0 obj\n" + object2 + "\nendobj\n";
    const size_t offset8 = out.size();

    std::string rows;
    auto add_row = [&rows](int type, unsigned long long field2, unsigned field3) {
        rows.push_back(static_cast<char>(type));
        for (int shift = 24; shift >= 0; shift -= 8) {
            rows.push_back(static_cast<char>((field2 >> shift) & 0xFF));
        }
        rows.push_back(static_cast<char>((field3 >> 8) & 0xFF));
        rows.push_back(static_cast<char>(field3 & 0xFF));
    };
    add_row(0, 0, 0); // object 0, free
    add_row(1, offset1, 0);
    add_row(1, offset2, 0);
    add_row(2, 2, 0); // objects 3..6 live in object stream 2
    add_row(2, 2, 1);
    add_row(2, 2, 2);
    add_row(2, 2, 3);
    add_row(0, 0, 0); // object 7, free
    add_row(1, offset8, 0);

    const std::string object8 = "<< /Type /XRef /Size 9 /Root 3 0 R /W [1 4 2] "
                                "/Index [0 9] /Length " +
                                std::to_string(rows.size()) + " >>\nstream\n" + rows +
                                "\nendstream";
    out += "8 0 obj\n" + object8 + "\nendobj\n";
    out += "startxref\n" + std::to_string(offset8) + "\n%%EOF\n";
    return out;
}

void test_xref_stream_and_object_stream() {
    const std::string pdf = build_xref_stream_pdf();
    std::string error;
    auto document = open_string(pdf, &error);
    CHECK(document != nullptr);
    if (!document) {
        std::fprintf(stderr, "  error: %s\n", error.c_str());
        return;
    }
    const auto texts = paragraph_texts(*document);
    CHECK(texts.size() == 1);
    if (!texts.empty()) CHECK_STR(texts[0], "Object stream OK");
}

// ---------------------------------------------------------------------------
// 5. Recovery: xref offsets are wrong / trailer missing, objects are intact.
// ---------------------------------------------------------------------------

void test_broken_xref_recovery() {
    std::vector<std::string> objects = {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>",
        stream_body("", "BT /F1 18 Tf 72 700 Td (Recovered text) Tj ET"),
    };
    std::string pdf = make_classic_pdf(objects, 1, 0);
    const size_t startxref = pdf.rfind("startxref");
    if (startxref != std::string::npos) {
        pdf.replace(startxref, pdf.size() - startxref, "startxref\n1\n%%EOF\n");
    }

    std::string error;
    auto document = open_string(pdf, &error);
    CHECK(document != nullptr);
    if (!document) {
        std::fprintf(stderr, "  error: %s\n", error.c_str());
        return;
    }
    const auto texts = paragraph_texts(*document);
    CHECK(texts.size() == 1);
    if (!texts.empty()) CHECK_STR(texts[0], "Recovered text");

    // Objects only, no xref/trailer at all: the raw scanner + /Catalog fallback.
    const size_t xref_keyword = pdf.find("xref\n0 ");
    std::string truncated = xref_keyword == std::string::npos
                                ? pdf
                                : pdf.substr(0, pdf.rfind("\nxref\n") + 1);
    auto recovered = open_string(truncated, &error);
    CHECK(recovered != nullptr);
    if (recovered) {
        const auto recovered_texts = paragraph_texts(*recovered);
        CHECK(recovered_texts.size() == 1);
        if (!recovered_texts.empty()) CHECK_STR(recovered_texts[0], "Recovered text");
    }
}

// ---------------------------------------------------------------------------
// 6. Layout: hyphen joining, paragraph splitting, inline image skipping, TJ.
// ---------------------------------------------------------------------------

void test_layout_and_kerning() {
    const std::string content =
        "q 10 0 0 10 0 0 cm BI /W 1 /H 1 /CS /G /BPC 8 ID \x01\x02 EI Q\n"
        "BT /F1 12 Tf 72 720 Td (Hello wor-) Tj 0 -14 Td (ld) Tj "
        "0 -40 Td (Second paragraph) Tj ET";
    std::vector<std::string> objects = {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>",
        stream_body("", content),
    };
    const std::string pdf = make_classic_pdf(objects, 1, 0);
    std::string error;
    auto document = open_string(pdf, &error);
    CHECK(document != nullptr);
    if (!document) {
        std::fprintf(stderr, "  error: %s\n", error.c_str());
        return;
    }
    const auto texts = paragraph_texts(*document);
    CHECK(texts.size() == 2);
    if (texts.size() == 2) {
        CHECK_STR(texts[0], "Hello world");
        CHECK_STR(texts[1], "Second paragraph");
    }
    CHECK(document->total_chars() == 27);

    Paragraph paragraph;
    CHECK(document->seek((static_cast<uint64_t>(0) << 32) | 1));
    CHECK(document->next(paragraph));
    CHECK(paragraph.token == 1);
    if (!paragraph.runs.empty()) CHECK_STR(paragraph.runs[0].text, "Second paragraph");
    CHECK(document->permille(1) == 0);

    // TJ kerning: a large negative adjustment is rendered as a word gap.
    std::vector<std::string> tj_objects = {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>",
        stream_body("", "BT /F1 24 Tf 72 720 Td [(Hello) -400 (world)] TJ ET"),
    };
    const std::string tj_pdf = make_classic_pdf(tj_objects, 1, 0);
    auto tj_document = open_string(tj_pdf, &error);
    CHECK(tj_document != nullptr);
    if (tj_document) {
        const auto tj_texts = paragraph_texts(*tj_document);
        CHECK(tj_texts.size() == 1);
        if (!tj_texts.empty()) CHECK_STR(tj_texts[0], "Hello world");
    }
}

// ---------------------------------------------------------------------------
// 7. Two pages: token encoding, seek and permille across pages.
// ---------------------------------------------------------------------------

void test_multipage_tokens() {
    std::vector<std::string> objects = {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Resources << /Font << /F1 5 0 R >> >> /Contents 6 0 R >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Resources << /Font << /F1 5 0 R >> >> /Contents 7 0 R >>",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>",
        stream_body("", "BT /F1 12 Tf 72 720 Td (First page) Tj ET"),
        stream_body("", "BT /F1 12 Tf 72 720 Td (Second page) Tj ET"),
    };
    const std::string pdf = make_classic_pdf(objects, 1, 0);
    std::string error;
    auto document = open_string(pdf, &error);
    CHECK(document != nullptr);
    if (!document) return;

    document->rewind();
    Paragraph paragraph;
    CHECK(document->next(paragraph));
    CHECK(paragraph.token == 0);
    CHECK(document->next(paragraph));
    CHECK(paragraph.token == (static_cast<uint64_t>(1) << 32));
    CHECK(document->permille(paragraph.token) == 500);
    CHECK(document->seek(static_cast<uint64_t>(1) << 32));
    CHECK(document->next(paragraph));
    if (!paragraph.runs.empty()) CHECK_STR(paragraph.runs[0].text, "Second page");
    CHECK(!document->next(paragraph));
}

} // namespace

int main() {
    test_simple_classic_pdf();
    test_flate_cyrillic_type0();
    test_broken_pdf();
    test_pdf_without_text_layer();
    test_encrypted_pdf();
    test_type0_without_tounicode();
    test_xref_stream_and_object_stream();
    test_broken_xref_recovery();
    test_layout_and_kerning();
    test_multipage_tokens();

    if (g_failures == 0) {
        std::printf("All PDF tests passed.\n");
        return 0;
    }
    std::printf("%d PDF test check(s) failed.\n", g_failures);
    return 1;
}
