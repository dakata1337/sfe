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
| Ctrl+1 / Ctrl+2 | grid / list view |
| F5, Ctrl+R | reload |
| Ctrl+Shift+N | new window |
| Ctrl+Q | quit |

Click a column header in the list view to sort by it; click again to reverse.

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
