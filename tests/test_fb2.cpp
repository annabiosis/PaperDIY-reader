// Host test for the FB2 parser (pure C++17, no ESP-IDF).
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "core/byte_source.h"
#include "core/utf8.h"
#include "formats/formats.h"
#include "text/doc.h"
#include "text/types.h"

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                           \
        } else {                                                                                   \
            std::printf("OK %s:%d: %s\n", __FILE__, __LINE__, #cond);                             \
        }                                                                                          \
    } while (0)

bool read_file(const std::string& path, std::string& out) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    out.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return true;
}

bool read_fixture(const std::string& name, std::string& out) {
    std::vector<std::string> directories;
    const std::string source_file = __FILE__;
    const size_t slash = source_file.find_last_of("/\\");
    if (slash != std::string::npos) {
        directories.push_back(source_file.substr(0, slash + 1) + "fixtures/fb2/");
    }
    directories.push_back("ereader/tests/fixtures/fb2/");
    directories.push_back("tests/fixtures/fb2/");
    directories.push_back("fixtures/fb2/");
    for (const std::string& directory : directories) {
        if (read_file(directory + name, out)) return true;
    }
    std::printf("FAIL cannot find fixture %s\n", name.c_str());
    return false;
}

std::unique_ptr<reader::Document> open_memory(const std::string& bytes, std::string& error) {
    return reader::open_fb2(std::make_unique<reader::MemoryByteSource>(bytes), &error);
}

std::vector<reader::Paragraph> collect(reader::Document& document) {
    std::vector<reader::Paragraph> paragraphs;
    document.rewind();
    reader::Paragraph paragraph;
    size_t guard = 0;
    while (document.next(paragraph) && guard++ < 200000) paragraphs.push_back(paragraph);
    return paragraphs;
}

std::string text_of(const reader::Paragraph& paragraph) {
    std::string text;
    for (const reader::Run& run : paragraph.runs) text += run.text;
    return text;
}

const reader::Paragraph* find_by_text(const std::vector<reader::Paragraph>& paragraphs,
                                      const std::string& text) {
    for (const reader::Paragraph& paragraph : paragraphs) {
        if (text_of(paragraph) == text) return &paragraph;
    }
    return nullptr;
}

void dump(const std::vector<reader::Paragraph>& paragraphs) {
    std::printf("--- parsed paragraphs (%d) ---\n", static_cast<int>(paragraphs.size()));
    for (size_t i = 0; i < paragraphs.size(); ++i) {
        std::printf("[%d] kind=%d level=%d token=%llu text=%s\n", static_cast<int>(i),
                    static_cast<int>(paragraphs[i].kind), static_cast<int>(paragraphs[i].level),
                    static_cast<unsigned long long>(paragraphs[i].token),
                    text_of(paragraphs[i]).c_str());
    }
    std::printf("--- end ---\n");
}

