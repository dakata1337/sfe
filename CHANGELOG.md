# Changelog

What changed in each release of sfe. Changes waiting for the next release
are collected under *Unreleased*; when releasing, that heading becomes the
new version (`tools/release.sh X.Y.Z` does this, and the rest of a
release: see the top of that script).

## Unreleased

- A config.toml that is not UTF-8 (a stray byte from an editor saving in
  another encoding) is reported as an error with its line, instead of
  making sfe read past the end of a key binding. Found by fuzzing.
- File dialogs (portal): a program saving several files can no longer name
  one "..", "." or "/" to get a file outside the folder you chose. Found by
  fuzzing.
- Bookmarks from the GTK bookmarks file with a label in another encoding
  show readable (with a replacement character) instead of garbled.
- Mass rename: a very long counter width (`{n:1111111111111115}`) no longer
  freezes sfe; widths are capped at 32 digits. Found by fuzzing.

## 3.2.4

- Fixed a crash on every change in Settings, and the config being ignored at
  start ("error parsing float"), in languages that write decimals with a
  comma (Bulgarian, German, French, Russian, ...).

## 3.2.3

- Sidebar and Settings rows no longer show as dark boxes with themes that
  paint list rows (KDE's Breeze).

## 3.2.2

- Drag and drop files: between splits, between sfe windows and with other
  applications. Drag from a selected file to take the whole selection;
  dragging from an unselected file still draws a selection rectangle. A drop
  on a folder goes into it, elsewhere into the view's folder. It moves on the
  same file system and copies to another; hold Ctrl to copy, Shift to move.
- The trash and undo tests pass when `/tmp` is a tmpfs (CachyOS and others
  mount it so; the package build failed there).

## 3.2.1

- Open / save dialogs (also through the portal) open at 1200 x 800,
  shrunk to fit smaller screens (they were 1060 x 640 and 900 x 640).

## 3.2.0

- Search waits until typing pauses for 100 ms (`search_delay_ms`, in
  *Settings* under Search), and draws the typed letter before searching,
  so typing stays smooth in big folders (typing "file9999" in 100,000
  files: 0.35 s of work instead of 1.7 s). Enter and Up / Down still act
  on everything typed.
- Folders you leave stay loaded for 5 minutes (up to 512 MiB, the oldest
  dropped first; *Settings* → Memory), and changes on disk are picked up
  meanwhile. Going back to a folder of 100,000 files takes 0.65 s of work
  in the list instead of 2 s (the grid: 2.2 s instead of 3.5 s).
- The command line and the status corner no longer join letters such as
  "ffi" into one glyph (some monospace fonts, e.g. Nimbus Mono, do).
- Search can look in subfolders too: turn on *Search in subfolders*
  (`search_recursive`) in Settings. Matches below the folder show their
  path (`photos/2024/beach.jpg`), and Enter keeps them as a filter like
  any other search.
- The file-dialog service no longer leaks a little memory each time a
  program asks which filter was in use.

## 3.1.0

- Long names in the list view are shortened in the middle
  ("holiday photo … final edit.jpg"), so the extension stays visible.
- Grid view: long names get two lines with the "…" in the middle of the
  second, so the extension stays visible (they were cut off at the end).
- Big folders open much faster: 100,000 files take about 4 s of work in
  the grid (was 21 s) and 2.5 s in the list (was 12 s), and use less
  memory. Every grid item and list row now has the same size, and icons
  and texts are made only for the rows on screen. A thumbnail arriving
  redraws the view instead of laying it out again.

## 3.0.3

- In a narrow window, where the views stack, `split absolute` offers them
  as top / middle / bottom (or top / bottom) instead of left / right-top /
  right-bottom. The side-by-side names still work, so key bindings using
  them keep working.

## 3.0.2

- Grid view: a drag-selection can start on a file or in the empty space
  after the files (only empty space between files worked; the views do not
  drag files, so pressing on one and dragging did nothing). A click or
  right-click below the files, scrolled to the end, counts as empty space.
- Grid view: the mouse wheel works over the empty space after the files.
  Scrolled to the end, the wheel over that space did nothing (GTK's grid
  takes scrolling only where it has items).

## 3.0.1

- List view in a folder with long names: no more "value location passed as
  NULL" warnings, and the Type column is measured from every file (3.0.0
  stopped reading types once a name reached the width cap).

## 3.0.0

- Split views of the same folder hide Size, Modified and Type at the same
  window width. Each view learned a column's width only once it had shown
  it and guessed 90 px before, so a split opened narrow (Type never shown)
  disagreed with the other one. The widths now come from the texts: the
  widest type in the folder and fixed samples for size and date.
- In a folder of very many files, the list view hides Size, Modified and Type
  again when the window gets narrower. After a resize, GTK spends up to half a
  minute measuring the rows in the background, and the column check waited
  behind it, so the columns stayed and the names were squeezed to "a…xt".
