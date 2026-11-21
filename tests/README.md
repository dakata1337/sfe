# Tests

```bash
meson test -C build            # everything
meson test -C build ui -v      # one test, with its output
```

Test programs are built by `meson test` only, not by a plain build.

- **Unit tests** (`test_*.c`): the GTK-free code in `src/` (config, key
  bindings, file operations, rename, trash, undo, ...).
- **Widget tests** (`test_ui.c`): the real application and windows on a
  headless GTK display (`broadwayd`, part of GTK 3), in a throwaway home
  folder, driven like a user drives them: key presses, `:` commands, and
  the Settings widgets themselves. Skipped when `broadwayd` is missing.
  Each test variant runs in about 2 s.
- **Variants** that caught real bugs in other setups:
  - `ui-theme`: a Breeze-like theme that paints list rows
    (`data/themes/FakeBreeze`); sidebar rows showed as dark boxes (3.2.3).
  - `config-bg`, `ui-bg`: a decimal-comma language (Bulgarian), built into
    the build folder with `localedef`, so it needs no installed locale;
    every Settings change aborted there (3.2.4).

Writing a widget test: `open_window(dir)` gives a window on a folder made
by `make_folder()`; drive it with `key()`, `type()` and `run()` (a `:`
command), and wait for results with `WAIT_UNTIL(...)`, never a fixed sleep:
file jobs finish in the background (wait for the undo history to grow
before undoing, for example).

The parsers are also fuzzed: see `fuzz/README.md`.
