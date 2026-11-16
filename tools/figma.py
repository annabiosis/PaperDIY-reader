#!/usr/bin/env python3
"""Reads a Figma file and prints exact geometry for design review.

The e-reader panel is 540x960; this tool lists frames, and for a chosen frame the
elements (name, x/y/w/h, font size, text, fill) so they can be compared with the
firmware layout constants.

Auth: set FIGMA_TOKEN in the environment (Personal access token with the
`file_content:read` scope). The token is never written to disk.

Usage:
  python tools/figma.py frames "https://www.figma.com/design/<key>/<name>?node-id=1-2"
  python tools/figma.py dump   "<url>" [--frame "Читалка"] [--json raw.json]
  python tools/figma.py check  "<url>" [--frame "Читалка"]
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import urllib.error
import urllib.request

API = "https://api.figma.com/v1/files/"

# Panel and layout constants from the firmware (see docs/architecture.md).
PANEL = {"width": 540, "height": 960}
HEADER_H = 52
FOOTER_H = 44
MARGIN_L = 26
MARGIN_R = 26
FM_ROW_H = 45          # mono 24px + 8
FM_FONT_PX = 24
READER_FONTS_PX = (24, 29, 35)  # three body sizes in the atlas


def file_key(url_or_key: str) -> str:
    match = re.search(r"/(?:design|file)/([A-Za-z0-9]+)", url_or_key)
    if match:
        return match.group(1)
    return url_or_key.strip()


def node_id_from_url(url: str) -> str | None:
    match = re.search(r"node-id=([0-9]+-[0-9]+)", url)
    return match.group(0).split("=")[1].replace("-", ":") if match else None


def fetch(url_or_key: str, save: str | None = None) -> dict:
    token = os.environ.get("FIGMA_TOKEN", "").strip()
    if not token:
        raise SystemExit("FIGMA_TOKEN is not set (personal access token, scope file_content:read)")
    key = file_key(url_or_key)
    request = urllib.request.Request(API + key, headers={"X-Figma-Token": token})
    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            payload = json.load(response)
    except urllib.error.HTTPError as error:
        if error.code in (401, 403):
            raise SystemExit(f"Figma rejected the token ({error.code}): check the scope file_content:read")
        if error.code == 404:
            raise SystemExit("File not found: wrong link or the token has no access to it")
        raise SystemExit(f"Figma API error {error.code}: {error.reason}")
    except urllib.error.URLError as error:
        raise SystemExit(f"network error: {error.reason}")
    if save:
        with open(save, "w", encoding="utf-8") as handle:
            json.dump(payload, handle, ensure_ascii=False)
        print(f"raw json saved to {save}", file=sys.stderr)
    return payload


def walk(node: dict):
    yield node
    for child in node.get("children", []):
        yield from walk(child)


def bbox(node: dict) -> tuple[float, float, float, float]:
    box = node.get("absoluteBoundingBox") or {}
    return (box.get("x", 0.0), box.get("y", 0.0), box.get("width", 0.0), box.get("height", 0.0))


def color_of(node: dict) -> str:
    fills = node.get("fills") or []
    for fill in fills:
        if fill.get("type") == "SOLID" and fill.get("visible", True):
            color = fill.get("color", {})
            r = round(color.get("r", 0) * 255)
            g = round(color.get("g", 0) * 255)
            b = round(color.get("b", 0) * 255)
            alpha = fill.get("opacity", 1.0) * color.get("a", 1.0)
            if alpha >= 0.999:
                return f"#{r:02X}{g:02X}{b:02X}"
            return f"#{r:02X}{g:02X}{b:02X}@{alpha:.2f}"
    return ""


def font_of(node: dict) -> str:
    style = node.get("style") or {}
    size = style.get("fontSize")
    family = style.get("fontFamily")
    weight = style.get("fontWeight")
    if size is None and not family:
        return ""
    parts = [family or "?"]
    if weight:
        parts.append(str(weight))
    if size:
        parts.append(f"{size:g}px")
    return " ".join(parts)


def show_frames(payload: dict) -> None:
    document = payload.get("document", {})
    frames = [n for n in walk(document) if n.get("type") in ("FRAME", "CANVAS", "COMPONENT", "SECTION")]
    print(f"file: {payload.get('name', '?')}   last modified: {payload.get('lastModified', '?')}")
    print(f"{'type':<10} {'name':<34} {'size':>12}  id")
    for node in frames:
        x, y, w, h = bbox(node)
        if w <= 0 or h <= 0:
            continue
        marker = "  <-- panel" if (round(w) == PANEL["width"] and round(h) == PANEL["height"]) else ""
        print(f"{node.get('type', ''):<10} {node.get('name', '')[:34]:<34} {round(w):>5}x{round(h):<5} {node.get('id', '')}{marker}")


def find_frame(payload: dict, name: str | None, wanted_id: str | None) -> dict:
    nodes = list(walk(payload.get("document", {})))
    if wanted_id:
        for node in nodes:
            if node.get("id") == wanted_id:
                return node
    frames = [n for n in nodes if n.get("type") in ("FRAME", "COMPONENT") and bbox(n)[2] > 0]
    if name:
        for node in frames:
            if name.lower() in (node.get("name") or "").lower():
                return node
        raise SystemExit(f"frame not found by name: {name}")
    if not frames:
        raise SystemExit("no frames with geometry in this file")
    # Prefer a frame that matches the panel size, otherwise the largest one.
    for node in frames:
        x, y, w, h = bbox(node)
        if round(w) == PANEL["width"] and round(h) == PANEL["height"]:
            return node
    return max(frames, key=lambda n: bbox(n)[2] * bbox(n)[3])


def dump(payload: dict, frame: dict, wanted_id: str | None) -> None:
    fx, fy, fw, fh = bbox(frame)
    print(f"frame: {frame.get('name')}  {round(fw)}x{round(fh)}   id={frame.get('id')}")
    print(f"{'x':>5} {'y':>5} {'w':>5} {'h':>5}  {'type':<9} {'fill':<10} {'font':<26} name / text")
    rows = []
    for node in walk(frame):
        if node is frame:
            continue
        x, y, w, h = bbox(node)
        if w <= 0 or h <= 0:
            continue
        text = (node.get("characters") or "").replace("\n", " ⏎ ")
        rows.append((y - fy, x - fx, w, h, node, text))
    for y, x, w, h, node, text in sorted(rows):
        label = (node.get("name") or "")[:28]
        if text:
            label = f"{label}: {text[:40]}"
        print(f"{round(x):>5} {round(y):>5} {round(w):>5} {round(h):>5}  {node.get('type', ''):<9} "
              f"{color_of(node):<10} {font_of(node):<26} {label}")


def check(payload: dict, frame: dict) -> None:
    fx, fy, fw, fh = bbox(frame)
    print(f"frame: {frame.get('name')}  {round(fw)}x{round(fh)}")
    problems = []
    if round(fw) != PANEL["width"] or round(fh) != PANEL["height"]:
        problems.append(f"frame is {round(fw)}x{round(fh)}, the panel is {PANEL['width']}x{PANEL['height']}")

    fonts = set()
    rows = []
    for node in walk(frame):
        if node is frame:
            continue
        x, y, w, h = bbox(node)
        if w <= 0 or h <= 0:
            continue
        rows.append((round(y - fy), round(x - fx), round(w), round(h), node))
        size = (node.get("style") or {}).get("fontSize")
        if size:
            fonts.add(round(size))

    content_top = HEADER_H
    content_bottom = PANEL["height"] - FOOTER_H
    for y, x, w, h, node in rows:
        if x < 0 or x + w > PANEL["width"]:
            problems.append(f"'{node.get('name')}' sticks out horizontally: x={x} w={w}")
        if y < 0 or y + h > PANEL["height"]:
            problems.append(f"'{node.get('name')}' sticks out vertically: y={y} h={h}")
        if y < content_top and y + h > content_top:
            problems.append(f"'{node.get('name')}' crosses the header line at y={HEADER_H}")
        if y < content_bottom < y + h:
            problems.append(f"'{node.get('name')}' crosses the footer line at y={content_bottom}")

    usable = PANEL["width"] - MARGIN_L - MARGIN_R
    print(f"font sizes in the frame: {sorted(fonts)}")
    print(f"available text width with our margins: {usable} px (26..{PANEL['width']-MARGIN_R})")
    print(f"our file-manager row height: {FM_ROW_H} px, font {FM_FONT_PX} px -> "
          f"{(PANEL['height'] - HEADER_H - FOOTER_H - 8) // FM_ROW_H} rows per screen")
    print(f"our reader font sizes: {READER_FONTS_PX} px")
    if problems:
        print("\nissues:")
        for text in problems:
            print("  -", text)
    else:
        print("\nno geometry issues against the panel grid")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=("frames", "dump", "check"))
    parser.add_argument("url", help="Figma file URL or key (node-id in the URL selects a frame)")
    parser.add_argument("--frame", help="frame name to inspect (substring match)")
    parser.add_argument("--json", help="save the raw Figma JSON to this path")
    options = parser.parse_args()

    payload = fetch(options.url, options.json)
    if options.command == "frames":
        show_frames(payload)
        return 0

    wanted = node_id_from_url(options.url)
    frame = find_frame(payload, options.frame, wanted)
    if options.command == "dump":
        dump(payload, frame, wanted)
    else:
        check(payload, frame)
    return 0


if __name__ == "__main__":
    sys.exit(main())
