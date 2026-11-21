# Fuzzing findings

Every problem the fuzz targets found, newest first: what it was, how it
could happen in sfe, and the fix with its regression test. Problems in a
harness (not in sfe) are listed too, so the same mistake is not repeated.

## UI monkey (2026-11-21, `tools/ui-monkey.sh`)

### M1. GLib critical "g_signal_handler_disconnect: handler_id > 0" (open)

- **Found by:** the monkey, seed 8, at action 273 (3 of 5 replays; timing
  dependent; another run hit it at 243).
- **Actions before it:** keys in the Settings window with the focus on
  its page list (Shift+C, 1, Return, Shift+Space), then `:visual` in the
  main window.
- **Where:** inside GTK while handling events; no sfe function is on the
  stack. sfe's own disconnects (`split.c` one-shot size handler,
  `prefs.c` config handler on destroy) were read and look correct.
- **Status:** open. Needs GTK's debug symbols to see which GTK code
  disconnects, and whether it happens on Wayland too or only on the
  headless broadway display.

### Harness mistakes (not sfe)

- The monkey moved the sandbox's data folder away (`:cut share`, the
  folder was reachable with `cd ..`), so GTK lost its icon theme and
  aborted. The sandbox tools now live in a separate folder, and views are
  kept inside the monkey's own folder.
- An empty `PATH` broke GTK's image loaders, which run in a `bwrap`
  sandbox (glycin); `PATH` now holds only `bwrap`.
- Destroying a window also destroys its dialogs: the cleanup walked a
  list of already destroyed dialogs (a critical). It now holds references.
- The monkey can take permissions away (the Properties dialog), so cleanup
  restores them before deleting.

## Round 5 (2026-11-21, 1800 s, all 11 targets with `-p`)

Nothing found. The corpora keep growing (config 11,167 inputs, rename
3,780, thumbnailer 2,685), so new paths are still being reached, just no
new failures.

## Round 4 (2026-11-21, 900 s, all 11 targets at once with `-p`)

Nothing found: about 190 million inputs over the 11 targets. `trashinfo`
and `portalconf` reach little of sfe's own code (their parsing is GLib's
keyfile reader, which is not instrumented), so their clean result says less.

## Round 3 (2026-11-21, 600 s per target, 4 new targets)

### 7. Finding 6 was fixed in only one place (`portalreq`)

- **Found by:** `fuzz-portalreq` (the same critical, from line 166).
- **Input:** a `#choice ID VALUE` line from the dialog whose value is not
  UTF-8.
- **Cause:** round 2 added the UTF-8 check after the `#filter` and
  `#choice` branches, so only URI lines were checked.
- **Fix:** every line is checked first, at the top of the loop.
- **Test:** `/portalreq/current-filter-alone` (now with a `#choice` line).
- **Lesson:** put an input check where the input enters, not next to the
  line that crashed.

### 8. SaveFiles names "..", "/" and "." pointed outside the chosen folder (`portalreq`) — security

- **Found by:** `fuzz-portalreq` (critical from `g_file_get_child`, for an
  absolute name).
- **Input:** a SaveFiles request whose `files` contains `/`.
- **Cause:** a program saving several files sends their names; sfe takes
  each name's last part with `g_path_get_basename()`, which leaves `/`,
  `.` and `..` as they are. For `..` the reply named the parent of the
  folder the user chose, and the portal grants the asking program (maybe a
  sandboxed Flatpak) access to the URIs in the reply: write access outside
  the folder the user picked.
- **Fix:** such names become `file` (then `file (2)`, ... as for any name
  in use), so every URI is a file inside the chosen folder.
- **Test:** `/portalreq/save-files`.

### 9. Bookmark labels from the GTK file were not checked for UTF-8 (`bookmarks`, new target)

- **Found by:** `fuzz-bookmarks` (invariant: label is UTF-8).
- **Input:** a GTK bookmarks line with a Latin-1 label (`file:///opt Café`
  saved by a program in another encoding).
- **Cause:** other programs write `~/.config/gtk-3.0/bookmarks`; sfe passed
  the label to the sidebar as it was, and GTK only takes UTF-8 (a Pango
  warning and a garbled name).
- **Fix:** labels go through `g_utf8_make_valid` (shown as `Caf�`).
- **Test:** `/bookmarks/list`.

### New targets and tooling

- New targets for files sfe reads that other programs write: `bookmarks`,
  `trashinfo`, `thumbnailer` (with a safety invariant: a file's path is
  always exactly one argument of the thumbnailer), `portalconf`.
- The file targets spent most of their time in `fsync` (via
  `g_file_set_contents`): they now write with plain `fwrite` into a folder
  on tmpfs, about 50 times faster.
