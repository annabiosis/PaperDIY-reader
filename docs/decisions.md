# Decisions taken on the specification

Date: 2026-11-13. Based on specification v1.1 and the customer's answers.

## Device and environment

1. **Orientation is portrait 540×960.** The panel is initialized by M5GFX with
   `offset_rotation = 3`, so the logical screen is already portrait; PDF is also
   displayed in portrait (text reflow).
2. **ESP-IDF.** The target version per the specification is 5.5.1. Version 6.1 is installed
   on the machine; the project and the upstream components are adapted with patches (see
   `firmware/patches/`). The build script works with both versions.
3. **PWR is not intercepted.** Power on/off/boot is the M5PM1 system layer.
   The reader does not use PWR for navigation. Progress is saved
   periodically (30 s / 10 page flips) and on A+B exit, so that the loss
   on power off is minimal.
4. **Progress** is a JSON file `/.reader/progress.json` on the SD card (inspectable),
   written atomically (tmp + rename). It also stores the last
   opened file, so that the file manager places the cursor on it.

## Reader

5. **Navigation.** A — back/up, B — forward/down, A+B — open/exit.
   Auto-repeat after 500 ms of holding, step 150 ms; the UI coalesces repeats
   so as not to queue more than one E-Ink update.
6. **Font size (at the customer's direction).**
   - increase: hold **B**, with short presses of **A**;
   - decrease: hold **A**, with short presses of **B**.
   The gesture is recognized only while the held button has not gone into auto-repeat
   (the first 500 ms), so it does not conflict with page flipping. Size — 3 steps,
   saved in `/.reader/settings.json`.
7. **Page format.** Page-by-page layout for all text formats
   (TXT/MD/FB2/EPUB/PDF text). There is no scrolling: E-Ink does not like partial
   updates. Word wrapping, no hyphenation, left alignment.
8. **PDF — text extraction and reflow to the screen** (agreed).
   Text PDFs are supported (WinAnsi/ToUnicode, FlateDecode, page tree,
   including object streams). Scanned PDFs without a text layer
   show the message "no text layer"; there is no raster rendering.
9. **EPUB/FB2 images are skipped in v1** with the text placeholder
   `[image]`. The structure of the parsers allows them to be added later.
10. **"Recent files" are not implemented** (not part of v1). The last opened
    file is needed only to position the cursor in the file manager.
11. **Running headers and footers.** At the top — the file name (shortened) and the charge level; at the bottom —
    the page number and the percentage read.
12. **Screen update.** A full frame update on page change
    (`epd_text` GL16), every 10 pages — `epd_quality` (GC16) to clean up
    artifacts; the cursor in the file manager — a regional update.

13. **Screen update is regional.** The frame is assembled from "dirty" rectangles
    (`Board::touch`), and only they go to the panel: cursor movement in the file manager — two bars
    540×54, status lines — their own bars. Page flipping is one GL16 pass
    over the content (without a black flash), every 10 pages — GC16 to clean up artifacts.
    If the regions cover >68% of the screen or 20 partial updates have accumulated, the driver
    does a full update.
14. **Interface fonts are JetBrains Mono.** All service UI (file manager, file names
    and sizes, status lines, headings, percentages, messages) is set
    in monospaced JetBrains Mono; PT Serif with serifs remains only for book text.
15. **Characters that the font does not contain.** Soft hyphen, ZWSP/ZWNJ/ZWJ and BOM are discarded
    before rasterization (otherwise hyphens looked like "?"). Missing letters are replaced by
    U+FFFD, missing symbols (€, ₽ — they are absent from PT Serif) are drawn as a narrow space.

16. **Minimum fills in the interface.** The selected row in the file manager is not
    inverted: two filled triangles are drawn on the left and right, centered on
    the optical middle of the row (the middle of the ascent..descent box). The file manager font is the smallest
    of the three sizes (24 px, JetBrains Mono) — more rows per screen and less ink.
    When the cursor moves, only the two narrow 22 px bars at the edges are redrawn
    (measured: ~200 ms), the row text is not touched.
17. **Waveform modes matched to the task.** On-device measurements (`rdr bench`): GC16/GL16 —
    ~1.3 s for a full screen, DU (`fast`) — 399 ms, DU4 (`fastest`) — 297 ms. Therefore
    flipping runs in DU, small regions in DU4, and once every 10 pages and with a full frame on
    opening/font change — GC16, for gray levels and ghost removal. The Fastwave-Eink-Driver code
    (SPI controllers SSD16xx) is not applicable to our parallel panel, but the principle "fewer
    frames in the LUT" was used.

18. **Protecting progress from desynchronization.** The progress record stores the file size
    at the moment of saving. If the file on the card has been replaced (a different size), the position is considered
    invalid and reading starts from the beginning — otherwise `token` would point into someone else's
    text and the book would open "from the middle of a sentence".
19. **"Open from the beginning".** A short A+B in the file manager continues from the saved place (per specification
    4.5.4); holding A+B ≥ 800 ms deletes the progress record for the selected file and
    opens it from the first page. A long A+B in reading mode is not used.

20. **Input is separated from drawing.** Buttons are polled by a task on core 1 (2 ms step,
    10 ms debounce) and the gestures are put into a queue; the UI task merges the queue before
    the frame. Fast presses are neither lost nor waiting for the E-Ink wave, and intermediate frames
    are not drawn: N presses = one screen update.
21. **Modifier combinations in the file manager.** Holding B + pressing A — to the start of the list;
    holding A + pressing B — page down (if there are fewer files than a page — cursor to
    the last one). The combination is recognized after the auto-repeat of the held button has started as well:
    the repeats have already scrolled the list, and the press makes the jump.

22. **Flipping after a font change.** The reading position is stored as a document point
    (paragraph token + offset in characters). On a font size change the page "snaps" to
    the nearest natural pagination boundary (one containing the previous position), otherwise
    the page would begin in the middle of a paragraph, and "back" would land on an overlapping
    page. The back history is rebuilt in windows from checkpoints (every 10th page,
    position + number), so flipping back works without a full pass over the book.
    The page number is not shown in the footer — it depends on the font size and would require
    a full pass; instead, the percentage.

23. **Hyphenation and widows/orphans.** Book text is hyphenated by school rules
    (`text/hyphenate.cpp`, ~150 lines, no tables): a single consonant between vowels goes
    into the next syllable, a consonant cluster is split before the last one, "й/ь/ъ/ы" do not start
    a line, at least two letters on each side. A soft hyphen (U+00AD) from FB2/EPUB
    counts as an explicit break point — many books are already marked up by the typesetter.
    Headings, lists and code are not hyphenated.
    Widows/orphans: a paragraph for which only one line of room is left on the page
    moves entirely to the next one; if after the break a paragraph had one last
    line left, it is moved to the previous one, so the page does not start with an orphan.
    Cost: 0.15 KB of code, hyphenation only for words that do not fit into a line.

24. **Page refresh mode — DU + regular "flush".** Pages are flipped in DU
    (~330–400 ms), and every 4th frame is forced to GC16 (~1.3 s) so that ghosts do not
    accumulate; manually — the `rdr clean` command. Glyphs are drawn in a **thresholded**
    (1-bit) way: only fully black pixels remain in the buffer, so a short DU pulse has nothing
    to "under-drive", and on a GC16 frame the picture looks the same —
    pages do not alternate "smooth/harsh". Measured: text 100% pure black and white (without
    halftones), page 330–400 ms, flush 1.3 s.
    If maximum letter smoothness is required — enable anti-aliasing again
    (`ui::set_crisp(false)`) and switch pages to GC16.

25. **The atlas is rasterized hinted (1-bit), not gray with a threshold.** Previously the glyphs
    were drawn in 8-bit gray and then thresholded to 1 bit: at 28 px size up to 13% of the stroke was lost
    (letters "got thinner"), at 25–26 px they got fatter instead, so some
    pages looked crisp and others blurry. Now `tools/fonts/gen_fonts.py`
    uses FreeType with `FT_LOAD_TARGET_MONO`: strokes are snapped to the pixel
    grid, the mask is 1-bit right away, the letter width is taken from the same hinted glyph
    (metrics and raster agree). Atlas measurement: all bitmaps are exactly {0, 255},
    and on screen the text is 100% pure black. Tools: `tools/fonts/gen_fonts.py`
    (generation), `tools/fonts/compare_rendering.py` (comparison of methods and sizes).

26. **The selected file manager row also switches the icon to the `fill` variant** (as in the mockup): the icon set
    contains `folder`/`folder_fill`, `file`/`file_fill`, `file_locked`/`file_locked_fill`.
    When the cursor moves, not only the cursor bars are redrawn but also two 24×24
    icon squares, so during fast flipping the frame always has exactly one fill icon.

27. **Header and footer — white background, no gray fill** (at the customer's request): the header
    keeps a black line at the bottom, the footer has no border. Partial updates of these
    bars use **GC16**, not DU/DU4: the text there is replaced, and a short pulse left
    the previous label showing through — which is why labels from different modes mixed.
    Measured: updating a 540×42 bar — 822 ms. Content (rows, icons) — DU, narrow
    cursor bars — DU4.

28. **Directories in the list are in square brackets** (`[Dasha]`), without slashes; the path in the header
    keeps the slashes.
29. **Preloader when opening a book.** E-ink cannot do smooth animation, so the screen
    shows three discrete steps: "reading file" 15% → "parsing" 55% → "layout" 85%,
    with a bar and a percentage. It is shown only for files larger than 300 KB (small books
    open instantly and do not need the extra frames). Each step is one DU frame (~330 ms).
30. **Self-healing of corrupted files.** If the parser rejected TXT/MD/FB2, the application
    re-encodes the file to UTF-8 (encoding detection by BOM/declaration/heuristics, with
    fallback to CP1251), cleans control characters, escapes single `&`, appends
    the closing tag and fixes the encoding declaration, then checks that the file opens.
    The copy is written next to it (`<file>.utf8`) and a dialog is shown: "A — YES (replace),
    B — NO". On replacement the original is saved as `<file>.bak`. Manual run: `rdr heal <path>`.
31. **The page number is not counted for all formats.** The background counter opens a second
    instance of the book; for TXT/MD/FB2 (the whole file in memory, CP1251 doubles in UTF-8)
    this ran into the heap and crashed the application — there the counter is disabled, and the footer keeps
    only the current page. For EPUB/PDF the counter works, and when it finishes the second
    instance is freed.

32. **The page number is saved together with the position.** In `progress.json` every book
    now has `page` (the page number) and `layout` (the layout signature: margins, width,
    height, font size). On opening:
    * the layout matched — the number is restored instantly, for **any** format;
    * the layout is different (font size changed) or the record is old (page = 0) — the number
      is recalculated by a pass from the beginning to the saved position (`walk_to_position`),
      which also aligns the page to the grid.
    This eliminated the bug "after a reboot the book opened in its place, but the counter
    showed 1" — especially noticeable on FB2, where the background counter is disabled.

33. **The selected file manager row is bold.** Besides the arrow on the left and the filled icon, the file
    name is drawn in a bold face (`mono_bold` 24 px): on e-ink this is the most noticeable
    and cheapest selection cue — the same grid, the same metrics, more ink.
    Because of this, when the cursor moves **both rows are redrawn entirely** (the old one
    returns to the regular face), not just the cursor bars: measured — 266 ms per
    frame against ~200 ms before.

34. **Unified typography and rhythm for "dialog" screens.** Error messages,
    questions (self-healing) and the loading screen are drawn by a single builder
    `App::draw_dialog()`: heading 32 px bold, text 28 px with word wrapping,
    hints 24 px. Block heights are computed from atlas metrics (`line_height`,
    `ascent`), the paddings are three constants (between blocks, before hints, card margins),
    the whole stack is centered vertically. No "magic" coordinates: when the atlas font size
    changes, the layout recalculates itself. The loading screen is built on the same rhythm
    (32/28/24 + a 14 px bar).

35. **File names during self-healing.** The repaired copy is first written as
    `<file>.repair.tmp` and checked by the parser, and only then gets its name:
    * "A — replace": the broken original is renamed to **`broken-<name>`**,
      and the repaired file takes the original name (the book keeps opening under its own name);
    * "B — keep": the original is not touched, and the copy gets **`fixed-<name>`**.
    On repeats the prefix is numbered (`broken-2-<name>`, `broken-3-<name>`), so old
    copies are not overwritten, and the name immediately shows which file produced what.
    The temporary `.repair.tmp` is deleted on refusal. For cleanup there is `rdr rm <path>`,
    and manual launch of healing is `rdr heal <path>`.

36. **The file manager has no bottom panel.** The list uses the full height
    (16 rows instead of 15), and `content_region()` and `visible_rows()` count the footer only
    on the reading screen. In the reading footer only the page number and the percentage remain,
    button hints have been removed; a thin reading progress bar is added on the right.

37. **Power-on indicator.** Immediately after power is applied, before the card is mounted,
    the screen "POWERING ON / preparing book" is drawn with the same 32/28 font sizes as
    the dialogs. So the screen does not stay empty while initialization runs.

38. **Auto-open of the last book.** `progress.json` stores the `reading` flag.
    It is set when a book is opened and cleared when returning to the manager (A+B) or on
    "open from the beginning". If the device was turned off from a book, on the next power-on it
    opens the same file at the saved position by itself. Opening is done not in `begin()`,
    but in the first tick of `run()`: otherwise the debug console managed to interfere with the
    half-initialized state (now commands that change state wait for
    loading to finish, and `dump` during an operation answers `rdr busy`).

39. **Progress is saved on every page flip.** Previously the JSON was written once every
    30 s (or every 10 pages), so turning the device off ate the last pages and
    the book opened "not at the right place". Now `turn_page()` (forward and back) writes
    progress immediately — the record is small, and against a 300–1300 ms screen update it is invisible.

40. **The power-on screen is closed.** `begin()` ends with a switch to the file
    manager and `mark_all()`: otherwise the "POWERING ON" frame remained on the panel and the device
    looked hung (this is exactly what happened when turning off from the manager). Early
    exits of `render()` now clear the busy flag through an RAII guard, so debug
    commands do not get an "eternally busy" application.

41. **Charge is shown immediately and honestly.** The sensor (M5PM1) is read at startup, not
    a minute later by a timer: previously `_battery_percent` stayed `-1` until the first poll
    (60 s), and the header, which now always draws the percentage, showed "0%". If the charge
    could not be read, "--" is displayed rather than zero. Diagnostics: `rdr bat` prints
    millivolts and percentage (measured: 4194 mV → 99%).

42. **The power button cannot be reassigned.** It is connected to the PMIC (M5PM1,
    I²C 0x6E), and the PMIC handles presses itself: **single** — board restart,
    **double** — power off. The firmware does not live until the press, so the binding
    "click = page" was removed (polling I²C every 2 ms only wasted the bus).
    Practical conclusion: restart by the button is safe — progress is written on every
    page flip, and after such a restart the book opens itself at the same place.
    Diagnostics remain: `rdr pek` (key state + mV), `rdr bat`.

43. **Footnotes: model and FB2 parser (first step).** `Paragraph` gained the list
    `Link { run, begin, end, note }` — the byte range of the link inside one run plus
    the footnote number (1-based, 0 = target not found). `Document` gained
    `note_count()`/`note(index)`. The FB2 parser makes two passes: the first collects
    `<body name="notes">` (`<section id="…">` → footnote text, numbered in order),
    the second — the regular text, where `<a l:href="#id">` becomes a `Link`, and the footnote body
    no longer gets into the book text; a number repeated in a footnote ("1 Dedicated to…")
    is trimmed. Check on a real book: **243 links, 243 footnotes, all resolve**;
    broken links (`#missing`) remain with `note = 0`, so the panel can say
    "footnote text not found". Test: `tests/test_links.cpp` (18 checks).

44. **Link mode (second step).** A long A+B in reading opens the mode: A/B move the
    focus over the links of the page (with auto-repeat), at the page boundary — page turn and transition
    to the first/last link of the new page, a short A+B exits. Focus is marked by a
    triangle in the left margin — the same language as the cursor in the manager. At the bottom
    a panel opens with the footnote text at 14 px size (the smallest in the atlas) and a counter
    `2/4`; a broken link shows "Footnote text not found". The layout provides, for each
    line, `LineView::links` (note, x, width) — the panel and the marker do not recompute the text.
    An important detail: the panel and the triangle are drawn after the page, so their regions
    are registered via `board().touch()`, otherwise the driver does not send them to the panel
    (symptom — the panel is "empty" in the panel buffer, although it is present in the frame). Test:
    `tests/test_links.cpp` (32 checks, including layout segments).

45. **Footnote panel: readability and scrolling.** The footnote text was moved from 14 px to
    **24 px** (the smallest reading size, serif) — 14 px was not readable on e-ink.
    Panel paddings are **24 px**, exactly half of the text margins (48). At the top of the panel
    a heading: the link label `[N]` plus the footnote's own title from `<title>` (numeric
    titles that duplicate the number are discarded), below it a separator line. If the text does not
    fit — a thin scrollbar on the right (4 px, level 6), as in reading. Scrolling inside
    the panel: **hold A + click B — down, hold B + click A — up** (the same
    combinations outside the panel change the font size, but in link mode they are intercepted). The body
    is reflowed once per focus change, and scrolling only selects the window.

46. **The panel scrollbar is only for long footnotes, and black.** Reflow of the footnote body
    is cached by footnote number (`_link_lines_note`): previously the cache was checked by
    `_link_shown_note`, which the code zeroed one line above, so the lines remained from
    the previous footnote — a short one inherited the length of a long one and got a spurious scrollbar.
    Now the bar is drawn only when `max_scroll > 0` (measured: a short footnote of 1 line —
    no bar, a long one of 18 lines — bar present) and is filled with pure black (`fillRect`), not
    with gray level 6.

47. **Footnote scrolling works: auto-repeat is disabled in link mode.** Previously
    holding A started auto-repeat (`ClickA` every 160 ms), the focus moved away, and the combination
    "hold A + click B" never reached scrolling. Now on entering link mode
    `set_gesture_repeat(false)` is called (outside the mode — `true`): holding works as a
    modifier, single clicks move the focus, hold A + click B scrolls the footnote down,
    hold B + click A — up. Panel metrics (reflow, number of visible lines, scrolling range)
    are computed immediately on focus change in `prepare_link_text()`, and not only in
    drawing — otherwise the first scroll after entering did not work. On-device measurement:
    down ×2 changes the text, up ×2 returns exactly the original 2081 px of ink.

48. **The panel shows the same label as the text and the same cursor as the file manager.** The internal
    footnote number counts all `<section>` of the footnotes body, including the author's notes
    (`avt_note_1`), so for the link `[1]` it equals 2 — the heading "[2]" looked like an
    error. Now `LinkView` carries the byte range of the link (`para`, `run`, `begin`,
    `end`), and the UI takes from it the **printed label** ("[1]") and shows it in the heading;
    the internal number remains only for accessing the text. Focus in link mode is
    marked by **the same `cursor` sprite as in the file manager** (24×24 in the left
    margin), instead of a triangle — a single selection language. The `test_links` test checks
    label extraction (36 checks).

49. **The page is shifted temporarily so that the link does not hide behind the panel.**
    We do not touch the paginator: `_page_shift` is an offset applied only during drawing (lines,
    cursor, rules). It is computed in `prepare_link_text()` for the current link: if its
    baseline is below the top of the panel, the page is raised by the required amount (with a margin
    of 12 px and a limit so that the top line does not go under the header). On exiting the mode
    `_page_shift = 0` — the page and the saved reading position remain as before, because
    the layout and the position never changed. The cursor is aligned to the center of the line ink:
    the metrics of the line's own font are taken (not of the base size) plus 2 px of optical
    correction for the label brackets — the measurement matched: cursor center 295.5 = label center 295.5.

50. **Screenshots read the panel in strips.** A whole frame (540×960×3 bytes ≈ 1.5 MB) did not
    fit into PSRAM when a large book with footnotes was open (`rdr err no-memory`).
    Now `rdr shot` reads and returns 48 rows at a time (≈78 KB), not a single buffer.

51. **The page shift is limited by the header.** The first version of the shift did not take the top
    boundary into account: a deep link pulled the text under the header, and the lines climbed
    onto it. Now the shift is limited so that the first line does not go above the header
    (`kHeaderHeight + 4`), and if because of that the link still hides behind the panel —
    **the panel is reduced** (to 140 px), rather than the header being painted over. Measured:
    at all focus positions the lines start at y=56–59 (the header ends at 42), the header text
    is not overlapped (header ink 1719–1771 px), the panel smoothly shrinks from 534 to 778 → 140 px in height.

52. **Why "sometimes the whole screen was redrawn" and what was fixed.** Three reasons:
    * in link mode every step did `mark_all()`, and the cleaning policy counted such
      frames as "book pages" — every 4th step went to full GC16 (a flash);
    * panel regions were registered twice (in the branch and inside `render_link_panel`), and
      `commit_frame` summed the areas **without merging**: 40% + 40% = 80% > the 68% threshold
      → the driver replaced the partial update with a full frame;
    * footnote scrolling did not need a page redraw at all, but caused one.
    Fixes: in link mode GC16 is no longer enabled (`!_link_mode`), regions
    are merged before coverage is computed (`Board::merge_regions()`), and scrolling of the
    footnote text marks only the panel bar (`kPanelPart`). Measured: scrolling became a partial
    update of `540×175` in **30 ms** instead of a full frame of ~320 ms. Focus change
    still redraws the content area entirely — that is legitimate, the page is
    shifted.

53. **Footnote panel height is driven by the content.** The panel is computed from the number of lines of
    the reflowed footnote: `heading + lines × line_height + padding`, but not less than 140 px
    and not more than the previous maximum (2/5 of the screen). A short footnote gets a short panel,
    a long one — the maximum.

54. **The "shadow" above the panel is sparse dots, not gray.** It turned out (verified on the
    device): in DU mode the panel does not reproduce halftones at all — `fill_level(12)`
    is output as white, so a gray bar of a "transparent shadow" cannot be made while
    the pages run in DU. Solution: a 24 px bar is drawn as a **sparse grid of black dots**
    (`ui::fill_dither`, period 5 px ≈ 4% of the area) **over** the text — the text under it
    shows through, and the bar reads as a light shadow. Measured: 6 dot rows of ~110 dots
    in the bar. The bar is drawn only with a valid focus and is reset on a page without
    links; when the panel height changes the frame is redrawn entirely, otherwise the old boundary
    remained on the screen.

55. **The shadow dots lie on a clean background, without a gradient.** The first version drew the grid
    over the letters — the text under the bar looked "bitten". Now the lines that fall into the
    shadow bar (and below it) are not drawn at all: the dots go on white, the bar reads as a
    soft separator, and the letters above it remain crisp. There is no gradient — one uniform
    grid with a period of 5 px. Check on a screenshot: row 530 contains exactly 108 dots
    (every 5 px), rows 528–532 apart from it are empty, the panel starts with a frame at 534,
    under the heading — a separator line at 594.

56. **The shadow is cancelled: the panel simply has a thick frame.** The idea of a "transparent shadow"
    did not take hold: DU has no halftones, dots over the text spoiled the letters, and the text under the dots
    had to be left undrawn. What remains is a **black 4 px frame** at the top of the panel (verified:
    rows 534–537 are fully black) and a separator under the heading. The shadow code (`fill_dither`,
    `level_color`) was removed from the renderer.

57. **Drawing optimization: what was done and where the limit is.**
    * the copying `LineView line = raw_line;` in the line loop was removed — it was a pair
      of `std::vector` per line per frame;
    * repeated threshold copying of the glyphs (`set_crisp`) was removed: the atlas is now
      rasterized with mono hinting and is already 1-bit, so the copy was pure work;
    * a link step that stays on **the same line**, and footnote scrolling, mark only
      the panel (`kPanelPart`) — a partial update instead of a full frame;
    * update regions are merged before coverage is computed (`Board::merge_regions`), which is why
      small edits no longer "inflated" to a full frame;
    * in link mode GC16 is not enabled, so the flashes between steps are gone;
    * `rdr shot` reads the panel in strips (does not require 1.5 MB of PSRAM).
    Measurements after the fixes: page 312–335 ms (full DU), footnote scrolling 267 ms
    (partial 540×384), moving to another line ~300 ms.
    **Limit:** ~90% of the time goes to the panel (the DU waveform itself is ≈ 300 ms per frame), and
    drawing takes 30–40 ms. Further drawing fixes yield percentages; the real
    levers are the choice of waveform and the update area:
    1) pages in DU4 (5 frames instead of 8) — ~200–230 ms, but thinner text;
    2) "cleanup on idle": instead of GC16 every 4th page, do it after 2 s
       of inactivity (pages stay fast, contrast is restored);
    3) with a small page shift in link mode, redraw only the bar of the
       changed lines, not the whole content.

