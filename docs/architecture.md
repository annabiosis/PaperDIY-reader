# Firmware architecture

```
firmware/main/
  main.cpp              entry point: board().init() → App::begin() → App::run()
  board/board.*         M5Unified initialization, SD (SDSPI), battery, E-Ink updates
  input/gestures.*      A/B gesture recognition: click, A+B, auto-repeat, font modifiers
  core/                 UTF-8, encodings (CP1251/UTF-16/UTF-8), ByteSource (file/memory)
  formats/              parsers for TXT, MD, FB2, EPUB, PDF → paragraph stream
  text/                 text model, font atlas, page-by-page layout
  store/progress.*      JSON progress and settings on SD (/.reader/progress.json)
  ui/                   drawing of file manager/reading/message screens + App loop
```

## Data flow

1. `App::run()` polls GPIO 4/3 every 10 ms, suppresses bounce (12 ms) and feeds
   stable states to `GestureEngine`.
2. `GestureEngine` turns holds and combinations into a `Gesture` and calls
   `App::on_gesture`.
3. The file manager reads the directory via POSIX (`opendir`/`stat`) on the mounted
   FAT32, filters extensions and sorts folders before files.
4. When a file is opened, `open_document()` selects a parser by extension. All parsers
   implement `Document`: paragraph stream + `seek(token)` + `permille()`.
5. `Paginator` cuts paragraphs into lines using metrics (`LayoutMetrics` is built from
   the current font size) and produces `PageView` — a list of lines with absolute
   coordinates and spans (run fragments).
6. `App::render_reader()` draws the spans using native atlases via
   `pushGrayscaleImage`, then performs a full screen update.

## Reading position

`PagePos{token, char_offset}` — the page address:

- `token` — the paragraph position in the source document (byte offset for text
  formats, `(spine << 32) | offset` for EPUB, `(page << 32) | paragraph` for PDF);
- `char_offset` — the offset inside the paragraph in code points (for pages that began in
  the middle of a paragraph).

That is why the position stays meaningful after a font size change: `Paginator::seek`
finds the paragraph by `token` and steps back by `char_offset`.

## Memory

- Text atlases — 0.85 MB in flash (const), they take no RAM.
- TXT/MD/FB2 are decoded entirely (limit 6–8 MB) into the heap; EPUB keeps only the current
  chapter; PDF extracts text page by page.
- The E-Ink frame buffer (960×540/2 bytes) is held by M5GFX itself in PSRAM.

## Buttons

| Gesture | File manager | Reading |
|---|---|---|
| A | cursor up | previous page |
| B | cursor down | next page |
| A+B (simultaneously, < 500 ms) | enter/open | exit to the file manager |
| A or B hold | auto-repeat (500 ms, step 160 ms) | page-flip auto-repeat |
| hold B + A presses | — | larger font |
| hold A + B presses | — | smaller font |

The "modifier + press" gesture is recognized only before the auto-repeat of the held
button starts, so flipping by holding does not conflict with font adjustment.

## Input

`input/input_queue.cpp` keeps a separate task: GPIO 4/3 polling every 2 ms,
10 ms debounce, `GestureEngine` (click, A+B, long A+B, combinations with
hold, auto-repeat) and a queue of 32 gestures.

At the start of every cycle the UI task drains the queue completely and only then
draws the frame. Two consequences follow from this:

- presses are not blocked by the E-Ink wave (200–400 ms) — they accumulate in the queue;
- a batch of presses collapses into one state change and **one** panel update
  (N presses of B = cursor +N and one frame).

The meaning of `Gesture::FontUp` / `FontDown` depends on the screen: in reading it is the font
size, in the file manager — "go to the start of the list" and "page down".

## Line layout

- `text/hyphenate.cpp` — school hyphenation rules for Cyrillic: hyphenation is allowed
  after a vowel before a single consonant, between consonants of a cluster (before the last one),
  "й/ь/ъ/ы" do not start a line, minimum 2 letters on each side. A soft hyphen from
  the source text counts as an explicit break point.
- `build_chars` marks hyphenation points on characters, `build_line`, on overflow,
  chooses the longest prefix that fits together with the hyphen, and marks the line
  `hyphenated`; the renderer draws the hyphen.
- Widow/orphan control: a paragraph that has room left for only one line is moved to the
  next page entirely; if after the break a paragraph still had one last line left — that line
  moves as well, so the next page does not start with an orphan.

## Page navigation

`Paginator` is a sequential machine: pages are determined by a pass from the beginning of the
document. Therefore:

- when opening from a saved place the position is restored exactly (the page may
  begin in the middle of a paragraph);
- on a **font size change** the page snaps to the natural boundary containing
  the previous position (`snap_to_layout` → `walk_to_position`);
- "back" walks the stack of page starts; when the stack is exhausted, the window is rebuilt from
  the nearest checkpoint (`extend_history_backwards`). Only pages that **end before** the
  current one get into the window — otherwise they would overlap it and flipping
  would loop;
- checkpoints (paragraph position every 10 pages) also store the page number; they also
  bound the cost of the passes.

## Screen update

Drawing and output to the panel are separated:

1. `board().begin_frame()` — collection of "dirty" regions starts.
2. Screen functions draw only the changed parts and call `board().touch(rect)`.
   The application knows what to change: `kHeaderPart / kContentPart / kFooterPart`, and in
   the file manager also the specific list rows (`row_region`).
3. `board().commit_frame(mode)`:
   - merges intersecting/adjacent rectangles;
   - if the total area > 68% of the screen or 20 partial updates have accumulated —
     one full pass, otherwise one `display(x, y, w, h)` per region;
   - the mode is chosen by the caller: book pages — `Quality` (GC16, 16 gray levels,
     uniform contrast), file list — `Fast` (DU, 1 bit, responsive scrolling),
     cursor bars and small patches — `Fastest` (DU4).

The cursor in the file manager is two filled triangles at the edges of the row; when
it moves, only the 22 px bars on the left and right are redrawn, the row text
remains untouched. This means less ink and fewer waveform frames.

On-device check: `rdr stats` shows `full`, `partial`, the last
rectangle and its duration in ms. Cursor movement gives `partial=2`
(bars `22×90`, ~200 ms), a page flip — `full=…`, 315–399 ms.

### Measured modes (`rdr bench`)

| region | quality | text | fast | fastest |
|---|---:|---:|---:|---:|
| 540×54 | 821 | 822 | 248 | 182 |
| 540×480 | 1053 | 1055 | 324 | 240 |
| 540×960 | 1284 | 1287 | 399 | 297 |

## Tests

`tests/` — host tests (MinGW g++) for parsers and layout, without ESP-IDF:
`test_txt`, `test_md`, `test_fb2`, `test_epub`, `test_pdf`, `test_layout`.
Run: `.\tests\run_tests.ps1`.
