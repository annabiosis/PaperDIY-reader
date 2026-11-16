# Changelog

All notable changes to this firmware are listed here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/spec/v2.0.0.html), mapped onto the firmware contract:

- **MAJOR** — breaks data on the SD card or a user-facing contract (the progress file schema,
  the `broken-` / `fixed-` file naming, the serial console protocol).
- **MINOR** — new features without migration: a new book format, a new gesture, a new panel
  behaviour.
- **PATCH** — fixes and performance work that keep data and contracts intact.

The version is defined once, in `firmware/CMakeLists.txt` (`PROJECT_VER`), and reported by
`rdr version` and the boot log.

## [1.0.0] - 2026-11-16

First public release.

### Reading
- Paginated reader for TXT, Markdown, FB2, EPUB and PDF (text layer only).
- Encoding autodetect per file: UTF-8 with or without BOM, CP1251, UTF-16 LE/BE.
- Hyphenation with Russian school rules and soft hyphens from the book; widow and orphan
  control so a paragraph never leaves a single line behind.
- Three font sizes from a hinted PT Serif / JetBrains Mono atlas rasterised as 1-bit masks;
  headings, bold, italic, bold-italic, quotes, lists, code and separators.
- Real page numbers with the reading percentage; the total is counted in the background and
  recomputed after a font change.

### Navigation and references
- A/B page turns with auto-repeat, A+B opens and leaves a book, long A+B starts from the
  beginning.
- Hold B + tap A / hold A + tap B change the font size.
- Reference mode: long A+B enters, A/B move the focus, a cursor marks the focused reference and
  a panel at the bottom shows the note. Short notes size the panel to their content, long ones
  keep half the screen and scroll. The page shifts temporarily so the pointer stays visible;
  the saved reading position is never modified.
- The last book reopens at its saved position after a power cycle.

### File manager
- Directories in brackets, format and size metadata, icons, bold selected row with a cursor.
- A scrollbar, and locked rows (file above the open limit) marked with a lock icon and the
  italic face.

### E-ink behaviour
- DU page refreshes with a periodic GC16 cleanup, GC16 for text-replacement bars, DU4 for small
  patches.
- Refresh regions are merged before the coverage decision, so small edits do not escalate to a
  full pass; the reference panel and single-line focus moves repaint partial regions only.

### Maintenance
- On-device repair of files with a broken encoding, with a confirmation dialog and
  `broken-<name>` / `fixed-<name>` naming.
- Boot splash and a three-step loading screen for slow opens.
- Serial debug console: screenshots in bands, dumps, benchmarks, gesture injection, chunked
  file transfer (`rdr append`), refresh statistics and `rdr note` for the reference panel.

[1.0.0]: https://github.com/annabiosis/PaperDIY-reader/releases/tag/v1.0.0
