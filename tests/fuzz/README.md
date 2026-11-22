# Fuzzing

libFuzzer targets for sfe's parsers, built with AddressSanitizer and
UndefinedBehaviorSanitizer. Each target feeds random inputs to one parser
and checks that it does not crash, leak or hit undefined behaviour, and
where possible that a round trip gives back what went in.

```bash
tools/fuzz.sh                 # every target, 60 s each
tools/fuzz.sh -t 600 config   # one target, 10 minutes
tools/fuzz.sh -t 3600 -j 8 cmdline   # 8 processes for an hour
tools/fuzz.sh -p -t 1800      # all targets at once for 30 minutes, one core each
```

With `-p` every target gets its own cores, handed out across all L3 caches
(CCDs) and over physical cores before their SMT siblings; left alone, the
scheduler kept all of them on one CCD of a Ryzen 5900X. With `-j N -p` each
target gets N cores.

The first run sets up `build-fuzz/` (needs clang). Inputs that reach new
code are kept in `build-fuzz/corpus/TARGET`, so later runs continue where
the last one stopped; `tests/fuzz/corpus/TARGET` holds the hand-written seeds.

| Target | Code | Input | Checks |
|---|---|---|---|
| `config` | `sfe_config_parse` | config.toml text | written config parses again and is unchanged (what every Settings change does) |
| `cmdline` | `sfe_cmdline_tokenize`, `_split`, `_quote`, `_rank` | a `:` command line; for ranking, query and candidates on separate lines | offsets inside the line; quoting an argument and splitting gives it back |
| `fuzzy` | `sfe_fuzzy_score`, `_match` | query, NUL, file name | score and match agree; positions increase and lie inside the name |
| `keymap` | `sfe_keymap_parse`, `_format`, `_bind`, `_lookup` | a key sequence (`<C-w>v`) | formats and parses back to the same keys; a bound sequence is found |
| `filefilter` | `sfe_file_filters_parse`, `_matches` | GVariant text `a(sa(us))`, NUL, file names | no crash |
| `rename` | `sfe_renamer_new`, `_apply` | 8 bytes of options, then template, find, replace and name on lines | valid UTF-8 out for valid UTF-8 in |
| `portalreq` | `sfe_portal_request_new`, `_results` | method byte, serialized `a{sv}` options (as D-Bus sends them), 0xff, `sfe --choose` output | no crash |
| `bookmarks` | `sfe_bookmarks_list` | the GTK bookmarks file, NUL, sfe's own bookmark entries | every bookmark has a file and a UTF-8 label |
| `trashinfo` | `sfe_trash_original_location`, `_find`, `_everything` | a `.trashinfo` file | an original location is absolute, and finds the item again (restoring is not fuzzed: it would move files where `Path=` says) |
| `thumbnailer` | `sfe_thumbnailers_load`, `sfe_thumbnailer_argv` | a `.thumbnailer` file, NUL, a file path | the path is never split or merged: an `Exec` word that is exactly `%i` gives exactly the path |
| `portalconf` | `sfe_portal_conf_set`, `_uses_sfe` | the user's portals.conf, NUL, the system file used as a seed | turning sfe on and off is reflected in the file |

## When something is found

The run stops and saves the input as `tests/fuzz/crashes/TARGET/crash-…` (or
`leak-…`, `timeout-…`). Reproduce it, with the full report:

```bash
build-fuzz/tests/fuzz/fuzz-config tests/fuzz/crashes/config/crash-…
```

Fix the bug, add the input's essence as a case to the matching test in
`tests/`, and remove the crash file.

## Adding a target

Write `tests/fuzz/fuzz_NAME.c` with `LLVMFuzzerTestOneInput()` (see `common.h`
for `FUZZ_CHECK` and the log setup), add NAME to the list in
`tests/fuzz/meson.build` and to `tools/fuzz.sh`, and put a few typical inputs in
`tests/fuzz/corpus/NAME/`. A target that reads files gets a temporary folder from
`fuzz_tmpdir()` (on tmpfs, removed at exit; it is also `HOME` and
`XDG_DATA_HOME`) and writes the input there with `fuzz_write()`. Only GTK-free code (the `sfe-core` library) can be
fuzzed this way.
