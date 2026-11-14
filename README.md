<p align="center"><img src="data/icons/hicolor/scalable/apps/sfe.svg" alt="sfe logo" width="96"></p>

# sfe — Simple File Explorer

![sfe](docs/guide/images/overview.png)

sfe is a small, fast file manager for Wayland, written in C with GTK3. It
aims to stay minimal and readable while covering what a daily file manager
needs: a sidebar with bookmarks, drives and a folder tree; up to three split
views; fuzzy search; copy, move, trash and batch rename; and custom commands.

It follows your GTK theme and reloads colour schemes from tools like
Noctalia, matugen or pywal instantly. Drives mount without a root password
through UDisks2, and nothing depends on systemd, so it runs on Artix, Gentoo
and other OpenRC distributions too.

**[Read the user guide](docs/guide/index.md)** for every feature and
keyboard shortcut.

## Building

```sh
meson setup build
ninja -C build
meson test -C build
./build/sfe [DIRECTORY...]
```

On Arch / Artix, `packaging/arch/PKGBUILD` builds a package with all runtime
dependencies listed:

```sh
cd packaging/arch
makepkg -si
```

Runtime dependencies: gtk3, glib2, udisks2 + polkit (mounting), xdg-utils
(opening files) and ffmpeg (video thumbnails, upcoming). See the PKGBUILD
for the full, annotated list.

## License

sfe is released under the MIT license.

It statically links [tomlc17](https://github.com/cktan/tomlc17) by cktan
(MIT).
