# Building and installing sfe

## From source

Get the source of the latest release from the
[releases page](https://github.com/dakata1337/sfe/releases), or clone the
repository for the development version. Then:

```sh
meson setup build --prefix=/usr     # optimized release build
ninja -C build
meson test -C build
./build/sfe [DIRECTORY...]          # try it without installing
sudo meson install -C build         # sfe, its .desktop file, icon, man page and guide
```

`sudo ninja -C build uninstall` removes it again. Installing adds sfe to
application launchers and lets *Settings → System* make it the default file
manager. For working on sfe, `meson setup build -Dbuildtype=debug` gives a
debug build.

## Arch / Artix

There are two PKGBUILDs with all runtime dependencies
listed; the packages replace each other:

```sh
cd packaging/arch/sfe && makepkg -si       # sfe: the latest release from GitHub
cd packaging/arch/sfe-dev && makepkg -si   # sfe-dev: this checkout, as it is
```

### Switching from a manual install

If sfe was installed with `sudo meson install` before, pacman refuses to
overwrite those files (“exists in filesystem”). Remove that install first,
from the build folder it was made from (it uses Meson's install log, so it
removes exactly those files):

```sh
sudo ninja -C build uninstall
```

or let pacman take the files over:
`sudo pacman -U --overwrite '/usr/*' sfe-*.pkg.tar.zst`.

## Dependencies

Build: meson, ninja, a C17 compiler, gtk3 and glib2 headers (tomlc17 is
included in the source). Runtime: gtk3, glib2, udisks2 + polkit (mounting), xdg-utils
(opening files) and ffmpeg (video thumbnails); archive tools, thumbnailers,
bubblewrap (runs the thumbnailers in a sandbox),
xdg-desktop-portal (sfe as other programs' Open / Save dialog) and
ntfs-3g (NTFS drives the kernel driver refuses) are optional. See the PKGBUILDs for the full, annotated list.