void test_utf8_fixture() {
    std::printf("\n== UTF-8 fixture ==\n");
    std::string raw;
    const bool have_fixture = read_fixture("utf8.fb2", raw);
    CHECK(have_fixture);
    if (!have_fixture) return;

    std::string error;
    std::unique_ptr<reader::Document> document = open_memory(raw, error);
    CHECK(document != nullptr);
    if (!document) {
        std::printf("FAIL open_fb2 error: %s\n", error.c_str());
        return;
    }

    CHECK(document->info().title == u8"Тестовая книга");
    CHECK(document->info().author == u8"Иван Петрович Сидоров, Анна Морозова");
    CHECK(document->total_chars() == reader::utf8_length(raw));

    const std::vector<reader::Paragraph> paragraphs = collect(*document);
    CHECK(paragraphs.size() == 11);
    if (paragraphs.size() != 11) dump(paragraphs);

    auto at = [&paragraphs](size_t index) -> const reader::Paragraph* {
        return index < paragraphs.size() ? &paragraphs[index] : nullptr;
    };

    const reader::Paragraph* heading = at(0);
    CHECK(heading != nullptr);
    CHECK(heading && heading->kind == reader::ParaKind::Heading);
    CHECK(heading && heading->level == 1);
    CHECK(heading && text_of(*heading) == u8"Глава первая");

    const reader::Paragraph* first = at(1);
    CHECK(first != nullptr);
    CHECK(first && first->kind == reader::ParaKind::Text);
    CHECK(first && text_of(*first) == u8"Первый абзац и курсив, жирный и A & B.");
    bool italic_run = false;
    bool bold_run = false;
    if (first) {
        for (const reader::Run& run : first->runs) {
            if (run.style.face == reader::Face::Italic &&
                run.text.find(u8"курсив") != std::string::npos) {
                italic_run = true;
            }
            if (run.style.face == reader::Face::Bold &&
                run.text.find(u8"жирный") != std::string::npos) {
                bold_run = true;
            }
        }
    }
    CHECK(italic_run);
    CHECK(bold_run);

    CHECK(at(2) && at(2)->kind == reader::ParaKind::Blank);
    CHECK(at(3) && text_of(*at(3)) == u8"Второй абзац.");
    CHECK(at(4) && text_of(*at(4)) == u8"Третий абзац.");

    CHECK(at(5) && at(5)->kind == reader::ParaKind::Heading);
    CHECK(at(5) && at(5)->level == 2);
    CHECK(at(5) && text_of(*at(5)) == u8"Подзаголовок");

    const reader::Paragraph* code = at(6);
    CHECK(code && code->kind == reader::ParaKind::Code);
    CHECK(code && text_of(*code) == u8"int main() { return 0; }");
    bool code_mono = code != nullptr && !code->runs.empty();
    if (code) {
        for (const reader::Run& run : code->runs) {
            if (run.style.face != reader::Face::Mono) code_mono = false;
        }
    }
    CHECK(code_mono);

    CHECK(at(7) && text_of(*at(7)) == u8"Ячейка 1 | Ячейка 2");
    CHECK(at(8) && text_of(*at(8)) == u8"Ячейка 3 | Ячейка 4");
    CHECK(at(9) && text_of(*at(9)) ==
                       u8"Символы: & < > \" ' П A и мусор конец.");
    CHECK(at(10) && text_of(*at(10)) == u8"Картинка: и текст.");

    CHECK(find_by_text(paragraphs, u8"Второй абзац.") != nullptr);

    bool binary_leaked = false;
    for (const reader::Paragraph& paragraph : paragraphs) {
        if (text_of(paragraph).find("iVBOR") != std::string::npos) binary_leaked = true;
    }
    CHECK(!binary_leaked);

    // Seek to the third text paragraph ("Третий абзац.").
    const reader::Paragraph* target = at(4);
    CHECK(target != nullptr);
    if (target) {
        const uint64_t token = target->token;
        CHECK(document->seek(token));
        reader::Paragraph paragraph;
        CHECK(document->next(paragraph));
        CHECK(paragraph.token == token);
        CHECK(text_of(paragraph) == u8"Третий абзац.");

        CHECK(document->permille(token) ==
              static_cast<uint32_t>(token * 1000 / raw.size()));
        CHECK(document->permille(0) == 0);
        CHECK(document->permille(raw.size()) == 1000);
        CHECK(document->permille(raw.size() * 4) == 1000);

        // An unknown token inside the tag positions at the next paragraph.
        CHECK(!document->seek(token + 1));
        CHECK(document->next(paragraph));
        CHECK(paragraph.token > token);

        document->rewind();
        CHECK(document->next(paragraph));
        CHECK(paragraph.token == paragraphs.front().token);
    }
}

void test_cp1251_fixture() {
    std::printf("\n== CP1251 fixture ==\n");
    std::string raw;
    const bool have_fixture = read_fixture("cp1251.fb2", raw);
    CHECK(have_fixture);
    if (!have_fixture) return;

    std::string error;
    std::unique_ptr<reader::Document> document = open_memory(raw, error);
    CHECK(document != nullptr);
    if (!document) {
        std::printf("FAIL open_fb2 error: %s\n", error.c_str());
        return;
    }
    CHECK(document->info().title == u8"Кириллица");
    CHECK(document->info().author == u8"Пётр Иванов");

    const std::vector<reader::Paragraph> paragraphs = collect(*document);
    CHECK(paragraphs.size() == 2);
    if (paragraphs.size() != 2) dump(paragraphs);
    CHECK(paragraphs.size() > 0 && paragraphs[0].kind == reader::ParaKind::Heading);
    CHECK(paragraphs.size() > 0 && text_of(paragraphs[0]) == u8"Глава");
    CHECK(paragraphs.size() > 1 && text_of(paragraphs[1]) == u8"Привет, мир! Это CP1251.");

    bool replacement_found = false;
    for (const reader::Paragraph& paragraph : paragraphs) {
        if (text_of(paragraph).find("\xEF\xBF\xBD") != std::string::npos) replacement_found = true;
    }
    CHECK(!replacement_found);
}

