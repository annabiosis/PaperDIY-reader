"""Summarises a saved Figma page: frames, their top-level parts, fonts and colors."""
import json
import sys
from collections import Counter


def walk(node):
    yield node
    for child in node.get("children", []):
        yield from walk(child)


def color_of(node):
    for fill in node.get("fills") or []:
        if fill.get("type") == "SOLID" and fill.get("visible", True):
            color = fill.get("color", {})
            r = round(color.get("r", 0) * 255)
            g = round(color.get("g", 0) * 255)
            b = round(color.get("b", 0) * 255)
            alpha = fill.get("opacity", 1.0) * color.get("a", 1.0)
            hexa = f"#{r:02X}{g:02X}{b:02X}"
            return hexa if alpha >= 0.999 else f"{hexa}@{alpha:.2f}"
    return ""


def box_of(node):
    b = node.get("absoluteBoundingBox") or {}
    return b.get("x", 0), b.get("y", 0), b.get("width", 0), b.get("height", 0)


def main():
    path = sys.argv[1]
    data = json.load(open(path, encoding="utf-8"))
    root = next(iter(data["nodes"].values()))["document"]
    print(f"page: {root.get('name')}")

    for frame in root.get("children", []):
        fx, fy, fw, fh = box_of(frame)
        print(f"\n=== {frame.get('type')} '{frame.get('name')}' {round(fw)}x{round(fh)} (id {frame.get('id')})")
        for child in frame.get("children", []):
            x, y, w, h = box_of(child)
            print(f"   {child.get('type',''):<10} {child.get('name','')[:30]:<30} "
                  f"x={round(x-fx):>4} y={round(y-fy):>4} {round(w):>4}x{round(h):<4} {color_of(child)}")

        fonts = Counter()
        texts = []
        for node in walk(frame):
            if node.get("type") != "TEXT":
                continue
            style = node.get("style") or {}
            size = round(style.get("fontSize", 0))
            family = style.get("fontFamily", "?")
            color = color_of(node)
            fonts[(family, size, color)] += 1
            texts.append(((node.get("characters") or "").strip(), size))
        if fonts:
            print("   fonts:")
            for (family, size, color), count in sorted(fonts.items(), key=lambda item: -item[0][1]):
                print(f"      {family} {size}px {color}  x{count}")
        if texts:
            unique = []
            for text, size in texts:
                if text and (text, size) not in unique:
                    unique.append((text, size))
            sample = ", ".join(f"'{t[:24]}'({s})" for t, s in unique[:6])
            print(f"   texts: {sample}")


if __name__ == "__main__":
    main()
