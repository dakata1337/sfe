#!/bin/sh
# Retakes every screenshot of the user guide.
#
#   docs/guide/tools/screenshots.sh [BUILD_DIR]
#
# Runs ./build/sfe (or BUILD_DIR/sfe) against a generated demo home, drives
# it with keystrokes and saves PNGs into docs/guide/images/. sfe runs in a
# private mount namespace (unshare) where the demo home is /home/alex, so no
# real paths or user names end up in the pictures. Requirements: Hyprland
# >= 0.57 (Lua dispatcher), grim >= 1.5 (-T), unshare (unprivileged user
# namespaces), python3 with Pillow, and a US keyboard layout (text is typed
# key by key). The dark screenshots use the colours of your
# ~/.config/gtk-3.0/gtk.css.
#
# Nothing shows up on your desktop: the test windows (class "sfe-guide", so a
# window rule can tell them from your own sfe) open on the hidden
# special:magic workspace without taking focus, keys go straight to them,
# and each window is captured on its own (grim -T). They run without the
# session bus, so they never hand their window over to a running sfe, and
# your clipboard (the copy/paste shots use it) is restored afterwards when
# wl-clipboard is installed.
#
# Context menus are popups, which a window capture leaves out: the menu
# screenshots are kept as they are unless SFE_GUIDE_MENU=1, which shows
# special:magic on your screen for a moment to take them.
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
PUMP=
CLIP_TYPE=

mkdir -p "$OUT"

# Test windows: hidden, floating, never focused. (The rule only matches the
# "sfe-guide" class and lasts until Hyprland reloads its config.)
hyprctl eval 'hl.window_rule({ name = "sfe-guide-hidden", match = { class = "^sfe-guide$" },
    workspace = "special:magic silent", float = true, no_initial_focus = true })' >/dev/null
ln -s "$SFE" "$WORK/sfe-guide" # the program name becomes the window class

# Keep the clipboard to put it back at the end.
if command -v wl-paste >/dev/null && command -v wl-copy >/dev/null; then
    # Plain text when offered (what most pastes want), else the first type.
    types=$(wl-paste --list-types 2>/dev/null) || types=
    CLIP_TYPE=$(printf '%s\n' "$types" | grep -m1 -x 'text/plain;charset=utf-8' \
        || printf '%s\n' "$types" | grep -m1 -x 'text/plain' \
        || printf '%s\n' "$types" | head -n1)
    if [ -n "$CLIP_TYPE" ]; then
        wl-paste --no-newline --type "$CLIP_TYPE" >"$WORK/clipboard" 2>/dev/null || CLIP_TYPE=
    fi
