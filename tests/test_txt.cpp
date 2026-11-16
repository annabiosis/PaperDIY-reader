// Host tests for the plain-text reader (encoding detection, paragraphs, seek).
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "core/byte_source.h"
#include "core/encoding.h"
#include "formats/formats.h"
#include "text/doc.h"

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(condition)                                                                       \
    do {                                                                                        \
        ++g_checks;                                                                             \
        if (!(condition)) {                                                                     \
            ++g_failures;                                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);            \
        }                                                                                       \
    } while (0)

#define CHECK_STR(actual, expected)                                                            \
    do {                                                                                        \
        ++g_checks;                                                                             \
        if (std::string(actual) != std::string(expected)) {                                     \
            ++g_failures;                                                                       \
            std::fprintf(stderr, "FAIL %s:%d: got \"%s\" expected \"%s\"\n", __FILE__, __LINE__, \
                         std::string(actual).c_str(), std::string(expected).c_str());           \
        }                                                                                       \
    } while (0)

std::string paragraph_text(const reader::Paragraph& paragraph) {
    std::string text;
    for (const auto& run : paragraph.runs) text += run.text;
    return text;
}

std::unique_ptr<reader::Document> open_string(const std::string& content, std::string* error) {
    auto source = std::make_unique<reader::MemoryByteSource>(content);
    return reader::open_txt(std::move(source), error);
}

std::vector<std::string> all_paragraphs(reader::Document& document) {
    std::vector<std::string> out;
    document.rewind();
    reader::Paragraph paragraph;
    while (document.next(paragraph)) out.push_back(paragraph_text(paragraph));
    return out;
}

void test_utf8_blank_lines() {
    const std::string text =
        "Первая строка.\n"
        "Продолжение первой.\n"
        "\n"
        "Второй абзац.\n"
        "\n"
        "\n"
        "Третий абзац.\n";
    std::string error;
    auto document = open_string(text, &error);
    CHECK(document != nullptr);
    if (!document) return;

    const auto paragraphs = all_paragraphs(*document);
    CHECK(paragraphs.size() == 3);
    if (paragraphs.size() == 3) {
        CHECK_STR(paragraphs[0], "Первая строка. Продолжение первой.");
        CHECK_STR(paragraphs[1], "Второй абзац.");
        CHECK_STR(paragraphs[2], "Третий абзац.");
    }
}

void test_cp1251() {
    // "Привет" in CP1251: CF F0 E8 E2 E5 F2
    const std::string text = "\xCF\xF0\xE8\xE2\xE5\xF2 \xEC\xE8\xF0";
    std::string error;
    auto document = open_string(text, &error);
    CHECK(document != nullptr);
    if (!document) return;
    const auto paragraphs = all_paragraphs(*document);
    CHECK(paragraphs.size() == 1);
    if (!paragraphs.empty()) CHECK_STR(paragraphs[0], "Привет мир");
}

void test_utf8_bom() {
    const std::string text = "\xEF\xBB\xBF" "BOM text here";
    std::string error;
    auto document = open_string(text, &error);
    CHECK(document != nullptr);
    if (!document) return;
    const auto paragraphs = all_paragraphs(*document);
    CHECK(paragraphs.size() == 1);
    if (!paragraphs.empty()) CHECK_STR(paragraphs[0], "BOM text here");
}

void test_single_line_book() {
    // No blank lines at all: every line becomes its own paragraph.
    const std::string text = "строка один\nстрока два\nстрока три\n";
    std::string error;
    auto document = open_string(text, &error);
    CHECK(document != nullptr);
    if (!document) return;
    const auto paragraphs = all_paragraphs(*document);
    CHECK(paragraphs.size() == 3);
    if (paragraphs.size() == 3) {
        CHECK_STR(paragraphs[0], "строка один");
        CHECK_STR(paragraphs[2], "строка три");
    }
}

void test_seek_and_permille() {
    std::string text;
    for (int i = 0; i < 20; ++i) {
        text += "Абзац номер " + std::to_string(i + 1) + "\n\n";
    }
    std::string error;
    auto document = open_string(text, &error);
    CHECK(document != nullptr);
    if (!document) return;

    std::vector<uint64_t> tokens;
    document->rewind();
    reader::Paragraph paragraph;
    while (document->next(paragraph)) tokens.push_back(paragraph.token);
    CHECK(tokens.size() == 20);

    if (tokens.size() == 20) {
        CHECK(document->seek(tokens[10]));
        CHECK(document->next(paragraph));
        CHECK_STR(paragraph_text(paragraph), "Абзац номер 11");
        CHECK(document->permille(tokens[0]) == 0);
        CHECK(document->permille(tokens[19]) > 800);
        CHECK(document->permille(tokens[19]) < 1000);
    }
}

void test_empty_and_garbage() {
    std::string error;
    auto empty = open_string("   \n\n\t\n", &error);
    CHECK(empty == nullptr);

    auto binary = open_string(std::string("\x01\x02\x03", 3), &error);
    CHECK(binary == nullptr);
}

void test_windows_line_endings() {
    const std::string text = "Первая\r\nВторая\r\n\r\nТретья\r\n";
    std::string error;
    auto document = open_string(text, &error);
    CHECK(document != nullptr);
    if (!document) return;
    const auto paragraphs = all_paragraphs(*document);
    CHECK(paragraphs.size() == 2);
    if (paragraphs.size() == 2) {
        CHECK_STR(paragraphs[0], "Первая Вторая");
        CHECK_STR(paragraphs[1], "Третья");
    }
}

void test_encoding_detection() {
    const uint8_t utf8[] = {0xD0, 0x9F, 0xD1, 0x80, 0xD0, 0xB8};
    CHECK(reader::detect_encoding(utf8, sizeof(utf8)) == reader::TextEncoding::Utf8);

    const uint8_t cp1251[] = {0xCF, 0xF0, 0xE8, 0xE2, 0xE5, 0xF2};
    CHECK(reader::detect_encoding(cp1251, sizeof(cp1251)) == reader::TextEncoding::Cp1251);

    const uint8_t bom[] = {0xEF, 0xBB, 0xBF, 'a'};
    CHECK(reader::detect_encoding(bom, sizeof(bom)) == reader::TextEncoding::Utf8);

    const uint8_t utf16[] = {0xFF, 0xFE, 'a', 0x00};
    CHECK(reader::detect_encoding(utf16, sizeof(utf16)) == reader::TextEncoding::Utf16Le);
}

} // namespace

int main() {
    test_utf8_blank_lines();
    test_cp1251();
    test_utf8_bom();
    test_single_line_book();
    test_seek_and_permille();
    test_empty_and_garbage();
    test_windows_line_endings();
    test_encoding_detection();

    std::printf("%d checks, %d failure(s)\n", g_checks, g_failures);
    if (g_failures == 0) std::printf("OK all TXT tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
