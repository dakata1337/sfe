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
  sidebar.{c,h}   places, bookmarks, devices list
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
  fuzzy.{c,h}     pure scoring function (unit-tested)
  thumbs.{c,h}    thumbnail worker pool + freedesktop cache
  opener.{c,h}    xdg-open + extension overrides
  prefs.{c,h}     settings dialog
tests/            meson test: fuzzy, config round-trip, mass-rename patterns
data/             sfe.desktop, default config.toml
```

Extensibility hooks: adding a command means one row in the action table plus a callback. Users can also define their own actions in the config without recompiling:

```toml
[[actions]]
name = "Open terminal here"
command = "foot -D %d"     # %f selected files, %d current dir
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
thumbnail_max_size_mb = 50

[sidebar]
bookmarks = ["~/Downloads", "~/Documents"]
show_gtk_bookmarks = true     # also read ~/.config/gtk-3.0/bookmarks

[open]                        # extension -> command, %f = file
mp4 = "mpv %f"
pdf = "zathura %f"
```

Written atomically (temp file + rename). The settings dialog edits it, and a file watcher picks up edits made by hand.

## 5. Key behaviours

- **Navigation:** the path entry has tab-completion, Enter navigates, and an invalid path is shown in red. Back/forward is kept per pane; ↑ goes to the parent.
- **Splits (max 3 panes):** the layout is `sidebar | left | right-top / right-bottom`. Each pane has three buttons next to its path entry: vertical split, horizontal split, close.
  - With a single pane, only vertical split is enabled. It creates `left | right`.
  - Horizontal split is enabled only on the right pane, and only while the right column has one pane. It creates `right-top / right-bottom`.
  - Close removes that pane. If you close the left pane, the right column slides left, so right-top becomes left and right-bottom becomes right.
  - Built from two fixed `GtkPaned`s (outer horizontal, inner vertical). The new pane starts in the same directory as the pane it was split from.
  - The "active pane" (last focused, subtly highlighted) receives paste, search and shortcuts.
- **Clipboard:** one app-wide clipboard, so copy/cut/paste works inside a pane and across panes. Paste conflicts ask Overwrite / Skip / Rename / Apply-to-all. Large jobs run async with a progress bar.
- **Context menu:** right-clicking empty space acts on the folder (Paste, New folder, New file, Select all, Open terminal/custom actions, Properties). Right-clicking an item acts on the selection (Open, Open with…, Cut, Copy, Rename, Mass rename (when more than one item is selected), Move to trash, Delete, Bookmark (directories), Properties).
- **Search (Ctrl+F):** a floating entry centered horizontally, sitting at the start of the bottom third of the active pane. Every keystroke refilters through `GtkTreeModelFilter` and resorts by score. Ranking: exact match > prefix > word-boundary > substring > fuzzy subsequence (fzf-style bonuses for consecutive and boundary matches). Matching is case-insensitive unless the query has an uppercase letter. Esc closes the overlay and restores the view; Enter opens the top match.
- **Mass rename:** find/replace (plain or regex), prefix/suffix, numbering `{n:03}`, case change, extension kept or changed. A live old→new preview marks collisions in red. Renames run as a two-phase temp-name pass so swaps like a↔b work.
- **Shortcuts:** Ctrl+C/X/V, F2 rename, Delete → trash, Shift+Delete → delete, Ctrl+H hidden toggle, Ctrl+L path entry, Alt+←/→/↑, Backspace, Ctrl+F, Ctrl+N new folder, Ctrl+D bookmark, Ctrl+, settings.

## 6. Decisions confirmed with you

- Search: an in-window floating overlay (GtkOverlay), centered, in the bottom third.
- Splits: at most 3 panes (1 left, 2 stacked on the right), as described above.
- Views: grid + details list, switchable per pane.
- Mass rename: a built-in dialog with live preview.

## 7. Milestones (each one builds and runs)

1. ✅ **Skeleton:** meson deps, GtkApplication, config load/save with defaults, theme hot reload, empty window.
2. **Single pane:** async listing, grid + list view, icon theme icons, navigation + history, opener with overrides, hidden toggle, sorting.
3. **Sidebar:** places, bookmarks (add/remove/reorder), devices via UDisks2 (mount/unmount/eject, auto-navigate after mount).
4. **Splits:** 3-pane layout, v/h split rules, close + slide-left, active-pane tracking.
5. **File ops:** action table, context menu, clipboard, copy/move/trash/delete jobs, new file/folder, properties.
6. **Rename and mass rename.**
7. **Fuzzy search overlay** (with unit tests for `fuzzy.c`).
8. **Thumbnails:** images, videos, freedesktop cache, worker pool, lazy loading of visible items only.
9. **Settings dialog**, UI scaling, `.desktop` file, `meson install`, README.
