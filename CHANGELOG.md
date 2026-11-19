# Changelog

What changed in each release of sfe. Changes waiting for the next release
are collected under *Unreleased*; when releasing, that heading becomes the
new version.

## Unreleased

- `:cd` without a folder goes to the start folder from the settings
  (it went to the home folder).
- `:mass_rename [all|files|directories]` renames the selected items (in
  visual mode or several), else the whole folder, with *Include* set to the
  argument; it no longer takes file names. `R` does the same.
- `:mkdir` and `:touch` take one name: `:mkdir Hello World!` makes the
  folder “Hello World!” (it made two folders).
- `:split close` is gone in favour of `:q` / `:quit` (`<C-w>q` runs `q`);
  the old name still works for now, with a note.
- `:split` completion says what each sub-command does, and whether it makes
  a view, moves the focus or moves the view; `:split move` offers all four
  directions.
- `G` jumps to the last item without selecting every item on the way (in
  the grid too); `gg` the same for the first.
- `dd` moves to the trash, like `x` (it was cut; cut is still Ctrl+X and
  `:cut`).
- `:open_with <program>` with nothing selected opens the current folder in
  it (it said there was nothing to open).

## 1.5.0

- Sort ascending or descending, saved in the config (`sort_order`, next to
  `sort_by`) and in *Settings*. The sort is the same in every view: a
  column header click, the new *Sort By* submenu (right-click empty space,
  also in the grid) and the new `:sort [<key>] [<direction>]` command all
  change and save it.

## 1.4.1

- Docs: Chromium and Electron apps (Claude Desktop, VS Code, …) use sfe as
  their file dialog only when they can reach the session bus.
  `docs/file-chooser.md` now has a troubleshooting section on setting
  `DBUS_SESSION_BUS_ADDRESS` for the session (needed without systemd,
  e.g. on Artix), and on an old `sfe-portal` still answering after an
  update.

## 1.4.0

- sfe can be the file dialog of other programs: *Settings → System → Use sfe
  for Open / Save dialogs* (through xdg-desktop-portal; Firefox and Zen
  also need `widget.use-xdg-desktop-portal.file-picker = 1`). Behind it,
  `sfe --choose open|save|folder` and the `sfe-portal` backend. The dialog
  shows the program's file type filters and extra options, previews the
  selected file on the right when opening and stays attached to the
  program's window. Folder dialogs list only folders, and only a request
  for several files selects more than one.

- Installed packages no longer print sfe's debug messages: debug logging
  now follows meson's `debug` option instead of "anything but a release
  build" (the Arch packages build with `buildtype=plain`).

## 1.3.0

- The views always end with empty space (a third of the view, at least
  six rows in the list and one row in the grid), to right-click for the
  folder's menu in long folders.
- In the grid, Shift+click and Shift+arrows select in reading order
  instead of a rectangle.
- In narrow views the list drops columns from the right (Type, then
  Modified, then Size) to make room for the names.
  Column widths follow their contents and can no longer be dragged.
- The sidebar says "No devices" (or "No removable drives" with the
  removable-only setting) instead of showing an empty *Devices* heading.

## 1.2.0

- Mounted drives in the sidebar show how full they are with a thin bar
  (red above 90%); the numbers are in the tooltip.
- The system disk (mounted at `/`) is listed under *Devices*.
- Devices are ordered: mounted disks, then disks that can be mounted, then
  removable drives.
- *Settings → Sidebar → Devices* can list only removable drives.

## 1.1.1

- Fixed two crashes in the command line completion.
- A view in a folder it cannot open (such as `/root`) can be focused,
  navigated away from and closed.
- Debug builds define `DEBUG` and enable GTK debug messages.

## 1.1.0

- File icons can follow the theme's colours: *tinted* (folders in the
  accent colour) or *symbolic* (one-colour icons), chosen in Settings from
  previews.

## 1.0.0

- The first release.
