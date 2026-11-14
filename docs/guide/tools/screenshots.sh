#!/bin/sh
# Retakes every screenshot of the user guide.
#
#   docs/guide/tools/screenshots.sh [BUILD_DIR]
#
# Runs ./build/sfe (or BUILD_DIR/sfe) against a generated demo home, drives
# it with keystrokes and saves PNGs into docs/guide/images/. sfe runs in a
# private mount namespace (unshare) where the demo home is /home/alex, so no
# real paths or user names end up in the pictures. Requirements: Hyprland
# >= 0.57 (Lua dispatcher), grim, unshare (unprivileged user namespaces),
# python3 with Pillow, and a US keyboard layout (text is typed key by key).
# The dark screenshots use the colours of your ~/.config/gtk-3.0/gtk.css.
set -eu

ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
SFE=${1:-$ROOT/build}/sfe
OUT=$ROOT/docs/guide/images
WORK=$(mktemp -d "${TMPDIR:-/tmp}/sfe-guide-XXXXXX")
DEMO=$WORK/home          # where the demo home really is
FAKE=/home/alex          # where sfe sees it
W=1200
H=960
P=

mkdir -p "$OUT"
python3 "$ROOT/docs/guide/tools/demo-home.py" "$DEMO" >/dev/null
if [ -d "$HOME/.config/gtk-3.0" ]; then
    mkdir -p "$DEMO/.config/gtk-3.0"
    cp "$HOME"/.config/gtk-3.0/*.css "$DEMO/.config/gtk-3.0/" 2>/dev/null || true
fi

# ---------------------------------------------------------------------------
# Helpers

clients() { hyprctl clients -j; }

# Address of PID's largest (main) or smallest (dialog) window.
window_of() { # PID largest|smallest
    clients | python3 -c '
import json, sys
cs = [c for c in json.load(sys.stdin) if c["pid"] == int(sys.argv[1])]
cs.sort(key=lambda c: c["size"][0] * c["size"][1], reverse=sys.argv[2] == "largest")
print(cs[0]["address"] if cs else "")' "$1" "$2"
}

geometry_of() { # ADDRESS
    clients | python3 -c '
import json, sys
for c in json.load(sys.stdin):
    if c["address"] == sys.argv[1]:
        print("%d,%d %dx%d" % (c["at"][0], c["at"][1], c["size"][0], c["size"][1]))' "$1"
}

dispatch() { hyprctl dispatch "$1" >/dev/null; }

key() { # MODS KEY [WINDOW=main|dialog]
    target=$(window_of "$P" "$([ "${3:-main}" = dialog ] && echo smallest || echo largest)")
    dispatch "hl.dsp.send_shortcut({ mods = \"$1\", key = \"$2\", window = \"address:$target\" })"
    sleep 0.15
}

# Types text key by key (US layout).
type_text() { # TEXT [WINDOW]
    printf '%s' "$1" | python3 -c '
import sys
names = {"/": ("", "slash"), "-": ("", "minus"), "_": ("SHIFT", "minus"), ".": ("", "period"),
         " ": ("", "space"), "~": ("SHIFT", "grave"), "{": ("SHIFT", "bracketleft"),
         "}": ("SHIFT", "bracketright"), ":": ("SHIFT", "semicolon")}
for ch in sys.stdin.read():
    if ch in names:
        mods, key = names[ch]
    elif ch.isupper():
        mods, key = "SHIFT", ch.lower()
    else:
        mods, key = "", ch
    print(mods + "|" + key)' | while IFS='|' read -r mods k; do
        key "$mods" "$k" "${2:-main}"
    done
}

goto() { # PATH (inside the demo home)
    key CTRL l
    key CTRL a
    type_text "$FAKE/$1"
    key "" Return
    sleep 0.8
}

# Starts sfe in DIR, with the demo home mounted at $FAKE. The real home is
# mounted back in place so fonts and desktop settings still apply.
launch() { # DIR [XDG_CONFIG_HOME]
    pkill -x sfe 2>/dev/null || true
    sleep 0.3
    mkdir -p "$WORK/realhome"
    # Inside the namespace the user is uid 0; naming uid 0 "alex" makes the
    # properties dialog show "alex" as the owner instead of "root".
    sed 's/^root:[^:]*:0:0:[^:]*:[^:]*:/alex:x:0:0:Alex:\/home\/alex:/' /etc/passwd >"$WORK/passwd"
    sed 's/^root:/alex:/' /etc/group >"$WORK/group"
    REAL_HOME=$HOME WORK=$WORK FAKE=$FAKE DEMO=$DEMO SFE=$SFE DIR=$1 CONFIG=${2:-$FAKE/.config} \
        unshare -rm sh -c '
        mount --bind "$WORK/passwd" /etc/passwd
        mount --bind "$WORK/group" /etc/group
        mount --bind "$REAL_HOME" "$WORK/realhome"
        mount -t tmpfs none /home
        mkdir -p "$REAL_HOME" "$FAKE"
        mount --bind "$WORK/realhome" "$REAL_HOME"
        mount --bind "$DEMO" "$FAKE"
        exec env HOME="$FAKE" XDG_CONFIG_HOME="$CONFIG" XDG_DATA_HOME="$FAKE/.local/share" "$SFE" "$FAKE/$DIR"
    ' >/dev/null 2>&1 &
    sleep 2.5
    P=$(pgrep -nx sfe)
    main=$(window_of "$P" largest)
    if clients | grep -q '"fullscreen": [12]'; then
        dispatch "hl.dsp.window.fullscreen({ window = \"address:$main\" })"
    fi
    dispatch "hl.dsp.window.float({ action = \"enable\", window = \"address:$main\" })"
    dispatch "hl.dsp.window.resize({ x = $W, y = $H, exact = true, window = \"address:$main\" })"
    dispatch "hl.dsp.window.center({ window = \"address:$main\" })"
    sleep 0.8
}

shot() { # NAME [main|dialog]
    target=$(window_of "$P" "$([ "${2:-main}" = dialog ] && echo smallest || echo largest)")
    grim -g "$(geometry_of "$target")" "$OUT/$1.png"
}

# Screenshot of a region of the screen relative to the main window.
shot_region() { # NAME X Y WIDTH HEIGHT
    geo=$(geometry_of "$(window_of "$P" largest)")
    x=${geo%%,*}
    rest=${geo#*,}
    y=${rest%% *}
    grim -g "$((x + $2)),$((y + $3)) ${4}x${5}" "$OUT/$1.png"
}

# ---------------------------------------------------------------------------
# The shots

launch Pictures
shot overview
shot_region sidebar 0 0 222 $H

key CTRL 2
goto Documents
shot list-view
key CTRL 1

launch Projects
key CTRL backslash
sleep 0.6
goto Projects/website
key CTRL minus
sleep 0.6
goto Pictures/Holiday
shot splits

launch Downloads
key "" Home
key "" Right
key "" Menu
sleep 0.6
shot_region menu-files 220 0 $((W - 220)) $H
key "" Escape
key CTRL a
sleep 0.2
key "" Escape   # nothing selected: the folder menu
key "" Menu
sleep 0.6
shot_region menu-folder 220 0 $((W - 220)) $H
key "" Escape

launch Documents
key CTRL f
type_text "no"
sleep 0.6
shot search
key "" Escape

# "Already exists": copy Downloads/manual.pdf into a folder that has one.
cp "$DEMO/Downloads/manual.pdf" "$DEMO/Documents/manual.pdf"
launch Downloads
key CTRL f
type_text "manual"
key "" Escape
key CTRL c
goto Documents
key CTRL v
sleep 1
shot conflict dialog
key "" Escape dialog
rm "$DEMO/Documents/manual.pdf"

launch Documents
key CTRL f
type_text "todo"
key "" Escape
key "" F2
sleep 0.6
shot rename-one dialog
key "" Escape dialog

launch Pictures/Camera
key CTRL a
key "" F2
sleep 0.8
key SHIFT Tab dialog
key CTRL a dialog
type_text "Holiday_{n:02}{ext}" dialog
sleep 0.5
shot rename-pattern dialog
key "" Escape dialog

launch Pictures
key CTRL f
type_text "holiday"
key "" Escape
key ALT Return
sleep 1.5
shot properties dialog
key "" Escape dialog

# Compress… from the context menu (Open, Open With, Cut, Copy, Rename, Compress).
launch Pictures
key CTRL f
type_text "wallpaper"
key "" Escape
key SHIFT F10
sleep 0.5
for _ in 1 2 3 4 5 6; do key "" Down; done
key "" Return
sleep 0.8
shot compress dialog
key "" Escape dialog

launch Pictures
key CTRL p
sleep 1
shot preferences dialog
key "" Escape dialog

# A long copy to show the progress bar.
mkdir -p "$DEMO/Backup/photos"
python3 -c "
import os
for i in range(25000):
    open(os.path.join('$DEMO/Backup/photos', 'p%05d.jpg' % i), 'w').write('x' * 200)"
launch Backup
key "" Home
key CTRL c
goto Videos
key CTRL v
sleep 1.2
shot progress
sleep 6

# The same window with a plain GTK theme (no gtk.css): colours follow the theme.
mkdir -p "$WORK/light/sfe"
cp "$DEMO/.config/sfe/config.toml" "$WORK/light/sfe/"
launch Pictures "$WORK/light"
shot theme-light

pkill -x sfe || true

# Smaller files: the screenshots are flat UI, so a palette loses nothing visible.
python3 - "$OUT" <<'EOF'
import os, sys
from PIL import Image
for name in sorted(os.listdir(sys.argv[1])):
    if name.endswith(".png"):
        path = os.path.join(sys.argv[1], name)
        Image.open(path).convert("RGB").quantize(256, dither=Image.Dither.NONE).save(path, optimize=True)
EOF
rm -rf "$WORK"
echo "screenshots written to $OUT"
