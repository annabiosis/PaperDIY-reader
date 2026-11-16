"""Prints text styles (line height, spacing) and small details of a saved Figma page."""
import json
import sys


def walk(node):
    yield node
    for child in node.get("children", []):
        yield from walk(node)


def color_of(node):
    for fill in node.get("fills") or []:
        if fill.get("type") == "SOLID" and fill.get("visible", True):
            c = fill.get("color", {})
            return "#%02X%02X%02X" % (round(c.get("r", 0) * 255), round(c.get("g", 0) * 255),
                                      round(c.get("b", 0) * 255))
    return ""


def main():
    path = sys.argv[1]
    data = json.load(open(path, encoding="utf-8"))
    root = next(iter(data["nodes"].values()))["document"]

    print("--- text styles")
    seen = set()
    for node in walk(root):
        if node.get("type") != "TEXT":
            continue
        style = node.get("style") or {}
        size = round(style.get("fontSize", 0))
        family = style.get("fontFamily", "?")
        line = style.get("lineHeightPx")
        pct = style.get("lineHeightPercent")
        spacing = style.get("letterSpacing")
        key = (family, size, round(line or 0, 1), round(pct or 0, 1),
               round(spacing or 0, 2), style.get("textAlignHorizontal"))
        if key in seen:
            continue
        seen.add(key)
        sample = (node.get("characters") or "").strip().replace("\n", " ")[:28]
        print(f"{family:<20} {size:>3}px  lineHeight={line or '-'} ({pct or '-'}%)  "
              f"spacing={spacing or 0}  align={style.get('textAlignHorizontal')}  '{sample}'")

    print("\n--- text boxes (first 12)")
    count = 0
    for node in walk(root):
        if node.get("type") != "TEXT" or count >= 12:
            continue
        b = node.get("absoluteBoundingBox") or {}
        style = node.get("style") or {}
        print(f"x={round(b.get('x',0)):>5} y={round(b.get('y',0)):>5} "
              f"{round(b.get('width',0)):>5}x{round(b.get('height',0)):<5} "
              f"{style.get('fontSize',0):>3}px {color_of(node)} "
              f"'{(node.get('characters') or '').strip()[:30]}'")
        count += 1

    print("\n--- small frames / rectangles named Frame 9 or selection")
    for node in walk(root):
        name = node.get("name") or ""
        if name in ("Frame 9", "selection", "CURSOR", "Rectangle 7"):
            b = node.get("absoluteBoundingBox") or {}
            print(f"{node.get('type',''):<10} {name:<12} x={round(b.get('x',0))} y={round(b.get('y',0))} "
                  f"{round(b.get('width',0))}x{round(b.get('height',0))} fill={color_of(node)} "
                  f"visible={node.get('visible', True)}")


if __name__ == "__main__":
    main()