- Views of the same folder share its contents: a new split on a folder that
  is already open no longer reads it again or keeps a second copy of its
  files. A new split of a 100,000-file folder takes 8 MB more instead of
  155 MB (list view; grid 63 MB instead of 211 MB) and appears without
  waiting for the disk. Its layout still takes GTK most of the time. F5 in
  one of them reloads all.
- Enter in the search (Ctrl+F) keeps the matches as a filter of the view
  instead of opening the highlighted one: a badge in the corner shows the
  query and count, the files can be worked on as usual, and Esc (or the
  badge's ✕) shows all files again. The letters that matched are marked in
  the names while searching or filtered. (Enter used to open the highlighted
  match; now Enter, then Enter again.)
- Memory goes back down after leaving or closing a view of a big folder:
  glibc kept the freed memory for reuse, so sfe stayed at its peak (about
  200 MB after visiting a folder of 100,000 files; 630 MB with three
  views). It is now returned to the system: 52 MB after going back.
- Command-line completion works right after a big folder loads. The list
  view measures its rows in the background after loading (about 30 s for
  100,000 files), and the completion update waited behind that work: the
  list stayed unfiltered and Tab cycled through the stale list. The update
  now runs ahead of it, and Tab always completes the text as typed.
- In a folder of very many files, the command line (and search) responds at
  once: opening or closing it moved the keyboard focus, which restyled the
  grid, and GTK then measured every icon again (about 7 s to open and 10 s
  to close in a folder of 100,000 files, with completion stuck meanwhile).
  The grid no longer changes style with focus; a selected icon keeps the
  same tint whether or not the grid has the focus.
- Thumbnails no longer go missing after scrolling away and quickly back: a
  new request could join a thumbnail job that had just been skipped and get
  "no thumbnail", leaving the plain icon until the folder was reloaded.
- Searching a folder of thousands of files no longer freezes at every key:
  when more than 1,000 files match, only the best 1,000 are listed, and the
  count says "best 1000 of 15000". In a 20,000-file folder a keystroke went
  from about 2.4 s to 0.16 s in the list view; in the grid the first key
  still takes under a second, the next ones a fraction of that.
- Moving between split views goes round them clockwise: `J` focuses the next
  view clockwise (left → right-top → right-bottom → left) and `K` the next
  counter-clockwise; `Ctrl+J` / `Ctrl+K` move the active view the same way.
  It works from every view (up / down did nothing from the left view
  before). `H`, `L`, `Ctrl+H` and `Ctrl+L` are gone, and so are the
  sub-commands `split left|right|up|down` and `split move left|right|up|down`:
  use `split next|previous` and `split move next|previous` (key bindings in
  `config.toml` that use the old ones need the new names).
- The Save dialog no longer accepts "." or ".." as a file name (it returned
  the folder itself, which the program then failed to write to).
- Properties: the permission check boxes work on your own read-only files
  (mode 0444). They were greyed out unless the file was writable, though
  the owner may always change permissions; now they follow ownership.
- Saving several files at once through sfe's file dialog (the portal's
  SaveFiles) no longer lets the program overwrite files already in the
  chosen folder: those get free names such as `photo (2).jpg`, as when
  copying.
- Files renamed right after they appear (a browser download's `.part`
  becoming the finished file) no longer leave a ghost entry under the old
  name, which could not be opened until the folder was reloaded.

## 2.1.2

- The size count behind the "Yanked … (32 MB)" notice stops when the notice
  goes away or a new one replaces it. Yanking a big folder tree (a home
  folder, /usr) kept a thread walking the whole tree for minutes.
- *Replace* no longer deletes the existing file before copying or moving
  the new one over it. A copy that failed or was cancelled (full disk,
  unreadable source, an unplugged drive) lost both; now the old file is
  only replaced once the new one is complete. Replacing a folder with a
  file, or the other way round, still removes the old one first.
- A `config.toml` with a syntax error is no longer overwritten. Any change
  made in sfe (a column header click, a bookmark, Settings) used to replace
  the broken file with the defaults, losing bookmarks, keys and custom
  commands; now such changes apply without being saved, with a
  notification, until the file is fixed.

## 2.1.1

- NTFS drives the kernel's driver refuses (Windows Fast Startup or
  hibernation leaves them unclean, and udisks reported only "wrong fs type,
  bad option, bad superblock") are mounted with ntfs-3g when it is
  installed; without it, the error says to install it.

## 2.1.0

- Lighter on big folders: about 800 bytes less memory per file (16 MB
  for 20,000 files) and about 12% less CPU to open them, with the same
  icons. sfe picks a file's icon from its type instead of keeping the
  icons GIO looked up for every file.

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
