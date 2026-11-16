// Checks that FB2 note references are resolved: every <a l:href="#id"> points at a
// note body, and the note text is not empty.
#include <cstdio>
#include <memory>
#include <string>

#include "core/byte_source.h"
#include "formats/formats.h"
#include "text/layout.h"
#include "text/doc.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what) {
  ++g_checks;
  if (!condition) {
    ++g_failures;
    std::printf("FAIL %s\n", what);
  }
}

const char kBook[] =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<FictionBook xmlns=\"http://www.gribuser.ru/xml/fictionbook/2.0\""
    " xmlns:l=\"http://www.w3.org/1999/xlink\">\n"
    "<description><title-info><book-title>Сноски</book-title></title-info></description>\n"
    "<body><section>\n"
    "<p>Первый абзац со сноской<a l:href=\"#note_1\">[1]</a> и продолжением.</p>\n"
    "<p>Второй абзац с двумя: <a l:href=\"#note_2\">[2]</a> и <a l:href=\"#note_3\">[3]</a>.</p>\n"
    "<p>Ссылка в пустоту <a l:href=\"#missing\">[9]</a> не должна ломать разбор.</p>\n"
    "</section></body>\n"
    "<body name=\"notes\">\n"
    "<section id=\"note_1\"><title><p>1</p></title><p>Текст первой сноски.</p></section>\n"
    "<section id=\"note_2\"><title><p>Комментарий</p></title><p>Текст второй сноски.</p></section>\n"
    "<section id=\"note_3\"><title><p>3</p></title><p>Текст третьей сноски.</p></section>\n"
    "</body>\n"
    "</FictionBook>\n";

} // namespace

int main() {
  std::string error;
  auto document = reader::open_document(reader::Format::Fb2,
                                        std::make_unique<reader::MemoryByteSource>(kBook), &error);
  check(document != nullptr, "document parses");
  if (!document) return 1;

  check(document->note_count() == 3, "three notes are collected");
  std::printf("notes: %zu\n", document->note_count());
  check(document->note(0) == "Текст первой сноски.", "first note text");
  check(document->note(2) == "Текст третьей сноски.", "third note text");
  // A numeric heading is the note number, not a title; a real heading is kept.
  check(document->note_title(0).empty(), "numeric note heading is dropped");
  check(document->note_title(1) == "Комментарий", "note heading is kept");

  reader::Paragraph paragraph;
  int links = 0;
  int resolved = 0;
  int paragraphs = 0;
  while (document->next(paragraph)) {
    ++paragraphs;
    for (const auto& link : paragraph.links) {
      ++links;
      check(link.run < paragraph.runs.size(), "link run in range");
      check(link.end > link.begin, "link range is not empty");
      const std::string ref = paragraph.runs[link.run].text.substr(link.begin, link.end - link.begin);
      std::printf("  '%s' -> note %u\n", ref.c_str(), link.note);
      if (link.note > 0) {
        ++resolved;
        check(link.note <= document->note_count(), "note index in range");
      }
    }
  }
  check(paragraphs == 3, "notes body is not mixed into the text");
  check(links == 4, "four references are found (three notes plus the broken one)");
  check(resolved == 3, "the broken reference stays unresolved");


  // Layout must expose the reference rectangles so the UI can draw the focus mark
  // and the bottom panel without re-measuring text.
  {
    reader::LayoutMetrics metrics;
    metrics.x = 20;
    metrics.y = 10;
    metrics.width = 480;
    metrics.height = 600;
    metrics.size_index = 1;
    auto layout_document = reader::open_document(
        reader::Format::Fb2, std::make_unique<reader::MemoryByteSource>(kBook), &error);
    check(layout_document != nullptr, "document for layout parses");
    reader::Paginator paginator(*layout_document, metrics);
    reader::PageView page;
    int segments = 0;
    while (paginator.next(page)) {
      for (const auto& line : page.lines) {
        for (const auto& link : line.links) {
          ++segments;
          check(link.width > 0, "link segment has width");
          check(link.x >= metrics.x, "link segment starts inside the text block");
        }
      }
    }
    std::printf("layout link segments: %d\n", segments);
    // One reference is one segment, but a reference that wraps over a line break
    // produces a segment per line, so only the lower bound is fixed.
    check(segments >= 4, "every reference produces at least one layout segment");
  }


  // The panel must be able to print the reference label exactly as in the book,
  // independent of the internal note index.
  {
    auto label_document = reader::open_document(
        reader::Format::Fb2, std::make_unique<reader::MemoryByteSource>(kBook), &error);
    check(label_document != nullptr, "document for labels parses");
    reader::LayoutMetrics metrics;
    metrics.x = 20;
    metrics.y = 10;
    metrics.width = 480;
    metrics.height = 600;
    metrics.size_index = 1;
    reader::Paginator paginator(*label_document, metrics);
    reader::PageView page;
    int checked = 0;
    while (paginator.next(page)) {
      for (const auto& line : page.lines) {
        for (const auto& link : line.links) {
          if (link.para >= page.paragraphs.size()) continue;
          const reader::Paragraph& paragraph = page.paragraphs[link.para];
          if (link.run >= paragraph.runs.size()) continue;
          const std::string& text = paragraph.runs[link.run].text;
          if (link.begin >= text.size() || link.end > text.size()) continue;
          const std::string label = text.substr(link.begin, link.end - link.begin);
          check(!label.empty(), "link label is not empty");
          std::printf("  label '%s' -> note %u\n", label.c_str(), link.note);
          ++checked;
        }
      }
    }
    check(checked >= 4, "labels are available for every reference");
  }

  std::printf("%d checks, %d failure(s)\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
