# Changelog

What changed in each release of sfe. Changes waiting for the next release
are collected under *Unreleased*; when releasing, that heading becomes the
new version.

## Unreleased

- sfe can be the file dialog of other programs: *Settings → System → Use sfe
  for Open / Save dialogs* (through xdg-desktop-portal; Firefox and Zen
  also need `widget.use-xdg-desktop-portal.file-picker = 1`). Behind it,
  `sfe --choose open|save|folder` and the `sfe-portal` backend. The dialog
  shows the program's file type filters and extra options, previews the
  selected file on the right and stays attached to the program's window.

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
