# PaperDIY Reader

An e-ink ebook reader firmware for the **M5Stack M5PaperDIY** (ESP32-S3, 540×960 parallel e-ink panel, two buttons, SD card). It reads TXT, Markdown, FB2, EPUB and PDF, keeps per-book progress, and treats the panel as what it is: a slow, 16-level display with no backlight and no partial-update free lunch.

![File manager](docs/shots/filemanager.png)
![Reading screen](docs/shots/reader.png)

## Features

**Reading**
- Page-based layout with hyphenation: Russian school rules plus the soft hyphens already present in the book.
- Widow and orphan control: a paragraph never leaves a single line at the bottom, a page never starts with a lone last line.
- Three font sizes, PT Serif for book text, JetBrains Mono for UI chrome, all rasterised with FreeType hinting into 1-bit masks.
- Styles from the source: headings, bold, italic, bold-italic, quotes, lists, code, separators.
- Encoding autodetect per file: UTF-8 (with or without BOM), CP1251, UTF-16 LE/BE.

**Navigation**
- A/B page turns with auto-repeat, A+B opens and leaves a book.
- Long A+B starts the book from the beginning.
- Hold B + tap A / hold A + tap B change the font size.
- Real page numbers `N / M` with the reading percentage; the total is counted in the background and recomputed after a font change.
- The last opened book reopens at the saved position after a power cycle; progress is written on every page turn.

**References and footnotes**
- Long A+B in the reader enters reference mode: A/B move the focus between references, the focused line is marked with the same cursor sprite as the file manager.
- A panel at the bottom shows the note text at the smallest reading size, with the note's own heading and a progress bar when it does not fit.
- The page shifts temporarily so the focused reference stays visible above the panel; leaving the mode restores the exact reading position.
- Hold A + tap B / hold B + tap A scroll the note; short A+B leaves the mode.

**File manager**
- Directories in square brackets, icons from the design set (folders, files, locked files), format and size on the right.
- The selected row uses the bold face, the filled icon variant and the cursor sprite — no inverted blocks of ink.
- Files that cannot be opened (above the size limits) are greyed out with a lock icon.
- Scrollbar, and a bottom bar that only exists in the reader.

**E-ink specific**
- Pages refresh with DU (~330 ms) and every 4th page gets a full GC16 pass; the file manager uses DU, small patches use DU4.
- Refresh regions are merged and escalated to a full pass only when it is actually cheaper.
- A boot splash and a three-step loading screen keep the panel from looking dead while a book is parsed.

**Maintenance**
- If a file fails to parse, the reader can rewrite it as UTF-8 (encoding detection with a CP1251 fallback, control characters stripped, bare `&` escaped, closing tag appended), verify that the repaired copy opens, and ask what to do with it: `A — replace` (the broken original is kept as `broken-<name>`) or `B — keep` (the copy is written as `fixed-<name>`).
- A serial debug console behind USB provides screenshots, dumps, benchmarks, key injection and file transfer (`rdr ...` commands, see below).

## Requirements

- M5Stack M5PaperDIY (the firmware targets its ESP32-S3, 540×960 panel, PMIC and SD wiring).
- ESP-IDF **5.5.1** (IDF 6.x does not build the vendor M5Unified I2S driver).
- Python 3.10+ for the tools (Pillow, freetype-py) and for the vendor component fetcher.
- A microSD card formatted as FAT32.

## Build and flash

```bash
git clone --recurse-submodules https://github.com/annabiosis/PaperDIY-reader
cd PaperDIY-reader
python vendor/M5PaperDIY-UserDemo/fetch_repos.py    # pulls M5GFX, M5Unified, LGFX_Fonts at pinned revisions
```

Then, with ESP-IDF 5.5.1 activated (on Windows the helper script does it for you):

```powershell
. .\tools\idf_env.ps1 -Version 5.5.1
cd firmware
idf.py build
idf.py -p COM5 flash monitor
```

On Linux/macOS activate the IDF export script instead of `idf_env.ps1`:

```bash
. $IDF_PATH/export.sh
cd firmware && idf.py build && idf.py -p /dev/ttyACM0 flash
```

The firmware is a normal ESP-IDF project: `firmware/CMakeLists.txt` adds the vendor components as `EXTRA_COMPONENT_DIRS`, `firmware/partitions.csv` defines the layout, and the font/icon atlases are pre-generated C++ files under `firmware/main/text/atlas/` and `firmware/main/ui/`.

## First run

1. Copy books to the SD card (`/sdcard/books/...` is a convenient default).
2. Insert the card and power the device on.
3. The file manager starts in the last used directory. A+B opens the selected file, A+B again returns to the manager.

Progress and settings live in `/sdcard/.reader/progress.json` (positions, page numbers, font size, the "book was open" flag). Delete that file to reset everything.

