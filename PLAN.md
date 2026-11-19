# sfe — Simple File Explorer: implementation plan

## 1. Toolkit decision: GTK3 (confirmed)

| Concern | GTK3 | GTK4 |
|---|---|---|
| Follows system theme | Every classic theme ships `gtk-3.0` CSS; `gtk-theme-name` is fully supported | Upstream says custom themes are "not supported"; many themes have incomplete `gtk-4.0` CSS |
| Live theme switch (`gsettings set ... gtk-theme`) | Yes, applied live on Wayland | Yes |
| Hot reload of `~/.config/gtk-3.0/gtk.css` (what Noctalia/matugen/pywal write) | **Not automatic** in either toolkit — sfe will watch the file with `GFileMonitor` and reload a `GtkCssProvider` itself | Same |
| Wayland | Native backend, fine | Native backend, slightly better |
| Large directories | `GtkIconView`/`GtkTreeView` are OK up to ~20k entries | `GtkGridView` virtualizes better |

GTK3 wins on theming, which was the deciding requirement. Trade-off: GTK3 is in maintenance mode and its views are slower on huge directories.

Your setup (checked): Noctalia writes `~/.config/gtk-3.0/noctalia.css` and `gtk.css` imports it. With the file watcher, sfe will recolor the moment Noctalia regenerates the scheme.

## 2. Dependencies

Build time (all already on your system): `gtk+-3.0`, `gio-2.0`, `gio-unix-2.0`, `gdk-pixbuf-2.0`, and the vendored `tomlc17`. No libudisks2, gvfs or ffmpeg libs get linked.

Runtime, optional, degrading gracefully when missing:
- **udisks2 + polkit** — unprivileged mounting. sfe talks to UDisks2 directly over D-Bus with GDBus, so it needs no gvfs and works without systemd. UDisks2 is started by D-Bus activation, so no init script is required. polkit only needs a logind implementation, and elogind covers OpenRC. **udisks2 is not installed on your machine** (`pacman -S udisks2`); polkit and elogind are. Without udisks2 the sidebar still lists mounted filesystems from `/proc/self/mountinfo` but cannot mount anything.
- **Thumbnailers** — sfe follows the freedesktop thumbnail spec: it reads `/usr/share/thumbnailers/*.thumbnailer` and caches in `~/.cache/thumbnails/{normal,large}`, so the cache is shared with other file managers. Images are thumbnailed in-process with gdk-pixbuf. Videos use any installed thumbnailer (ffmpegthumbnailer, totem), with `ffmpeg -ss … -frames:v 1` as a fallback (ffmpeg is installed).
- **xdg-open** — the default opener. Per-extension overrides come from the config.

## 3. Source layout (one module = one responsibility)

```
src/
  main.c          GtkApplication entry, Wayland-first backend selection
  app.{c,h}       global state: config, clipboard, list of windows
  config.{c,h}    defaults, TOML load (tomlc17) + small TOML writer, file watch
  theme.{c,h}     gtk.css watcher / hot reload, UI scaling
  window.{c,h}    main window: sidebar | split tree, global shortcuts
  sidebar.{c,h}   places, bookmarks, devices list (top) + directory tree (bottom)
  dirtree.{c,h}   lazy directory tree that follows the active pane
  devices.{c,h}   UDisks2 D-Bus client (mount/unmount/eject) + mountinfo fallback
  split.{c,h}     up to 3 panes: left | right-top / right-bottom (GtkPaned), focus tracking
  pane.{c,h}      one directory view: path entry, ← → ↑, split/close buttons, view
  dirmodel.{c,h}  async listing into GtkListStore, GFileMonitor, sort/filter
  history.{c,h}   back/forward stack
  actions.{c,h}   ACTION TABLE — every command (copy, rename, …) is one entry;
                  context menu, shortcuts and custom user actions all read it
  ctxmenu.{c,h}   builds the right-click menu from the action table
  clipboard.{c,h} cut/copy/paste; also exports text/uri-list + x-special/gnome-copied-files
  fileops.{c,h}   async copy/move/trash/delete jobs, progress, conflict dialog
  rename.{c,h}    single rename + mass rename dialog (live preview)
  props.{c,h}     properties dialog (size, perms, owner, times, mime)
  search.{c,h}    Ctrl+F overlay UI
  keymap.{c,h}    normal-mode key sequences -> commands, chords, [keys] overrides
  cmdline.{c,h}   command line parsing (shell-like quoting) and completion ranking
                  (grouped: active view first, then fuzzy score)
  commands.{c,h}  the ":" commands, their completion sources; every action is a command
  cmdbar.{c,h}    ":" command bar UI: completion list, Tab cycling, history, errors
  fuzzy.{c,h}     pure scoring function (unit-tested)
  thumbs.{c,h}    thumbnail worker pool + freedesktop cache
  opener.{c,h}    xdg-open + extension overrides
  prefs.{c,h}     settings dialog
tests/            meson test: fuzzy, config round-trip, mass-rename patterns
data/             sfe.desktop, default config.toml
```

