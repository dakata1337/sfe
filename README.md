# sfe — Simple File Explorer

![sfe](docs/guide/images/overview.png)

sfe is a small, fast file manager for Wayland, written in C with GTK3. It
aims to stay minimal and readable while covering what a daily file manager
needs: a sidebar with bookmarks, drives and a folder tree; up to three split
views; fuzzy search; thumbnails and album covers on folders; copy, move,
trash, undo and batch rename;
and custom commands. Everything also works from the keyboard, vim style,
with a `:` command line.

It follows your GTK theme and reloads colour schemes from tools like
Noctalia, matugen or pywal instantly. Drives mount without a root password
through UDisks2, and nothing depends on systemd, so it runs on Artix, Gentoo
and other OpenRC distributions too.

**[Read the user guide](docs/guide/index.md)** for every feature and
keyboard shortcut.

## Installing

On Arch / Artix, `packaging/arch/sfe` builds the latest release with
`makepkg -si`. Building from source, running it without installing, the
dependencies and switching from a manual install are covered in
**[docs/install.md](docs/install.md)**.

## Documentation

- [Building and installing](docs/install.md)
- [The user guide](docs/guide/index.md), also installed to
  `/usr/share/doc/sfe/guide/index.md`
- `man sfe`
- Settings live in `~/.config/sfe/config.toml` (or Ctrl+P)

## License

sfe is released under the MIT license.

It statically links [tomlc17](https://github.com/cktan/tomlc17) by cktan
(MIT).
