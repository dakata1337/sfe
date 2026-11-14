#!/usr/bin/env python3
"""Fails when the user guide's shortcut table is missing a shortcut.

    check_guide.py SOURCE_ROOT

Reads the default shortcuts from the action table (src/actions.c) and the
application shortcuts (src/app.c), and checks that each one appears in the
"Keyboard shortcuts" table of docs/guide/index.md. Adding a shortcut without
documenting it makes `meson test` fail.
"""

import os
import re
import sys

MODIFIERS = {"Control": "Ctrl", "Shift": "Shift", "Alt": "Alt", "Super": "Super"}
KEYS = {"backslash": "\\\\", "minus": "-", "Return": "Return", "Delete": "Delete"}


def display(accel):
    """GTK accelerator ("<Control><Shift>n") -> guide notation ("Ctrl+Shift+N")."""
    mods = re.findall(r"<(\w+)>", accel)
    key = re.sub(r"<\w+>", "", accel)
    key = KEYS.get(key, key.upper() if len(key) == 1 else key)
    return "+".join([MODIFIERS.get(m, m) for m in mods] + [key])


def accels_in(path, pattern):
    with open(path, encoding="utf-8") as f:
        text = f.read()
    found = []
    for name, accels in re.findall(pattern, text):
        for accel in re.findall(r'"([^"]+)"', accels):
            found.append((name, accel))
    return found


def guide_shortcuts(path):
    with open(path, encoding="utf-8") as f:
        text = f.read()
    table = text.split("## Keyboard shortcuts", 1)[1]
    shortcuts = set()
    for line in table.splitlines():
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) >= 2 and cells[0] and not cells[0].startswith("---"):
            shortcuts.update(s.strip() for s in cells[0].split(","))
    return shortcuts


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    # { "name", "label", { "<Control>x", NULL }, ...
    actions = accels_in(os.path.join(root, "src", "actions.c"),
                        r'\{ "([a-z-]+)",\s+"[^"]*",\s+\{ ([^}]*)\}')
    # { "app.quit", { "<Control>q", NULL } },
    app = accels_in(os.path.join(root, "src", "app.c"), r'\{ "(app\.[a-z-]+)", \{ ([^}]*)\}')
    if not actions:
        sys.exit("found no shortcuts in src/actions.c: has the table format changed?")

    documented = guide_shortcuts(os.path.join(root, "docs", "guide", "index.md"))
    missing = [(name, display(a)) for name, a in actions + app if display(a) not in documented]
    for name, shortcut in missing:
        print(f"docs/guide/index.md: shortcut {shortcut} ({name}) is not in the Keyboard shortcuts table")
    if missing:
        sys.exit(1)
    print(f"all {len(actions) + len(app)} shortcuts are documented")


if __name__ == "__main__":
    main()