Extensibility hooks: adding a command means one row in the action table plus a callback. Users can also define their own actions in the config without recompiling:

```toml
[[actions.directories]]     # also [[actions.files]]; same name, different command
name = "Open terminal here"
command = "foot -D %f"     # %f selection (current dir if none), %d current dir
```

## 4. Config: `~/.config/sfe/config.toml`

```toml
[general]
start_path   = "~"
show_hidden  = false
ui_scale     = 1.0            # fonts via screen resolution, icons via icon size
view         = "grid"         # grid | list
icon_size    = 64
sort_by      = "name"         # name | size | modified | type
dirs_first   = true
confirm_delete = true
thumbnails   = true
thumbnail_max_mb = 50

[sidebar]
bookmarks = ["~/Downloads", "~/Documents"]
show_gtk_bookmarks = true     # also read ~/.config/gtk-3.0/bookmarks

[open]                        # extension -> command, %f = file
mp4 = "mpv %f"
pdf = "zathura %f"
```

Written atomically (temp file + rename). The settings dialog edits it, and a file watcher picks up edits made by hand.

## 5. Key behaviours

- **Sidebar layout:** a vertical `GtkPaned` with a draggable divider; the list gets 2/3 and the tree 1/3 by default, each scrolling (with a visible scrollbar) when its content does not fit.
  - **Top half:** flat rows with symbolic icons under dim section headers: *Places* (Home, Desktop, Documents, Downloads, …, Trash, File System), *Bookmarks* and *Devices*. Devices show a mount/eject button.
  - **Bottom half: directory tree.** A `GtkTreeView` with connector lines (`enable-tree-lines`) that lists directories only.
    - It follows the active pane: the tree expands down to the active pane's directory and highlights it.
    - The tree's root is the place that contains the active directory: `~` for anything under home, the mount point for a drive, otherwise `/`.
    - Children load lazily on expand. Hidden directories follow `show_hidden`. Expanded nodes are watched, so the tree updates live.
    - Clicking a node navigates the active pane; middle-clicking will open it in a new split once splits exist.
- **Navigation:** the path entry has tab-completion, Enter navigates, and an invalid path is shown in red. Back/forward is kept per pane; ↑ goes to the parent.
- **Splits (max 3 panes):** the layout is `sidebar | left | right-top / right-bottom`. Each pane has three buttons next to its path entry: vertical split, horizontal split, close.
  - With a single pane, only vertical split is enabled. It creates `left | right`.
  - Horizontal split is enabled only on the right pane, and only while the right column has one pane. It creates `right-top / right-bottom`.
  - Close removes that pane. If you close the left pane, the right column slides left, so right-top becomes left and right-bottom becomes right.
  - Narrow windows (split area under ~700 px) stack the views top / bottom instead of side by side; then the single view splits with the horizontal button, and the vertical one is disabled.
  - Built from two fixed `GtkPaned`s (outer horizontal, inner vertical). The new pane starts in the same directory as the pane it was split from.
  - The "active pane" (last focused, subtly highlighted) receives paste, search and shortcuts.
