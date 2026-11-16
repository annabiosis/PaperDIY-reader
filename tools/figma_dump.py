"""Dumps a Figma node subtree (saved via /v1/files/:key/nodes) as a geometry table."""
import json
import sys


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


def font_of(node):
    style = node.get("style") or {}
    size = style.get("fontSize")
    family = style.get("fontFamily")
    if not size and not family:
        return ""
    weight = style.get("fontWeight")
    parts = [family or "?"]
    if weight:
        parts.append(str(weight))
    if size:
        parts.append(f"{size:g}")
    return " ".join(parts)


def walk(node):
    yield node
    for child in node.get("children", []):
        yield from walk(child)


def find_node(root, node_id):
    for node in walk(root):
        if node.get("id") == node_id:
            return node
    return None


def main():
    path = sys.argv[1]
    node_id = sys.argv[2]
    only_text = "--text" in sys.argv
    data = json.load(open(path, encoding="utf-8"))
    root = next(iter(data["nodes"].values()))["document"]
    document = find_node(root, node_id)
    if document is None:
        raise SystemExit(f"node {node_id} not found in {path}")
    box = document.get("absoluteBoundingBox") or {}
    ox, oy = box.get("x", 0), box.get("y", 0)
    print(f"{document.get('name')}  {round(box.get('width',0))}x{round(box.get('height',0))}")
    print(f"{'x':>5} {'y':>5} {'w':>5} {'h':>5}  {'type':<10} {'fill':<14} {'font':<30} name / text")
    rows = []
    for node in walk(document):
        if node is document:
            continue
        b = node.get("absoluteBoundingBox") or {}
        w, h = b.get("width", 0), b.get("height", 0)
        if w <= 0 or h <= 0:
            continue
        if only_text and node.get("type") != "TEXT":
            continue
        text = (node.get("characters") or "").replace("\n", " / ")
        rows.append((b.get("y", 0) - oy, b.get("x", 0) - ox, w, h, node, text))
    rows.sort(key=lambda item: (item[0], item[1]))
    for y, x, w, h, node, text in rows:
        label = (node.get("name") or "")[:26]
        if text:
            label = f"{node.get('type')}: {text[:46]}"
        print(f"{round(x):>5} {round(y):>5} {round(w):>5} {round(h):>5}  {node.get('type',''):<10} "
              f"{color_of(node):<14} {font_of(node):<30} {label}")


if __name__ == "__main__":
    main()