Size limits: 8 MB for text formats (TXT/MD/FB2/EPUB), 12 MB for PDF; larger files are shown greyed out and are not opened.

## Controls

| Gesture | File manager | Reader |
|---|---|---|
| A | previous entry | previous page |
| B | next entry | next page |
| hold A / hold B | auto-repeat through the list | auto-repeat page turning |
| A+B (short) | open the selected entry | back to the file manager (or leave reference mode) |
| A+B (long) | open the book from the beginning | enter reference mode |
| hold B + tap A | go to the top of the list | larger font |
| hold A + tap B | jump one screen down the list | smaller font |
| power key | handled by the PMIC: a single press power-cycles the board, a double press switches it off | same |

## Host tests

The parsers, the text model and the layout have host tests that need no ESP-IDF and no hardware:

```bash
./tests/run_tests.sh          # Linux/macOS, g++
```

```powershell
.\tests\run_tests.ps1         # Windows, MinGW g++
.\tests\run_tests.ps1 -Filter test_layout
```

The suite covers TXT/MD/FB2/EPUB/PDF parsing, note and reference resolution (labels, targets, broken links), pagination invariants (no text lost or duplicated), hyphenation, widow/orphan rules and the layout's link rectangles. CI runs the same suite on Ubuntu.

## Tools

| Tool | Purpose |
|---|---|
| `tools/idf_env.ps1` | activates the ESP-IDF environment (Windows) |
| `tools/build.ps1` | build + flash shortcut |
| `tools/fonts/gen_fonts.py` | regenerates the font atlas (PT Serif + JetBrains Mono, sizes 14/18/24/28/32) with FreeType mono hinting |
| `tools/fonts/compare_rendering.py` | compares rasterisation methods and sizes, reports ink loss per size |
| `tools/icons/import_icons.py` | packs the exported PNG icons into a 1-bit sprite atlas |
| `tools/make_demo.py` | generates the demo books used during development |
| `tools/make_palette.py` | writes the 16-level palette files (`docs/palette/`) |
| `tools/devtool.py` | serial client: screenshots, dumps, file transfer, benchmarks, key injection |
| `tools/figma*.py` | read a Figma file through the API and dump exact element geometry |

## Project layout

```
firmware/
  main/board/       M5Unified init, SD, battery, e-ink update policy
  main/input/       button gestures and the input task
  main/formats/     TXT, Markdown, FB2, EPUB, PDF parsers
  main/text/        text model, font atlas, paginator, hyphenation
  main/store/       JSON progress and settings
  main/ui/          screens, layout rendering, icon atlas
  main/debug/       serial console
tools/              font/icon/palette generators, serial client
tests/              host tests (no ESP-IDF)
docs/               architecture, engineering decisions, screenshots, palette
vendor/             M5PaperDIY-UserDemo as a git submodule (components are fetched)
```

## Debug console

The firmware exposes a line-based console over USB serial (115200, USB-Serial-JTAG):

| Command | Effect |
|---|---|
| `rdr ping` | version banner |
| `rdr dump` | text of the current screen (file manager listing or reader page) |
| `rdr shot [x y w h]` | grayscale screenshot, read in bands |
| `rdr key A\|B\|AB\|ABL\|UP\|DN`, `rdr burst B 5` | inject gestures |
| `rdr open <path>`, `rdr openb64 <base64>`, `rdr openidx <n>` | open a file |
| `rdr get <path>`, `rdr getb64 <path>`, `rdr put <path> <base64>` | file transfer |
| `rdr ls <dir>`, `rdr rm <path>` | directory listing / delete |
| `rdr stats` | refresh statistics (full/partial flushes, last region, timing) |
| `rdr bench` | panel timing benchmark |
| `rdr clean` | force one full GC16 repaint |
| `rdr heal <path>` | run the encoding repair on a file |
| `rdr limit <bytes>` | override the open size limit (0 restores defaults) |
| `rdr bat`, `rdr pek` | battery voltage/percent, PMIC power-key state |
| `rdr reboot` | real power cycle (keeps progress) |
| `rdr reset` | reset the saved progress |

## Known limitations

- Page numbers depend on the layout, so they are recomputed after a font change (and counted in the background, not instantly).
- PDF only supports files with a text layer; scans are rejected with a message.
- No CSS in EPUB beyond basic block/inline markup, no images, no tables beyond simple ones.
- The power key cannot be remapped: the PMIC handles single and double presses itself.
- Progress is stored on the SD card; a missing card means no progress (and a message on screen).

## License

MIT — see [LICENSE](LICENSE). The vendored `M5PaperDIY-UserDemo` and the components it fetches (M5GFX, M5Unified, LGFX_Fonts) are MIT-licensed by M5Stack; `tests/support/miniz` carries its own permissive license.
