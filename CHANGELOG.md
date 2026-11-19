# Changelog

What changed in each release of sfe. Changes waiting for the next release
are collected under *Unreleased*; when releasing, that heading becomes the
new version (`tools/release.sh X.Y.Z` does this, and the rest of a
release: see the top of that script).

## Unreleased

## 2.0.2

- `gg` (and other cursor keys) scroll back to the cursor when the view was
  scrolled into the empty space after the files; `gg` only selected the
  first item, or stopped a few lines up on fast screens.
- Thumbnails come back after F5 in the list view while it is scrolled into
  the empty space after the files (they showed plain icons until the view
  was scrolled).

## 2.0.1

- Settings → System shows the version, the build type (release / debug)
  and the git commit sfe was built from, selectable to copy into bug
  reports. Release tarballs carry their commit too.
- The build and install instructions moved from the README to
  `docs/install.md`.

## 2.0.0

### Breaking changes

- The old command names deprecated since 1.0.0 are removed: `toggle_hidden`,
  `toggle_sidebar`, `toggle_tree`, `view_grid`, `view_list`,
  `split_vertical`, `split_horizontal`, `close_pane`, `next_pane`,
  `select_all`, `paste_into`, `bookmark_folder`, `bookmark_selection`,
  `unbookmark_folder`, `unbookmark_selection`, `open_with_other`. Key
  bindings in `config.toml` that use them say “unknown command”; the guide
  has a table of the new names (`hidden toggle`, `q`, `paste <folder>`, …).
- `:split close` is removed: use `:q` / `:quit` (`<C-w>q` runs `q`).
- `:mass_rename` takes `all`, `files` or `directories` instead of file
  names. It renames the items selected in visual mode (or several selected
  ones), else the whole folder, with *Include* set to the argument; `R`
  does the same.
- `:mkdir` and `:touch` take one name, spaces included (`:mkdir Hello
  World!` makes one folder), and need it: the name popup is gone.
- Rename… (F2 on one file), New Folder… and New File… open the command line
  (`:rename <file> <name>` with the stem selected, `:mkdir `, `:touch `)
  instead of a popup. `:rename` joins the words of an unquoted new name.
- `:cd` without a folder goes to the start folder from the settings instead
  of the home folder.
- In visual mode, a command that acts on the selection now clears it.
  *Settings → General → Clear visual selection after use* (`visual_clear`)
  turns this off.

### New

- Folder covers: a folder holding `cover.jpg`, `folder.png`, `front.jpg`,
  `AlbumArt…` and the like shows that picture, with a small folder badge,
  instead of its icon. *Settings → Thumbnails → Folder covers*
  (`folder_covers` in `[preview]`).
- A status corner at the bottom left: the visual-mode line, and for five
  seconds what was yanked, cut or pasted, with the total size (`Yanked 3
  files (32 MB)`).
- Pasted files become the selection.
- Settings: the General options are grouped (Window, Looks, Files); options
  you changed are in bold italics, and double-clicking an option's name
  resets it.
- Settings → System shows the colours sfe took from the theme, each with the
  theme colour it came from.
- `:split` completion says what each sub-command does: make a view, move
  the focus or move the view.
- `:open_with <program>` with nothing selected opens the current folder in
  it.

### Fixed

- The visual-mode line no longer covers the command list: it moves above it
  while the command line is open.
- `G` jumps to the last item without selecting every item on the way (in
  the grid too), and `gg` to the first.
- `:split move` completes all four directions (it offered nothing with a
  single view).
- Error messages follow generated themes (Noctalia, matugen): `error_bg_color`
  is preferred over the base theme's `error_color`, which gave stock red;
  destructive buttons take their text colour from `error_fg_color` instead
  of white.
- Settings: Close sits in a footer with room around it, no longer over the
  page list; the Open / Save dialogs setting links to the guide instead of
  naming a Firefox setting.

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
