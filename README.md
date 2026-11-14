# sfe — Simple File Explorer

A minimal, Wayland-native GTK3 file manager written in C.

## Building & running
```sh
meson setup build
ninja -C build
meson test -C build
./build/sfe [DIRECTORY...]
```

## Arch / Artix package
```sh
cd packaging/arch
makepkg -si
```
`packaging/arch/PKGBUILD` lists every runtime dependency, annotated with the
module that needs it.

## Runtime dependencies
| Package | Used for |
|---|---|
| gtk3, glib2, gdk-pixbuf2 | UI, file I/O, image thumbnails |
| udisks2 + polkit | mounting drives without root (works with elogind, no systemd needed) |
| xdg-utils | `xdg-open`, the default way files are opened |
| ffmpeg | video thumbnails (fallback when no thumbnailer is installed) |
| ffmpegthumbnailer *(optional)* | faster video thumbnails |

## Keyboard & mouse
| Key | Action |
|---|---|
| Enter / double-click | open folder or file |
| Backspace, Alt+Up | parent folder |
| Alt+Left / Alt+Right, mouse back/forward | history back / forward |
| Ctrl+L | edit the path (Tab completes, Esc cancels) |
| Ctrl+H | show/hide hidden files |
| Ctrl+D | bookmark the current folder (again to remove) |
| Ctrl+\\ | split side by side |
| Ctrl+- | split the right view top / bottom |
| F6 | move focus to the next view |
| Ctrl+W | close the active view (the window, if it is the last one) |
| Ctrl+1 / Ctrl+2 | grid / list view |
| F5, Ctrl+R | reload |
| Ctrl+C / Ctrl+X / Ctrl+V | copy / cut / paste files |
| Delete / Shift+Delete | move to trash / delete permanently (asks first) |
| Ctrl+A | select all |
| Ctrl+Shift+N | new folder |
| Alt+Enter | properties |
| Menu key, Shift+F10 | context menu |
| Ctrl+N | new window |
| Ctrl+Q | quit |

Click a column header in the list view to sort by it; click again to reverse.

## Split views
Up to three directory views: `left | right-top / right-bottom`. Each view has
split and close buttons next to its path:
- with one view, *split side by side* creates the right view;
- the right view can then be *split top / bottom*;
- closing the left view slides the right column over.

The view you last clicked or typed in is the active one (accent line under
its path bar); shortcuts, the sidebar and the directory tree act on it.

## Sidebar
- **Places, Bookmarks, Devices** on top. Click to open, middle-click or
  right-click → *Open in New Window*. Drag bookmarks to reorder them;
  right-click → *Remove Bookmark*. Bookmarks from `~/.config/gtk-3.0/bookmarks`
  are listed too (read-only).
- **Devices** come from UDisks2. Clicking an unmounted drive mounts it and
  opens it; the ⏏ button unmounts (and for USB drives powers them off so they
  can be unplugged).
- **Directory tree** in the bottom half follows the current folder. Its root is
  your home folder, or the drive / file system the folder is on.

Removable drives mount without a password. Internal partitions ask for an
admin password through your polkit agent; to allow them without one for
members of `wheel`, create `/etc/polkit-1/rules.d/50-udisks.rules`:
```js
polkit.addRule(function(action, subject) {
    if (action.id.indexOf("org.freedesktop.udisks2.filesystem-mount") == 0 &&
        subject.local && subject.active && subject.isInGroup("wheel"))
        return polkit.Result.YES;
});
```
Encrypted (LUKS) drives are not supported yet.

## Working with files
Right-click a selection for *Open*, *Open With*, *Cut*, *Copy*, *Move to
Trash*, *Delete Permanently*, *Add to Bookmarks* and *Properties*; right-click
empty space for *New Folder*, *New File*, *Paste*, *Select All*, *Show Hidden
Files* and the folder's *Properties*.

- Copy, cut and paste use the system clipboard in the same format as other
  file managers, so they work between split views, between windows and with
  other applications.
- Copies and moves run in the background; longer ones show a progress bar
  with a cancel button at the bottom of the window. When a file already
  exists you can *Replace*, *Skip* or *Keep Both* (`name (2).ext`), once or
  for all conflicts. Folders are merged.
- Trash follows the freedesktop spec (`~/.local/share/Trash`, or
  `.Trash-UID` on other drives), so other tools can restore the files.
- The properties dialog counts folder sizes in the background and lets you
  edit permissions.

### Custom commands
Add your own entries to the right-click menu in `config.toml`:
```toml
[[actions]]
name = "Open terminal here"
command = "foot -D %d"

[[actions]]
name = "Compress to archive.tar.gz"
command = "tar -czf archive.tar.gz %f"
```
`%d` is the current folder. An argument that is exactly `%f` becomes one
argument per selected file; `%f` inside a longer argument is the first file.
Commands run without a shell, in the current folder.

## Opening files
Files open with `xdg-open`. To use a specific program for an extension, add it
to the `[open]` table (`%f` is the file; without `%f` the path is appended):
```toml
[open]
mp4 = "mpv --fs %f"
"tar.gz" = "file-roller %f"
```

## Configuration
`~/.config/sfe/config.toml` is created with commented defaults on first run.
Edits to it are applied while sfe is running.

## Theming
sfe draws its own flat style (`src/style.css`), but every color comes from your
system: libadwaita-style color names (`window_bg_color`, `accent_bg_color`, …,
as written by Noctalia, matugen or adw-gtk3) are preferred, with the GTK3
theme's colors as the fallback. sfe watches `~/.config/gtk-3.0/` and the GTK
theme setting, so a regenerated color scheme applies instantly.

Debug logging: `G_MESSAGES_DEBUG=sfe ./build/sfe`

## Third-party dependencies
### Statically linked:
- [tomlc17](https://github.com/cktan/tomlc17) by cktan (MIT)
