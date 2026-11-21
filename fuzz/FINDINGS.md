# Fuzzing findings

Every problem the fuzz targets found, newest first: what it was, how it
could happen in sfe, and the fix with its regression test. Problems in a
harness (not in sfe) are listed too, so the same mistake is not repeated.

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
