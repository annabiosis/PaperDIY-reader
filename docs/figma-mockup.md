# Figma mockup measurements (page `reader`, file Annabiosis)

Source: `https://www.figma.com/design/qggyZoHr3wyynb6d0hpK0q/Annabiosis?node-id=8143-339`
, obtained via the Figma API (`tools/figma.py`, `tools/figma_dump.py`, `tools/figma_report.py`).

## Frames

| Frame | Size | What is on it |
|---|---|---|
| Frame 1 | 560×940 | file manager |
| Frame 2 | 560×940 | reading, PT Serif 24 |
| Frame 3 | 560×940 | reading, PT Serif 28 |
| Frame 4 | 560×940 | reading, PT Serif 32 |

**Discrepancy:** the mockup is 560×940, the panel is 540×960. −20 px in width, +20 px in height.
A decision is needed: redraw the frames for 540×960 or accept a proportional transfer
(currently in the firmware the grid is transferred with 8/10 px margins and the same row geometry).

## Header (`Top`, 560×42; in the mockup the fill is `#DDDDDD`, in the firmware it is white)

| Element | Coordinates | Size | Font |
|---|---|---|---|
| background | 0,0 | 560×42 | — |
| SD icon | x=8, y=8 | 24×24 | — |
| path | x=40, y=11 | 448×18 | JetBrains Mono 18 |
| charge percentage | x=495, y=11 | 33×18 | JetBrains Mono 18 |
| battery icon | x=528, y=8 | 24×24 | — |

## File list (`Line`, height 48, step 56)

| Element | Coordinates | Size | Font/color |
|---|---|---|---|
| row | y=58, 114, 170… | 560×48 | white fill |
| selection lines | top and bottom of the row | 560×2 | `#000000` |
| icon | x=39, vertically centered | 24×24 | stroke set |
| name | x=74 | 470×32 | JetBrains Mono 24 |
| extension | x=461, y=+8 | 44×24 | JetBrains Mono 18 |
| size | x=461, y=+32 | 44×18 | JetBrains Mono 14 |
| cursor | x=0 | 24×24 | triangle (vector 21×22) |
| scrollbar | x=556 | 4×423 (in reading 4×94) | `#666666` |

## Footer (`Bottom`, 560×42; in the mockup `#DDDDDD`@87%, in the firmware white without a border)

File manager: `btn-AB` icon + `OPEN` text (18 px) on the right.
Reading: `5 / 350` (x=8) and `2%` (x=92) on the left, progress bar 242×10; on the right —
`btn-AB` + `EXIT`, `btn-A-B-hold` + `Aa+`, `btn-B-A-hold` + `Aa−`.

## Reading

- font: PT Serif 24 / 28 / 32 (three frames — three size steps);
- text: x=48, y=66, width 464 (margins of 48 px);
- in the header — the file name and the charge, in the footer — the page, the percentage, the hints.

## What has already been transferred to the firmware

- header and footer 42 px; **white background** (the gray fill was removed at the customer's request),
  the header has a black line at the bottom, the footer has no border;
- file manager rows: 48 px height, 8 px gap (step 56), icon 24×24 in the x=36 column,
  name 24 px starting at x=68, two-line meta 18/14 on the right;
- icons: 18 sprites from Figma (folder/file/file-locked/back/sdcard/battery 100–low/
  cursor/btn-AB, btn-AB-hold, btn-A-B-hold, btn-B-A-hold), 1-bit masks, ~0.3 KB each;
- UI sizes 14/18/24 added to the atlas as a separate ladder (reading stayed 24/29/35);
- the hints in the footer are drawn with the button icons from the mockup.

## Decisions (based on the customer's answers)

1. We build for the **real panel 540×960**; the mockup grid is transferred with 8/10 px margins.
2. There are no row separators.
3. Scrollbar 4 px — both in the file manager and in reading.
4. Reading margins — **48 px**.
5. Reading sizes — **24 / 28 / 32** (the atlas was rebuilt).
6. The page number is real, `N / M`, recalculated on a font size change (background counter).

Additionally from the mockup: rows of "locked" files (too large) are gray with a lock and
the mark `( ! )`; the cursor is an arrow on the left only; the footer has no border, the labels are uppercase
(OPEN / EXIT / AA+ / AA−); the file format is bold and uppercase.

## Open questions (historical)

1. 560×940 versus 540×960 — do we redraw the mockup or keep the proportional transfer?
2. Row separators 2 px top/bottom: keep them (that is +2 ink lines per row) or remove them?
3. Scrollbar 4 px on the right — add it in the file manager and in reading?
4. Reading margins 48 px (in the mockup) versus 26 px (now): do we increase them?
5. Reading font steps 24/28/32 (mockup) versus 24/29/35 (atlas) — rebuild the atlas?
6. Page number `5 / 350`: show the real number (requires a pass over the document)
   or keep only the percentage?