- `tools/fuzz.sh -p` runs the targets at the same time pinned to cores
  across both CCDs; the scheduler had put all of them on one.
- Editing `tools/fuzz.sh` while it ran broke the running copies ("ue:
  command not found": bash reads scripts as it goes). The script is now a
  function called on its last line, so bash has read all of it first.

## Round 2 (2026-11-21, 180 s per target)

### 4. Long counter width in mass rename padded to a billion characters (`rename`)

- **Found by:** `fuzz-rename` (out of memory).
- **Input:** the template `{n:1111111111111115}`.
- **Cause:** `expand_template` (`src/rename.c`) read the width with
  `atoi()`, which overflows (undefined behaviour) and here gave a negative
  number. `MIN(width, 32)` kept it, and a negative `printf` width means
  "left-align in that many characters": sfe froze building a string of over
  a billion characters while the user typed a long width in the dialog.
- **Fix:** the width is read with `g_ascii_strtoull` (saturates instead of
  overflowing) and capped at 32.
- **Test:** `/rename/template-counter`.

### 5. An empty argument quoted to nothing (`cmdline`)

- **Found by:** `fuzz-cmdline` (quote/split round trip).
- **Cause:** `sfe_cmdline_quote("")` returned an empty string, which splits
  into no argument at all, not into one empty argument.
- **Fix:** it returns `""`.
- **Test:** `/cmdline/quote`.

### 6. A non-UTF-8 line from the dialog made the portal log a critical (`portalreq`)

- **Found by:** `fuzz-portalreq` (critical from `g_variant_builder_add`).
- **Cause:** `sfe_portal_request_results` put every line `sfe --choose`
  printed into a GVariant string, which must be UTF-8. sfe prints URIs
  (ASCII) and choice values, so this needs a broken dialog; the reply to
  the requesting program would have been malformed.
- **Fix:** such lines are skipped.
- **Test:** `/portalreq/current-filter-alone`.

### Tooling mistake

- With `-jobs`, libFuzzer writes `fuzz-N.log` into the current folder;
  targets run at the same time all wrote into the repository root, so
  their summaries mixed (the crash files were right). `tools/fuzz.sh` now
  runs each target in `build-fuzz/work/TARGET` and passes `-jobs` only
  for `-j` > 1.

## Round 1 (2026-11-21, 90 s per target)

### 1. Key binding with invalid UTF-8 read past the string (`keymap`, `config`)

- **Found by:** `fuzz-config` (ASan heap-buffer-overflow in
  `sfe_keymap_parse`, `src/keymap.c`), `fuzz-keymap`.
- **Input:** `[keys]` entry `"<C-w>\366" = "split vertical"`: one Latin-1
  byte, as an editor saving in the wrong encoding writes it.
- **Cause:** tomlc17 does not check that the file is UTF-8.
  `sfe_keymap_parse` steps through the sequence with `g_utf8_next_char()`;
  a lead byte announcing a 4-byte character jumps over the terminating NUL
  and keeps reading. A crash or garbage bindings at start.
- **Fix:** `sfe_config_parse` rejects a file that is not UTF-8 with
  "(line N) not valid UTF-8" (sfe keeps the previous settings and notifies,
  like any other config error), and `sfe_keymap_parse` rejects such a
  sequence itself.
- **Tests:** `/config/invalid-utf8`, `/keymap/invalid-utf8`.

### 2. File dialog filter with invalid UTF-8 logged a GLib critical (`filefilter`)

- **Found by:** `fuzz-filefilter` (critical from `g_variant_new_string`
  inside `g_variant_parse`).
- **Input:** a filter pattern containing the bytes `\333\321`.
- **Cause:** `sfe_file_filters_parse` passed the text straight to
  `g_variant_parse`, which logs a critical for invalid UTF-8 in a string.
- **Fix:** the text is checked first and refused with an error.
- **Test:** `/filefilter/errors`.

### 3. Fuzzy matching on invalid UTF-8 read past the string (`fuzzy`, `cmdline`)

- **Found by:** `fuzz-fuzzy` and `fuzz-cmdline` (ASan heap-buffer-overflow
  in `has_upper`, `src/fuzzy.c`).
- **Cause:** the same `g_utf8_next_char()` walk.
- **Not a bug in sfe:** every caller passes typed text or display names,
  which GIO guarantees are UTF-8. The harnesses now only pass UTF-8, as
  the callers do (documented in each harness).

### Harness mistakes

- `fuzz-rename` reported invalid UTF-8 output for an invalid UTF-8
  template; the rename dialog only passes UTF-8, so the harness now does too.
- `fuzz-portalreq` leaked the `GVariant` it built: the floating reference
  of `g_variant_get_normal_form()` was not taken (`g_variant_take_ref`).