58. **Note panel: half a screen for long notes, and the page scrolls instead of the
    panel shrinking.** The dynamic height now applies to short notes only: anything that
    needs more than half of the usable height gets exactly half (previously the panel could
    shrink to 140 px, which made long footnotes painful to read and to scroll). The focused
    reference is kept visible by shifting the page and clipping the lines that leave the
    band between the header and the panel, not by squeezing the panel. Measured: a long note
    gets a 459-px panel with the cursor at 399–422 px, a short one 172 px with the cursor at
    715–738 px, i.e. always above the panel border. Leaving the mode still restores the exact
    reading position, because the paginator is never touched.

59. **The pointer always lands on a real line.** The page shift was anchored on the
    focused line's baseline while the renderer clips lines by their box, so the focused
    line itself could be clipped and the cursor pointed at empty paper. The shift is now
    computed from the line box (`y + height`), the focused line is exempt from clipping, and
    an upper bound keeps it below the header. Measured over five consecutive references:
    every position has 2056–2323 px of ink on the cursor line, with the panel at 459 px for
    long notes and 172–316 px for short ones.

## v1 limitations

- Files up to 10 MB (PDF), up to 8 MB (text/EPUB), limited by free
  PSRAM; large files are opened in streaming mode where that is
  possible (EPUB/PDF — read on demand, TXT/FB2 — streaming).
- No Wi-Fi, OCR, DRM, text search, bookmarks (other than progress).
