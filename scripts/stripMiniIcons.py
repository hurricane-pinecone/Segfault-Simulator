"""Strips mini.icons of the glyphs the editor's Code panel can't draw.

Dear ImGui draws only the Basic Multilingual Plane (16-bit characters), and
most of mini.icons' glyphs are Material Design icons past U+FFFF. This drops
every icon entry using one, so mini.icons falls back to its default icon for
that category, and gives those defaults Nerd Font glyphs inside the plane.
The editor's init.lua then names its own icons for the file types that matter.

Run it on a freshly copied lua/mini/icons.lua when updating mini.nvim:

    python3 scripts/stripMiniIcons.py editor/nvim/pack/sfs/opt/mini.nvim/lua/mini/icons.lua
"""

import re
import sys

# Each category's default, inside the plane: a file, a folder, a symbol, an OS.
DEFAULTS = {
    "default": "",
    "directory": "",
    "extension": "",
    "file": "",
    "filetype": "",
    "lsp": "",
    "os": "",
}

ENTRY = re.compile(r"^\s*[\[\w'\".+-]+\]?\s*=\s*\{ glyph = ")
DEFAULT = re.compile(r"^(\s*)(\w+)(\s*= \{ glyph = ')([^']*)(')")


def outside_plane(line):
    return any(ord(c) > 0xFFFF for c in line)


def strip(text):
    kept = []
    in_defaults = False
    dropped = 0

    for line in text.split("\n"):
        if line.startswith("H.default_icons = {"):
            in_defaults = True
        elif in_defaults and line.startswith("}"):
            in_defaults = False

        if in_defaults and (match := DEFAULT.match(line)):
            line = DEFAULT.sub(
                lambda m: m.group(1) + m.group(2) + m.group(3) + DEFAULTS[m.group(2)] + m.group(5),
                line,
            )
        elif outside_plane(line) and ENTRY.match(line):
            dropped += 1
            continue

        kept.append(line)

    return "\n".join(kept), dropped


def main():
    path = sys.argv[1]

    with open(path, encoding="utf-8") as source:
        text, dropped = strip(source.read())

    left = [n for n, line in enumerate(text.split("\n"), 1)
            if outside_plane(line) and not line.lstrip().startswith("--")]

    if left:
        sys.exit(f"glyphs past U+FFFF left outside comments on lines {left[:10]}")

    with open(path, "w", encoding="utf-8") as target:
        target.write(text)

    print(f"dropped {dropped} icons past U+FFFF")


if __name__ == "__main__":
    main()
