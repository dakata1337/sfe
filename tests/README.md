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

## Monkey

`tools/ui-monkey.sh` sends random but reproducible input to the real
windows: keys from the whole keyboard with modifiers (to whatever window or
dialog is on top), `:` commands with random arguments, actions, and files
changing on disk. After every action sfe must still run and log no warning
or critical. It is not part of `meson test` (it is random, and can trip
over GTK itself), and runs as long as you like:

```bash
tools/ui-monkey.sh -b build -n 2000 -r 10    # 10 runs of 2000 actions
tools/ui-monkey.sh -b build -s 8 -n 500      # replay seed 8
tools/ui-monkey.sh -b build -n 2000 -r 48 -j 24   # 24 at a time
```

With `-j` each run gets its own core, spread over every CCD (the order of
`tools/fuzz.sh -p`, from `tools/cpus.sh`), and its own headless display. A
monkey mostly waits for sfe to settle (about 20% of a core), so more jobs
than cores is fine: beyond the CPU count, jobs share CPUs in the same order.

On a failure it prints the seed and the last 40 actions (with the focused
widget of each key). Timing still varies, so a replay may need a few tries.
It cannot touch anything outside its folder: `PATH` holds only `bwrap` (for
GTK's image loaders), GIO sees no applications, its data dirs are out of
its reach, and a view that leaves its folder is sent back after every
action. Findings: `fuzz/FINDINGS.md`, section "UI monkey".

The parsers are also fuzzed: see `fuzz/README.md`.
