# sfe as the system file dialog

sfe can answer other programs' Open / Save dialogs. Turn it on in
*Settings → System → Use sfe for Open / Save dialogs*; this page explains
what that does and how to set it up by hand.

## How file dialogs reach a program like sfe

Many programs don't draw their own Open / Save dialogs; they ask
**xdg-desktop-portal**. Firefox and Zen do this when
`widget.use-xdg-desktop-portal.file-picker` is `1` in `about:config`, as do
Chromium and Electron apps (Claude Desktop, VS Code, …) when they can reach
the session bus (see [Troubleshooting](#troubleshooting)), every Flatpak
app and GTK apps run with `GTK_USE_PORTAL=1`. The portal
forwards the request to whichever **backend** is configured for
`org.freedesktop.impl.portal.FileChooser`. On a stock Hyprland setup that is
`xdg-desktop-portal-gtk` (`/usr/share/xdg-desktop-portal/hyprland-portals.conf`
says `default=hyprland;gtk`, and the Hyprland portal has no file chooser).

sfe adds two pieces:

- **`sfe --choose open|save|folder`**: a separate sfe window with a bar at
  the bottom (a name field for saving, the file type filter, the program's
  extra options, *Cancel*, *Open* / *Save* / *Select*) and, when opening
  files, a preview of the selected file on the right. A folder dialog
  lists only folders. Enter or a double-click on a file chooses it;
  Ctrl+Enter is the *Open* / *Save* button. It prints the chosen files as
  URIs and exits with 0, or exits with 1 when cancelled. Try it on its own:

  ```sh
  sfe --choose open --multiple --folder ~/Downloads
  sfe --choose save --name report.pdf
  sfe --choose open --filters "[('Images', [(1, 'image/*')]), ('Text', [(0, '*.txt')])]"
  ```

  `--filters` takes the portal's filter list in GVariant text form
  (`a(sa(us))`: a name and rules, 0 = glob, 1 = MIME type), `--current-filter N`
  picks the one to start with, and `--choices` the extra options
  (`a(ssa(ss)s)`: id, label, options, default; no options makes a check box,
  `true` / `false`). The answer then starts with `#filter N` and one
  `#choice ID VALUE` line per option before the URIs.

- **`sfe-portal`** (installed to `/usr/lib/sfe-portal`, or the libexec
  folder of your prefix): a portal backend. For each Open / Save request it
  runs `sfe --choose` with the request's title, start folder, suggested
  name, button label, filters, extra options and parent window, and returns
  what was chosen, the filter in use and the options' values.

## Setting it up

1. Install sfe (`sudo meson install -C build`, or the package), so
   `sfe-portal`, its `.portal` file and its D-Bus service file are in place.

2. *Settings → System → Use sfe for Open / Save dialogs*. This writes one
   line to `~/.config/xdg-desktop-portal/<desktop>-portals.conf`
   (`hyprland-portals.conf` on Hyprland), keeping what else is there; a new
   file starts from the settings in effect before:

   ```ini
   [preferred]
   default=hyprland;gtk
   org.freedesktop.impl.portal.FileChooser=sfe
   ```

   and restarts the portal (and a running `sfe-portal`) so the next dialog
   uses it. Switching it off takes the line out again. By hand: edit the
   file, then `pkill -f '^/usr/lib/xdg-desktop-portal$'` (D-Bus starts it
   again on the next request).

   After updating sfe, the new `sfe-portal` answers once the old one has
   exited: it does so after two minutes without an open dialog (versions
   before that ran until logout: `pkill -x sfe-portal` once).

3. In Zen / Firefox: `about:config` → `widget.use-xdg-desktop-portal.file-picker` = `1`.
   Then *Save Page As…* or an upload button opens sfe.

4. Optional: make the dialogs float in Hyprland. They use the app id
   `sfe-chooser`; normal sfe windows stay `sfe`.

## Troubleshooting

**Chromium or an Electron app still shows the GNOME-style dialog.** These
programs ask the portal only if they are connected to the D-Bus session
bus when they start; otherwise they fall back to their built-in GTK
dialog. They connect only when `DBUS_SESSION_BUS_ADDRESS` is set, and
unlike GTK programs (Firefox, Zen) they don't fall back to the bus's
default socket, `$XDG_RUNTIME_DIR/bus`. On setups without systemd (Artix
and similar) the session bus often runs there without the variable being
passed to the compositor, so nothing started from it has the variable.

Check a running program (replace `chromium` with its process name):

```sh
tr '\0' '\n' < /proc/$(pgrep -o -x chromium)/environ | grep DBUS_SESSION_BUS_ADDRESS
```

No output means it can't use sfe. Set the variable for the whole session,
before anything is started. With Hyprland's Lua config
(`~/.config/hypr/hyprland.lua`), next to the other `hl.env` lines:

```lua
-- Programs started from Hyprland don't get the session bus address, so
-- Chromium / Electron apps can't reach the desktop portal (file dialogs).
local runtime_dir = os.getenv("XDG_RUNTIME_DIR")
if runtime_dir and not os.getenv("DBUS_SESSION_BUS_ADDRESS") then
    hl.env("DBUS_SESSION_BUS_ADDRESS", "unix:path=" .. runtime_dir .. "/bus")
end
```

Other setups can export it where the session starts, e.g. in `~/.zprofile`
before the compositor is launched:

```sh
export DBUS_SESSION_BUS_ADDRESS="unix:path=$XDG_RUNTIME_DIR/bus"
```

Programs that are already running keep their old environment, and so do
the ones a bar or launcher started at login opens for you: restart the
session (or quit the program and start it through the compositor, e.g.
`hyprctl dispatch 'hl.dsp.exec_cmd("chromium")'`).

**sfe still behaves like an older version after an update.** The old
`sfe-portal` may still be answering: it exits after two minutes without an
open dialog (versions before 1.4.0 ran until logout). `pkill -x sfe-portal`
makes the next dialog start the installed one.

## What works

- Open one file, several files (`multiple`), or a folder (`directory`,
  `<input webkitdirectory>` in browsers: only folders are listed). Only a
  request for several files selects more than one item: otherwise
  Ctrl/Shift+click and the rubber band pick just one, and *Select All* and
  visual mode (`v`) are off.
- Save with the suggested name and folder; replacing an existing file asks
  first. "Save as" for an existing file starts there.
- SaveFiles (saving several files at once into a folder).
- The request's title and button label (with its mnemonic, GTK programs
  send `_Open`); closing the request (the program gave up) closes sfe.
- **File type filters** (`filters`, `current_filter`): a drop-down in the
  bar; files that don't match are hidden (folders stay). Globs are shell
  patterns (`*.[pP][nN][gG]` works) and match regardless of case;
  `image/*` matches every image type. A `current_filter` sent without a
  list is the only filter (the program wants just those files), and one
  missing from the list is added to it. The filter in use is returned.
- **Choices**: the extra options some dialogs show (an encoding, "open
  read-only") as drop-downs and check boxes; their values are returned.
- **The parent window** (`parent_window`): the dialog is attached to the
  program that asked (`wayland:` handles through xdg-foreign, `x11:` window
  ids), so the compositor can keep it above that window.
- **Preview** (when opening files): the selected file's thumbnail (images, and videos or
  documents with a thumbnailer) or icon, with its type, size, date and an
  image's dimensions.

Tested through the real portal with Zen; through xdg-desktop-portal on a
private D-Bus session bus with a GTK 3 program asking the way Firefox does;
and in `tests/test_portalreq.c`, which turns requests shaped like
Firefox's (`*.[pP][dD][fF]` globs, folder requests that also carry filters)
into sfe's arguments and checks which files the dialog then lists.

To try different filters from a browser, open
[`tests/manual/file-dialogs.html`](../tests/manual/file-dialogs.html) from
disk: it opens the Open dialog with a chosen `accept` list (and multiple /
folder), checks what comes back against it, and saves files of a chosen
name and type (a download, or `showSaveFilePicker` in Chromium-based
browsers).

## Not done yet

- `modal` is ignored: the program that asked stays usable.
- Each dialog starts a full sfe process; it takes a moment to appear.