void test_broken_xml() {
    std::printf("\n== Broken XML ==\n");
    std::string raw;
    const bool have_fixture = read_fixture("broken.fb2", raw);
    CHECK(have_fixture);
    if (have_fixture) {
        std::string error;
        std::unique_ptr<reader::Document> document = open_memory(raw, error);
        CHECK(document != nullptr);
        if (document) {
            const std::vector<reader::Paragraph> paragraphs = collect(*document);
            CHECK(!paragraphs.empty());
            CHECK(!paragraphs.empty() && text_of(paragraphs[0]) == u8"текст");
        }
    }

    std::string error;
    std::unique_ptr<reader::Document> document = open_memory("<body><p>a<emphasis>b", error);
    CHECK(document != nullptr);
    if (document) {
        const std::vector<reader::Paragraph> paragraphs = collect(*document);
        CHECK(paragraphs.size() == 1);
        CHECK(!paragraphs.empty() && text_of(paragraphs[0]) == "ab");
    }

    document = open_memory("<FictionBook><body><p>раз</p></p><p>два", error);
    CHECK(document != nullptr);
    if (document) {
        const std::vector<reader::Paragraph> paragraphs = collect(*document);
        CHECK(paragraphs.size() == 2);
        if (paragraphs.size() != 2) dump(paragraphs);
        CHECK(!paragraphs.empty() && text_of(paragraphs[0]) == u8"раз");
        CHECK(paragraphs.size() > 1 && text_of(paragraphs[1]) == u8"два");
    }
}

void test_garbage_and_limits() {
    std::printf("\n== Garbage and limits ==\n");
    std::string garbage;
    const bool have_fixture = read_fixture("garbage.fb2", garbage);
    CHECK(have_fixture);
    if (have_fixture) {
        std::string error;
        std::unique_ptr<reader::Document> document = open_memory(garbage, error);
        CHECK(document == nullptr);
        CHECK(!error.empty());
    }

    std::string error;
    std::unique_ptr<reader::Document> document =
        open_memory("<html><head><title>T</title></head><body>x</body></html>", error);
    CHECK(document == nullptr);
    CHECK(!error.empty());

    std::string empty;
    CHECK(read_fixture("empty.fb2", empty));
    CHECK(empty.empty());
    document = open_memory(empty, error);
    CHECK(document == nullptr);
    CHECK(!error.empty());

    const std::string oversized(6u * 1024u * 1024u + 1, 'a');
    document = open_memory(oversized, error);
    CHECK(document == nullptr);
    CHECK(error == "файл слишком большой");

    // CP1251 text grows on conversion, so a 4 MB source exceeds the 6 MB cap.
    const std::string cp1251_bomb(4u * 1024u * 1024u, static_cast<char>(0xD0));
    document = open_memory(cp1251_bomb, error);
    CHECK(document == nullptr);
    CHECK(error == "файл слишком большой");
}

void test_utf8_bom() {
    std::printf("\n== UTF-8 BOM ==\n");
    std::string raw;
    if (!read_fixture("utf8.fb2", raw)) {
        CHECK(false);
        return;
    }
    const std::string with_bom = std::string("\xEF\xBB\xBF") + raw;
    std::string error;
    std::unique_ptr<reader::Document> document = open_memory(with_bom, error);
    CHECK(document != nullptr);
    if (!document) return;
    CHECK(document->info().title == u8"Тестовая книга");
    CHECK(document->total_chars() == reader::utf8_length(raw));
    const std::vector<reader::Paragraph> paragraphs = collect(*document);
    CHECK(!paragraphs.empty());
    CHECK(!paragraphs.empty() && text_of(paragraphs[0]) == u8"Глава первая");
}

