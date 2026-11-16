"""Iterative traversal helpers for saved Figma JSON (trees can be deep)."""


def walk(root):
    """Yields nodes depth-first, without recursion."""
    stack = [root]
    while stack:
        node = stack.pop()
        yield node
        children = node.get("children")
        if children:
            stack.extend(reversed(children))


def bbox(node):
    box = node.get("absoluteBoundingBox") or {}
    return (box.get("x", 0.0), box.get("y", 0.0), box.get("width", 0.0), box.get("height", 0.0))


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
