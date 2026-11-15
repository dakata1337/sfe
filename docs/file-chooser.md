# sfe as the system file dialog

sfe can answer other programs' Open / Save dialogs. Turn it on in
*Settings → System → Use sfe for Open / Save dialogs*; this page explains
what that does and how to set it up by hand.

## How file dialogs reach a program like sfe

Many programs don't draw their own Open / Save dialogs; they ask
**xdg-desktop-portal**. Firefox and Zen do this when
`widget.use-xdg-desktop-portal.file-picker` is `1` in `about:config`, as do
every Flatpak app and GTK apps run with `GTK_USE_PORTAL=1`. The portal
forwards the request to whichever **backend** is configured for
`org.freedesktop.impl.portal.FileChooser`. On a stock Hyprland setup that is
`xdg-desktop-portal-gtk` (`/usr/share/xdg-desktop-portal/hyprland-portals.conf`
says `default=hyprland;gtk`, and the Hyprland portal has no file chooser).

sfe adds two pieces:

- **`sfe --choose open|save|folder`**: a separate sfe window with a bar at
  the bottom (a name field for saving, *Cancel*, *Open* / *Save* /
  *Select*). Enter or a double-click on a file chooses it; Ctrl+Enter is the
  *Open* / *Save* button. It prints the chosen files as URIs and exits with
  0, or exits with 1 when cancelled. Try it on its own:

  ```sh
  sfe --choose open --multiple --folder ~/Downloads
  sfe --choose save --name report.pdf
  ```

- **`sfe-portal`** (installed to `/usr/lib/sfe-portal`, or the libexec
  folder of your prefix): a portal backend. For each Open / Save request it
  runs `sfe --choose` with the request's title, start folder, suggested
  name and button label, and returns what was chosen.

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

   and restarts the portal so the next dialog uses it. Switching it off
   takes the line out again. By hand: edit the file, then
   `pkill -f '^/usr/lib/xdg-desktop-portal$'` (D-Bus starts it again on the
   next request).

3. In Zen / Firefox: `about:config` → `widget.use-xdg-desktop-portal.file-picker` = `1`.
   Then *Save Page As…* or an upload button opens sfe.

4. Optional: make the dialogs float in Hyprland. They use the app id
   `sfe-chooser`; normal sfe windows stay `sfe`.

## What works

- Open one file, several files (`multiple`), or a folder (`directory`).
- Save with the suggested name and folder; replacing an existing file asks
  first. "Save as" for an existing file starts there.
- SaveFiles (saving several files at once into a folder).
- The request's title and button label; closing the request (the program
  gave up) closes sfe.

Tested through the real portal with Zen, and by calling the backend over a
private D-Bus session bus.

## Not done yet

- **File type filters** (`filters`, `current_filter`): sfe shows every file.
- **Choices** (extra options some dialogs show, e.g. an encoding).
- **The parent window** (`parent_window`): the dialog is not attached to the
  program that asked (on Wayland this needs xdg-foreign), so it opens as an
  ordinary window, and `modal` is ignored.
- Each dialog starts a full sfe process; it takes a moment to appear.