void test_blocks_and_inline() {
    std::printf("\n== Blocks, headings and inline markup ==\n");
    std::string error;
    std::unique_ptr<reader::Document> document = open_memory(
        "<FictionBook><body><section><title>Один</title>"
        "<section><title><p>Два</p></title><p>текст</p></section>"
        "</section></body></FictionBook>",
        error);
    CHECK(document != nullptr);
    if (document) {
        const std::vector<reader::Paragraph> paragraphs = collect(*document);
        CHECK(paragraphs.size() == 3);
        if (paragraphs.size() != 3) dump(paragraphs);
        CHECK(paragraphs.size() > 0 && paragraphs[0].kind == reader::ParaKind::Heading);
        CHECK(paragraphs.size() > 0 && paragraphs[0].level == 1);
        CHECK(paragraphs.size() > 0 && text_of(paragraphs[0]) == u8"Один");
        CHECK(paragraphs.size() > 1 && paragraphs[1].level == 2);
        CHECK(paragraphs.size() > 1 && text_of(paragraphs[1]) == u8"Два");
        CHECK(paragraphs.size() > 2 && text_of(paragraphs[2]) == u8"текст");
    }

    document = open_memory(
        "<body><section>"
        "<epigraph><p>цитата</p></epigraph>"
        "<cite><p>сноска</p></cite>"
        "<poem><stanza><v>строка</v></stanza></poem>"
        "<annotation><p>аннотация</p></annotation>"
        "<p>a<emphasis>b</emphasis><strong>c</strong><code>d</code>"
        "<strikethrough>e</strikethrough></p>"
        "</section></body>",
        error);
    CHECK(document != nullptr);
    if (document) {
        const std::vector<reader::Paragraph> paragraphs = collect(*document);
        CHECK(paragraphs.size() == 5);
        if (paragraphs.size() != 5) dump(paragraphs);
        CHECK(paragraphs.size() > 0 && paragraphs[0].kind == reader::ParaKind::Quote);
        CHECK(paragraphs.size() > 0 && text_of(paragraphs[0]) == u8"цитата");
        CHECK(paragraphs.size() > 1 && paragraphs[1].kind == reader::ParaKind::Quote);
        CHECK(paragraphs.size() > 1 && text_of(paragraphs[1]) == u8"сноска");
        CHECK(paragraphs.size() > 2 && paragraphs[2].kind == reader::ParaKind::Text);
        CHECK(paragraphs.size() > 2 && text_of(paragraphs[2]) == u8"строка");
        CHECK(paragraphs.size() > 3 && paragraphs[3].kind == reader::ParaKind::Quote);
        CHECK(paragraphs.size() > 3 && text_of(paragraphs[3]) == u8"аннотация");
        const reader::Paragraph* mixed = paragraphs.size() > 4 ? &paragraphs[4] : nullptr;
        CHECK(mixed != nullptr);
        CHECK(mixed && text_of(*mixed) == "abcde");
        bool italic_b = false;
        bool bold_c = false;
        bool mono_d = false;
        bool plain_e = false;
        if (mixed) {
            for (const reader::Run& run : mixed->runs) {
                if (run.style.face == reader::Face::Italic && run.text == "b") italic_b = true;
                if (run.style.face == reader::Face::Bold && run.text == "c") bold_c = true;
                if (run.style.face == reader::Face::Mono && run.text == "d") mono_d = true;
                if (run.style.face == reader::Face::Regular && run.text == "e") plain_e = true;
            }
        }
        CHECK(italic_b);
        CHECK(bold_c);
        CHECK(mono_d);
        CHECK(plain_e);
    }
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_fb2 start\n");
    test_utf8_fixture();
    test_cp1251_fixture();
    test_broken_xml();
    test_garbage_and_limits();
    test_utf8_bom();
    test_blocks_and_inline();

    std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::printf("OK all FB2 tests passed\n");
        return 0;
    }
    std::printf("FAIL %d FB2 test check(s) failed\n", g_failures);
    return 1;
}
