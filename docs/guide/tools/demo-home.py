#!/usr/bin/env python3
"""Creates the demo home folder used for the guide's screenshots.

    demo-home.py DIR

DIR gets a small, realistic home (Documents, Pictures, Projects, ...) plus a
config (DIR/.config/sfe/config.toml) with a few bookmarks. Screenshots are
taken with HOME=DIR so they never show anyone's real files. Needs Pillow for
the sample images.
"""

import os
import shutil
import sys

from PIL import Image, ImageDraw


def write(path, text=""):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)


def gradient(path, size, top, bottom):
    """A sky gradient with a sun and two hills, so thumbnails look like photos."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    w, h = size
    img = Image.new("RGB", size)
    draw = ImageDraw.Draw(img)
    for y in range(h):
        t = y / max(1, h - 1)
        color = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3))
        draw.line([(0, y), (w, y)], fill=color)
    sun = (min(255, top[0] + 90), min(255, top[1] + 80), min(255, top[2] + 60))
    r = h // 7
    draw.ellipse([w * 2 // 3 - r, h // 4 - r, w * 2 // 3 + r, h // 4 + r], fill=sun)
    far = tuple(c // 2 for c in bottom)
    near = tuple(c // 3 for c in bottom)
    draw.polygon([(0, h * 3 // 5), (w // 3, h * 2 // 5), (w * 3 // 4, h * 3 // 5), (w, h // 2), (w, h), (0, h)], fill=far)
    draw.polygon([(0, h * 4 // 5), (w // 2, h * 3 // 5), (w, h * 4 // 5), (w, h), (0, h)], fill=near)
    img.save(path)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    home = os.path.abspath(sys.argv[1])
    if os.path.exists(home):
        shutil.rmtree(home)

    d = lambda *p: os.path.join(home, *p)

    # Documents
    write(d("Documents", "notes.md"), "# Notes\n\n- buy milk\n")
    write(d("Documents", "report-2026.pdf"), "%PDF-1.4\n")
    write(d("Documents", "budget.ods"))
    write(d("Documents", "letter.odt"))
    write(d("Documents", "todo.txt"), "1. write the guide\n")
    write(d("Documents", "Work", "meeting-notes.md"), "# Meeting\n")
    write(d("Documents", "Work", "slides.odp"))
    write(d("Documents", "Recipes", "pancakes.md"), "# Pancakes\n")

    # Pictures (real images)
    palette = [
        ((255, 140, 66), (186, 45, 88)),
        ((72, 149, 239), (34, 51, 120)),
        ((90, 200, 140), (20, 90, 80)),
        ((250, 210, 90), (230, 110, 50)),
        ((170, 120, 230), (60, 40, 120)),
        ((120, 210, 230), (40, 110, 160)),
    ]
    for i, (a, b) in enumerate(palette, 1):
        gradient(d("Pictures", "Holiday", f"IMG_{4200 + i}.jpg"), (640, 480), a, b)
    gradient(d("Pictures", "wallpaper.png"), (960, 540), (30, 40, 70), (120, 80, 160))
    gradient(d("Pictures", "avatar.png"), (256, 256), (240, 180, 120), (200, 90, 90))
    gradient(d("Pictures", "screenshot.png"), (800, 450), (220, 220, 230), (150, 160, 180))

    # Music, Videos, Downloads
    for name in ("01 - Intro.mp3", "02 - Morning.mp3", "03 - Highway.flac"):
        write(d("Music", "Album", name))
    write(d("Videos", "birthday.mp4"))
    write(d("Videos", "tutorial.mkv"))
    write(d("Downloads", "archive.tar.gz"))
    write(d("Downloads", "installer.sh"), "#!/bin/sh\necho hi\n")
    write(d("Downloads", "manual.pdf"), "%PDF-1.4\n")
    write(d("Downloads", "font.ttf"))

    # Projects
    write(d("Projects", "website", "index.html"), "<!doctype html>\n")
    write(d("Projects", "website", "style.css"), "body {}\n")
    write(d("Projects", "website", "app.js"), "console.log('hi')\n")
    write(d("Projects", "website", "README.md"), "# Website\n")
    write(d("Projects", "website", "assets", "logo.svg"),
          '<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16"/>\n')
    write(d("Projects", "tool", "main.c"), "int main(void) { return 0; }\n")
    write(d("Projects", "tool", "main.h"), "#pragma once\n")
    write(d("Projects", "tool", "meson.build"), "project('tool', 'c')\n")
    write(d("Projects", "tool", "Makefile"), "all:\n")
    write(d("Projects", "tool", "README.md"), "# Tool\n")

    # A few files for rename demos
    for i in range(1, 6):
        gradient(d("Pictures", "Camera", f"DSC_{i:04d}.JPG"), (320, 240),
                 palette[i % 6][0], palette[i % 6][1])

    # Hidden files (Ctrl+H)
    write(d(".bashrc"), "# bash\n")
    write(d(".profile"), "# profile\n")

    # Config with bookmarks and one custom command
    write(d(".config", "sfe", "config.toml"), """[general]
view = "grid"

[sidebar]
bookmarks = [
    "~/Projects",
    "~/Documents/Work",
]

[[actions.directories]]
name = "Open terminal here"
command = "foot -D %f"

[[actions.files]]
name = "Open terminal here"
command = "foot -D %d"
""")
    print(home)


if __name__ == "__main__":
    main()
