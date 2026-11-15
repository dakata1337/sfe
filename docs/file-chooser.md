# sfe as the system file dialog (experimental)

Status: a working prototype on the `explore-file-chooser` branch. It answers
the desktop's file dialogs, but it is not part of a release yet.

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

## Trying it

1. Install sfe from this branch, so `sfe-portal`, its `.portal` file and its
   D-Bus service file are in place:

   ```sh
   meson setup build --prefix=/usr && ninja -C build && sudo meson install -C build
   ```

   (or `makepkg -si` in `packaging/arch/sfe-dev` on this branch).

2. Tell the portal to use sfe for file dialogs. Create
   `~/.config/xdg-desktop-portal/hyprland-portals.conf`:

   ```ini
   [preferred]
   default=hyprland;gtk
   org.freedesktop.impl.portal.FileChooser=sfe
   ```

3. Restart the portal so it reads the file (D-Bus starts it again on the
   next request):

   ```sh
   pkill -f '^/usr/lib/xdg-desktop-portal$'
   ```

4. In Zen / Firefox: `about:config` → `widget.use-xdg-desktop-portal.file-picker` = `1`.
   Then *Save Page As…* or an upload button opens sfe.

5. Optional: make the dialogs float in Hyprland (they use the app id
   `sfe-chooser`, normal sfe windows stay `sfe`).

To go back: delete the `FileChooser=sfe` line (or the file) and restart the
portal as in step 3.

## What works

- Open one file, several files (`multiple`), or a folder (`directory`).
- Save with the suggested name and folder; replacing an existing file asks
  first. "Save as" for an existing file starts there.
- SaveFiles (saving several files at once into a folder).
- The request's title and button label; closing the request (the program
  gave up) closes sfe.

Tested by calling the backend over a private D-Bus session bus, with the
dialogs driven from the keyboard. Not yet tried through the real portal with
a browser.

## Not done yet

- **File type filters** (`filters`, `current_filter`): sfe shows every file.
- **Choices** (extra options some dialogs show, e.g. an encoding).
- **The parent window** (`parent_window`): the dialog is not attached to the
  program that asked (on Wayland this needs xdg-foreign), so it opens as an
  ordinary window, and `modal` is ignored.
- Each dialog starts a full sfe process; it takes a moment to appear.