- **Clipboard:** one app-wide clipboard, so copy/cut/paste works inside a pane and across panes. Paste conflicts ask Overwrite / Skip / Rename / Apply-to-all. Large jobs run async with a progress bar.
- **Context menu:** right-clicking empty space acts on the folder (Paste, New folder, New file, Select all, Open terminal/custom actions, Properties). Right-clicking an item acts on the selection (Open, Open with…, Cut, Copy, Rename, Mass rename (when more than one item is selected), Move to trash, Delete, Bookmark (directories), Properties).
- **Search (Ctrl+F):** a floating entry centered horizontally, sitting at the start of the bottom third of the active pane. Every keystroke refilters through `GtkTreeModelFilter` and resorts by score. Ranking: exact match > prefix > word-boundary > substring > fuzzy subsequence (fzf-style bonuses for consecutive and boundary matches). Matching is case-insensitive unless the query has an uppercase letter. Esc closes the overlay and restores the view; Enter opens the top match.
- **Mass rename:** find/replace (plain or regex), prefix/suffix, numbering `{n:03}`, case change, extension kept or changed. A live old→new preview marks collisions in red. Renames run as a two-phase temp-name pass so swaps like a↔b work.
- **Shortcuts:** Ctrl+C/X/V, F2 rename, Delete → trash, Shift+Delete → delete, Ctrl+H hidden toggle, Ctrl+L path entry, Alt+←/→/↑, Backspace, Ctrl+F, Ctrl+Shift+N new folder (Ctrl+N new window, as in most file managers), Ctrl+D bookmark, Ctrl+, settings.

## 6. UI style

The style is flat and modern rather than GNOME/Adwaita-like: no gradients, borders or bevels, flat icon buttons with a soft hover tint, a filled rounded path entry, translucent rounded selection, thin overlay scrollbars and 1px hairline dividers.

The style lives in `src/style.css` and contains no hard-coded colors. At runtime `theme.c` builds a small palette (`@sfe_bg`, `@sfe_fg`, `@sfe_view_bg`, `@sfe_accent`, …) from whatever the system provides:
- It prefers libadwaita-style names (`window_bg_color`, `accent_bg_color`, …), which Noctalia, matugen and adw-gtk3 define.
- It falls back to classic GTK3 names (`theme_bg_color`, `theme_selected_bg_color`, …).

The palette is rebuilt whenever the GTK theme or `gtk.css` changes, so the flat style always uses the current color scheme. The style is scoped to the `.sfe-window` class.

## 7. Modal workflow (vim-like)

Every important feature is reachable from the keyboard. Mouse users lose nothing: the buttons and menus stay.

### Modes
- **Normal mode:** the file views. Single keys are commands.
- **Visual mode:** `v` in a file view; the cursor moves alone and selects chunks (below).
- **Command mode:** the `:` command line at the bottom.
- **Text input:** the path entry, the search field and dialogs. Keys type text there; Esc returns to normal mode.

### Normal-mode keys

| Keys | Command | Meaning |
|---|---|---|
| `j` / `k` | | cursor down / up |
| `h` / Backspace | `:cd ..` | parent folder |
| `l` / Enter | `:open` | open the folder or file |
| `gg` / `G` | | first / last item |
| `gh` | `:cd ~` | home |
| Ctrl+O / Ctrl+I | `:back` / `:forward` | history |
| Space | | toggle the selection of the current item and move down |
| `yy` | `:yank` | copy the selection (or the current item) to the clipboard |
| `dd` | `:cut` | cut |
| `p` | `:paste` | paste into this view |
| `x` / `X` | `:trash` / `:delete` | move to trash / delete permanently (asks) |
| `r` / `R` | `:rename` / `:mass_rename` | rename (milestone 6) |
| `zh` | `:hidden toggle` | hidden files |
| `zz` | `:center` | scroll the cursor (in visual mode the first selected item) to the middle |
| `v` | `:visual` | visual mode |
| `/` | `:search` | fuzzy search overlay (milestone 7) |
| `:` | | command line |
| Esc | | clear the selection, close overlays |

### Visual mode (TODO, 2026-11-15)

- `v` starts a chunk at the cursor; `j`/`k`/`gg`/`G` (and `h`/`l`, one item) stretch it like a Shift+click range. `v` again ends the chunk, the cursor then moves without selecting, and the next `v` starts another chunk: chunks add up like Ctrl+clicks. `o` jumps to the chunk's other end, Space toggles single items.
- `y` / `d` yank / cut at once. Any command that acts on the files ends visual mode, as do Esc, navigating, switching the view, searching, clicking, or another view becoming active. Leaving never changes the selection.
- Visual keys are a second keymap looked up first (`[keys.visual]` in the config); everything else falls through to the normal keys. `-- VISUAL --` and the count show in the bottom-left corner.

### Split keys (as you specified)