fi
python3 "$ROOT/docs/guide/tools/demo-home.py" "$DEMO" >/dev/null
# Already asked whether sfe should be the default file manager (that dialog
# would take the keys of the first shots).
mkdir -p "$DEMO/.local/state/sfe"
: >"$DEMO/.local/state/sfe/asked-default-file-manager"
if [ -d "$HOME/.config/gtk-3.0" ]; then
    mkdir -p "$DEMO/.config/gtk-3.0"
    cp "$HOME"/.config/gtk-3.0/*.css "$DEMO/.config/gtk-3.0/" 2>/dev/null || true
fi

# ---------------------------------------------------------------------------
# Helpers

clients() { hyprctl clients -j; }

# A field ("address", "stableId") of PID's largest (main) or smallest
# (dialog) window.
field_of() { # PID largest|smallest FIELD
    clients | python3 -c '
import json, sys
cs = [c for c in json.load(sys.stdin) if c["pid"] == int(sys.argv[1])]
cs.sort(key=lambda c: c["size"][0] * c["size"][1], reverse=sys.argv[2] == "largest")
print(cs[0][sys.argv[3]] if cs else "")' "$1" "$2" "$3"
}

window_of() { field_of "$1" "$2" address; } # PID largest|smallest

which_of() { [ "${1:-main}" = dialog ] && echo smallest || echo largest; }

geometry_of() { # ADDRESS
    clients | python3 -c '
import json, sys
for c in json.load(sys.stdin):
    if c["address"] == sys.argv[1]:
        print("%d,%d %dx%d" % (c["at"][0], c["at"][1], c["size"][0], c["size"][1]))' "$1"
}

dispatch() { hyprctl dispatch "$1" >/dev/null; }

key() { # MODS KEY [WINDOW=main|dialog]
    target=$(window_of "$P" "$(which_of "${3:-main}")")
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

# A hidden window only draws when something asks for a frame, so its
# animations (the progress bar sliding in, ...) would stand still. Asking
# for one about ten times a second keeps it drawing as if it were visible.
start_pump() {
    (
        while kill -0 "$P" 2>/dev/null; do
            for id in $(clients | python3 -c '
import json, sys
print(" ".join(c["stableId"] for c in json.load(sys.stdin) if c["pid"] == int(sys.argv[1])))' "$P"); do
                grim -t ppm -T "$id" - >/dev/null 2>&1 || true
            done
            sleep 0.05
        done
    ) 2>/dev/null & # grim may crash when a window closes under it: harmless
    PUMP=$!
}

# Stops the sfe this script started (never any other running sfe).
stop_sfe() {
    if [ -n "$PUMP" ]; then
        kill "$PUMP" 2>/dev/null || true
        PUMP=
    fi
    if [ -n "$P" ]; then
        kill "$P" 2>/dev/null || true
        sleep 0.3
    fi
}

# Goes UPS folders up, then down PATH through the search (it walks into
# folders at each "/"; Return opens the match). Not the path bar: its inline
# completion needs real keyboard focus, which a hidden window never has.
goto() { # UPS PATH
    for _ in $(seq "$1"); do
        key ALT Up
        sleep 0.4
    done
    key CTRL f
    type_text "$2"
    sleep 0.4
    key "" Return
    sleep 0.8
}

# Starts sfe in DIR, with the demo home mounted at $FAKE. The real home is
# mounted back in place so fonts and desktop settings still apply.
launch() { # DIR [XDG_CONFIG_HOME]
    stop_sfe
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
        exec env HOME="$FAKE" XDG_CONFIG_HOME="$CONFIG" XDG_DATA_HOME="$FAKE/.local/share" \
            XDG_STATE_HOME="$FAKE/.local/state" \
            DBUS_SESSION_BUS_ADDRESS=disabled: "$WORK/sfe-guide" "$FAKE/$DIR"
    ' >/dev/null 2>&1 &
    P=$! # unshare execs sh, which execs sfe: the same process
    for _ in $(seq 50); do
        [ -n "$(window_of "$P" largest)" ] && break
        sleep 0.1
    done
    sleep 1.5
    main=$(window_of "$P" largest)
    dispatch "hl.dsp.window.resize({ x = $W, y = $H, exact = true, window = \"address:$main\" })"
    start_pump
    sleep 0.8
}

# Captures one window, hidden or not (the pump keeps its frames current).
capture() { # main|dialog FILE
    id=$(field_of "$P" "$(which_of "$1")" stableId)
    sleep 0.3
    grim -T "$id" "$2"
}

shot() { # NAME [main|dialog]
    capture "${2:-main}" "$OUT/$1.png"
}

# A region of the main window.
shot_region() { # NAME X Y WIDTH HEIGHT
    capture main "$WORK/region.png"
    python3 -c '
import sys
from PIL import Image
x, y, w, h = map(int, sys.argv[3:7])
Image.open(sys.argv[1]).crop((x, y, x + w, y + h)).save(sys.argv[2])' "$WORK/region.png" "$OUT/$1.png" "$2" "$3" "$4" "$5"
}

# A region of the main window with its popup menu open (SFE_GUIDE_MENU=1):
# special:magic is shown on screen for the capture, then hidden again.
shot_menu() { # NAME X Y WIDTH HEIGHT
    if [ "${SFE_GUIDE_MENU:-0}" != 1 ]; then
        echo "keeping $1.png (popup menus need SFE_GUIDE_MENU=1)"
        return
    fi
    dispatch 'hl.dsp.workspace.toggle_special("magic")'
    sleep 0.8
    geo=$(geometry_of "$(window_of "$P" largest)")
    x=${geo%%,*}
    rest=${geo#*,}
    y=${rest%% *}
    grim -g "$((x + $2)),$((y + $3)) ${4}x${5}" "$OUT/$1.png"
    dispatch 'hl.dsp.workspace.toggle_special("magic")'
}

# Puts back what was in the clipboard and removes the work files.
finish() {
    stop_sfe
    if [ -n "$CLIP_TYPE" ]; then
        # (wl-copy stays in the background serving it: keep it off our output)
        wl-copy --type "$CLIP_TYPE" <"$WORK/clipboard" >/dev/null 2>&1 || true
    fi
    rm -rf "$WORK"
}
trap finish EXIT

# ---------------------------------------------------------------------------
# The shots

launch Pictures
shot overview
shot_region sidebar 0 0 222 $H

key CTRL 2
goto 1 Documents
shot list-view
key CTRL 1

launch Projects
key CTRL backslash
sleep 0.6
goto 0 website
key CTRL minus
sleep 0.6
goto 2 Pictures/Holiday
shot splits

# The command line completing a destination: the other view's folder first.
launch Projects/website
key CTRL backslash
sleep 0.6
goto 2 Documents
key SHIFT h
key SHIFT semicolon
type_text "copy index.html "
sleep 0.8
shot command-line
key "" Escape

launch Downloads
key "" Home
key "" Right
key "" Menu
sleep 0.6
shot_menu menu-files 220 0 $((W - 220)) $H
key "" Escape
key CTRL a
sleep 0.2
key "" Escape   # nothing selected: the folder menu
key "" Menu
sleep 0.6
shot_menu menu-folder 220 0 $((W - 220)) $H
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
goto 1 Documents
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
goto 1 Videos
key CTRL v
sleep 1.2
shot progress
sleep 6

# The same window with a plain GTK theme (no gtk.css): colours follow the theme.
mkdir -p "$WORK/light/sfe"
cp "$DEMO/.config/sfe/config.toml" "$WORK/light/sfe/"
launch Pictures "$WORK/light"
shot theme-light

stop_sfe

# Smaller files: the screenshots are flat UI, so a palette loses nothing visible.
python3 - "$OUT" <<'EOF'
import os, sys
from PIL import Image
for name in sorted(os.listdir(sys.argv[1])):
    path = os.path.join(sys.argv[1], name)
    if name.endswith(".png"):  # kept ones are already reduced: unchanged
        Image.open(path).convert("RGB").quantize(256, dither=Image.Dither.NONE).save(path, optimize=True)
EOF
echo "screenshots written to $OUT"