| Keys | Command | Meaning |
|---|---|---|
| Ctrl+W `v` | `:split vertical` | split side by side, with the same rules as the ◫ button (below) |
| Ctrl+W `s` | `:split horizontal` | split top / bottom, with the same rules as the ⊟ button (below) |
| Shift+`h`/`j`/`k`/`l` | `:split left/down/up/right` | move focus to the view in that direction |
| Shift+`W` | `:split next` | cycle focus clockwise: left → right-top → right-bottom |
| Ctrl+Shift+`h`/`j`/`k`/`l` | `:split move left/down/up/right` | swap the active view with the one in that direction |
| Ctrl+W `q` | `:q` | close the active view (the window if it is the last) |

- Split rules (shared by the keys, the buttons and Ctrl+\\ / Ctrl+-): one view splits either way; two views side by side split top / bottom and two stacked views split side by side, and then the view being split becomes right-top, the new one right-bottom and the other one moves left. Three views do not split. Where a split is not allowed the chord does nothing (the command line says why).
- `:split absolute left|right|right-top|right-bottom|top|bottom` focuses a position. Only positions that exist are offered: `left` / `right` side by side, `top` / `bottom` stacked, `left` / `right-top` / `right-bottom` with three views.
- Directions (`H`/`J`/`K`/`L`, `split move …`) follow the views on screen, so they also work in a narrow window where the views stack. At the edge they wrap around to the farthest view the other way (TODO, 2026-11-15); with no other view on that line they do nothing, without an error.
- Ctrl+W becomes a chord prefix, so the old Ctrl+W (close view) moves to Ctrl+W `q`.

### Key bindings are data
- A keymap table (`keymap.c`) maps key sequences to command strings.
- Users can override it in `config.toml`:
  ```toml
  [keys]
  "<C-w>v" = "split vertical"
  "yy" = "yank"
  ```
- While a chord is pending (`g`, `y`, `d`, `z`, Ctrl+W), the keys typed so far show at the right of the command bar, like vim's `showcmd`. After 0.5 s a list of the keys that can follow appears above it, with their help text (TODO, 2026-11-15); the chord then waits for the next key. A chord that isn't finished within 1 s without the list is dropped.

### Command line (`:`)
- **Layout:** a full-width bar at the bottom of the panes, like vim, with a completion list above it.
- **Keys:** Enter runs the command and Esc cancels. Tab / Shift+Tab cycle through the completions. Up / Down walk the command history of the session.
- **Errors** show in the bar in the error colour until the next key.
- **Arguments** are parsed like a shell (quotes, backslashes). Completion inserts names already quoted.
- **Paths:** names from the active view are inserted relative to its folder. Names from other views are inserted as full paths (with `~`).

**Context-aware completion.** Candidates come in groups, always in this order: the **active view** first, then the other views clockwise (right-top, right-bottom, then left). Within a group, candidates are ranked by the fuzzy matcher from milestone 7 (exact > prefix > word start > substring > fuzzy). Each candidate shows a small tag saying where it comes from (`left`, `right-top`, `bookmark`, `device`, …).

**Commands.** Every row of the action table is also a command; the arguments are what's new. Completion shows one row per command with its aliases (`mkdir | new_directory | new_folder`), the arguments dimmed and the help on the right, with the matched letters in the accent colour; actions that duplicate a command are not offered and are not commands (their old command names were removed in 2.0), and the key-binding helpers come last, dimmed (TODO, 2026-11-15).

| Command | Arguments (what completion offers) |
|---|---|
| `:cp` / `:mv` (`:copy` / `:move`) | `<file>… <destination>`. The destination is always required. For the second and later arguments, the other views' folders are offered first, then the files of the views. |
| `:ln` (`:symlink`) | `<target>… <destination folder or link name>` |
| `:rename` | `<file> <new name>`. Once the file is chosen, Tab offers its current name for editing. |
| `:mass_rename` | `[<file>…]` (default: the selection); opens the milestone 6 dialog |
| `:touch` / `:mkdir` | `<name>` (aliases `:new_file`, `:new_directory`, `:new_folder`) |
| `:trash` / `:delete` | `<file>…` |
| `:mount` / `:unmount` / `:eject` | `<device>`: unmounted drives for `mount`, mounted ones for the others |
| `:split` | sub-commands, with **`move` and `next` listed first**, then `vertical`, `horizontal`, `left`, `right`, `up`, `down`, `absolute`, `close` |
| `:bookmark` / `:unbookmark` | `[<folder>…]` / `[<bookmark>…]`; without arguments the folders selected on purpose (visual mode or several items), else the active view's folder |
| `:paste` | `[<folder>]` |
| `:open_with` | `[<application> [<file>…]]`: installed programs, the ones for the selected type first |
| `:sidebar` / `:tree` | `on/off/toggle` / `full/normal/toggle` |
| `:cd` / `:open` | `<path>` / `<file>` |
| `:hidden` / `:view` | `on/off/toggle` / `grid/list` |
| `:q` (`:quit`) | close the active view (the window if it is the last) |

## 8. Decisions confirmed with you

- Search: an in-window floating overlay (GtkOverlay), centered, in the bottom third.
- Splits: at most 3 panes (1 left, 2 stacked on the right), as described above.
- Views: grid + details list, switchable per pane.
- Mass rename: a built-in dialog with live preview.
- Look: flat and modern, not GNOME-like, with colors taken from the system theme.
- Sidebar: places, bookmarks and devices on top; a directory tree in the bottom half.
- Modal workflow: full vim-like normal mode, Ctrl+W chords, and a `:` command line with context-aware completion (active view first). The split keys follow your exact rules (section 7). `:copy` / `:move` / `:symlink` always need an explicit destination.

## 9. Milestones (each one builds and runs)

Every milestone also updates the user guide (`docs/guide/index.md`) and retakes its screenshots (`docs/guide/tools/screenshots.sh`). `meson test` fails if a shortcut is missing from the guide.

1. ✅ **Skeleton:** meson deps, GtkApplication, config load/save with defaults, theme hot reload, empty window.
2. ✅ **Single pane:** async listing, grid + list view, icon theme icons, navigation + history, opener with overrides, hidden toggle, sorting.
3. ✅ **Sidebar:** places, bookmarks (add/remove/reorder) and devices via UDisks2 (mount/unmount/eject, auto-navigate after mount) in the top half. The directory tree, which follows the active pane, goes in the bottom half (`dirtree.c`).
4. ✅ **Splits:** 3-pane layout, v/h split rules, close + slide-left, active-pane tracking.
5. ✅ **File ops:** action table, context menu, clipboard, copy/move/trash/delete jobs, new file/folder, properties.
6. ✅ **Rename and mass rename:** name prompt (F2; on the command line since 2.0), pattern dialog with template / find-replace / case / counter and live conflict preview (F2 on several, Shift+F2), two-pass rename with rollback. The directory model dropped GtkTreeModelFilter (it emitted criticals when shown rows were removed).
7. ✅ **Fuzzy search overlay:** tiered `fuzzy.c` scorer with unit tests; the directory model filters and orders by score; a floating card centred in the bottom third of the active pane (Ctrl+F; Up/Down/Enter/Esc).
8. ✅ **Modal workflow:** normal-mode keymap (data-driven, overridable in `[keys]`) with chord handling and showcmd. Split focus / swap commands. The `:` command line with history, argument parsing, and grouped context-aware completion using `fuzzy.c`. All commands from section 7. Unit tests for the keymap parser, the command parser and completion ordering. (`keymap.c`, `cmdline.c`, `commands.c`, `cmdbar.c`; every action is also a command.) Later: visual mode (`v`, chunks), `zz`, Ctrl+S hides the sidebar, the folder tree can fill the sidebar, chord suggestions, and undo (`undo.c`: an app-wide history of file changes; `:undo [<step>]`, `u`, Ctrl+Z; copy-like steps are undone into the trash, never deleted).
9. ✅ **Thumbnails:** images, videos, freedesktop cache, worker pool, lazy loading of visible items only. (`thumbs.c`: spec cache with failure markers shared with other file managers; gdk-pixbuf for images, installed `.thumbnailer` files, ffmpeg fallback for videos; a newest-first thread pool; panes request only the rows on screen plus a margin and cancel what scrolled away. Unit tests in `tests/test_thumbs.c`.)
10. ✅ **Settings dialog**, UI scaling, `.desktop` file, `meson install`, README. (`prefs.c`: a Settings window with General, Thumbnails, Sidebar, Keys, Custom Commands, Programs and System pages, `:settings [<page>]`; the scale also covers small icons, buttons and the sidebar (`sfe_scaled_icon_new`); a New Window desktop action, AppStream metainfo, a man page, the guide under `/usr/share/doc/sfe`, `--version`; `meson test` validates the data files.)
