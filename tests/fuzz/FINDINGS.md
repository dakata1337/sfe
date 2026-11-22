# Fuzzing findings

Every problem the fuzz targets found, newest first: what it was, how it
could happen in sfe, and the fix with its regression test. Problems in a
harness (not in sfe) are listed too, so the same mistake is not repeated.

## UI monkey (2026-11-21, `tools/ui-monkey.sh`)

### M10. In the tests, worker threads ran GTK code (harness; sfe hardened)

- **Found by:** `/ui/question-gone-without-answer` (M9) failing now and
  then in `meson test` while the monkey loaded the machine: an abort in
  `gtk_widget_get_frame_clock`; looped under load, 3 of 20 runs failed,
  each differently (`g_object_ref` on a non-object, realize and map
  assertions, a heap corruption in `calloc`).
- **Cause:** the backtrace showed the file-exists question being built on
  the copy's worker thread. Workers hand work to the main thread with
  `g_main_context_invoke()`, which calls the function in place when the
  calling thread can take the main context at that moment. sfe proper
  never lets it: `g_application_run()` holds the context for the whole
  run. The widget tests start sfe without it, so between two main loop
  iterations a worker could take the context and run GTK code next to the
  main thread. This probably explains several earlier one-off monkey
  crashes with no sfe code at fault: the heap corruption ("free():
  invalid size"), and the finalized or invalid objects in M8 and in the
  first parallel runs.
- **Fix:** the test holds the main context for its whole run, as
  `g_application_run()` does. sfe no longer relies on that either: both
  hand-offs (`fileops.c`, `thumbs.c`) go through `sfe_on_main_thread()`
  (`util.c`), which always queues to the main loop.
- **Test:** the M9 test looped under load: 0 of 20 failures with the sfe
  change alone (3 of 20 before), and `meson test` with both.

### M9. A file-exists question closed with its window left the copy waiting

- **Found by:** reading the code after a monkey run (seed 1178423665) that
  closed this question by destroying it and crashed 15 actions later
  ("object_ref: assertion '!object_already_finalized'", not reproduced in
  12 replays; see M8).
- **Cause:** the question ("“a.txt” already exists", or a failed step)
  answers the copy only from its buttons (`response`). It goes with its
  window (destroy with parent) without one: the copy then waited for an
  answer for ever and its reference leaked. Closing the window cancels
  the window's jobs, so a user mostly met the leak; the monkey, closing
  the dialog alone, left the copy hanging in the bar.
- **Fix:** `opsbar.c` answers Cancel when the dialog is destroyed (a second
  answer is ignored) and drops its reference there.
- **Test:** `/ui/question-gone-without-answer`: paste onto an existing
  file, destroy the question; the copy must end, the file untouched. Fails
  without the fix (the bar keeps the job).

### M8. Rare criticals: one from the headless display, two open

- "gtk_widget_get_preferred_height_for_width: assertion 'width >= 0'":
  twice in one gdb batch (120 runs of 6000 actions), then once in 72 runs
  of 15000 (seed 4029520165: the Menu key in Settings, Keys). The stack is
  all GTK: a context menu being placed (`gtk_menu_popup_at_rect`) and
  fitted into the monitor. The headless display's monitor is 0 x 0 (its
  size comes from a browser, and none connects), so a menu away from the
  corner gets a negative width; no real screen does that. **Harness:**
  the monkey tolerates this critical while the monitor is empty.
- "gtk_widget_get_has_tooltip: assertion 'GTK_IS_WIDGET (widget)'": once
  (under strace, so no action log or backtrace) in about 1.2 million
  actions. sfe has no tooltip code of its own; GTK's tooltips still
  pointed at a destroyed widget. Keyboard tooltip mode (Ctrl+F1) with the
  focused pane closed was probed and is fine.
- "object_ref: assertion '!object_already_finalized'": once (seed
  1178423665, right after `action bookmark-selection`, 15 actions after
  the monkey destroyed a file-exists question, M9); 12 replays under gdb
  did not hit it, so there is no backtrace. Probably M10.
- The tooltip one is left open: it did not come back in 720,000 actions
  under gdb, nor in 1,080,000 more.

### M7. With list as the default view, a new window's first key went nowhere

- **Found by:** the monkey (2 of 48 runs of 10000 actions): right after
  `:q` closed the last window and a new one opened, the first key logged
  "gtk_widget_event: assertion 'WIDGET_REALIZED_FOR_EVENT'"; the monkey's
  note: the focus (the list) was not realized.
- **Cause:** a new pane showed the grid until its files loaded, whatever
  the configured view; the list view already had the focus. A key pressed
  in that moment (typing right after opening a window) went to a view not
  on screen. The monkey had switched the default view in Settings.
- **Fix:** `pane.c` shows the configured view from the start.
- **Test:** `/ui/new-window-list-view`: default view list, a new window,
  the focus must be on screen at once and a key must not log anything.
  Fails without the fix.

### M6. GTK's emoji chooser closed before it loaded (GTK, not sfe)

- **Found by:** the monkey with `-j`: "g_variant_unref: assertion 'value
  != NULL'" (9 failed runs of 20000 actions), and once an abort in
  `gtk_widget_get_frame_clock`, mostly while the monkey closed the Rename
  Files dialog.
- **Cause:** Ctrl+. in a text field opens GTK's emoji chooser, which loads
  its emoji in idle steps; destroyed before then, it frees data it does
  not have yet. A user gets there too: Ctrl+., Escape, Escape quickly in a
  dialog (probed: the critical). In a normal session it is a warning on
  stderr, nothing breaks.
- **Harness:** the monkey no longer closes windows while a popup (a menu,
  the chooser) has the input, as a user cannot either (the frame clock
  abort came from that), and it tolerates this one GTK critical instead of
  stopping.

### M5. Copying a folder into a symlink to itself copied endlessly

- **Found by:** the monkey, seed 3995308520 (passed, but left a folder
  its cleanup could not delete): `Trash/files/sub (4)/f0/f0/…`, 1351
  levels deep, until the path reached the 4096-byte limit.
- **Cause:** the check that refuses a folder inside itself
  (`inside_itself`, `src/fileops.c`) compared paths only. Reached through
  a symlink, the destination looks unrelated: with `link -> f0`,
  `:cp f0 link` puts `f0` inside itself, and the copy then copies its own
  output level by level. In real use: `cp Projects Work` with
  `Work -> ~/Projects` (or a paste or drop there) fills the disk with
  nested copies until the paths are too long.
- **Fix:** the check also compares the real locations (symlinks resolved
  with `realpath`) of the source folder and the destination's folder. A
  symlink as the source is still allowed (it is copied as a link).
- **Test:** `/fileops/into-itself-through-symlink` (copy and move; fails
  without the fix).

### M4. Cursor keys in a view showing a message moved the focus out of sight

- **Found by:** the monkey, seed 4146712566, at action 193
  (`WIDGET_REALIZED_FOR_EVENT` in `gtk_widget_event`); the new log line
  showed the focus on an unrealized grid hidden behind the view's message.
- **Cause:** a view that cannot show its files (a folder without read
  permission, "Cannot open this folder") shows a message in their place.
  `move_cursor`, `jump_cursor`, `visual_start` and `sfe_pane_set_view`
  (`src/pane.c`) grabbed the focus for the hidden grid or list anyway, so
  after `j`, `k`, `G`, `gg`, `v` or Ctrl+1/2 the keys went to a widget
  nobody sees; when it had never been realized, GTK logged a critical.
- **Fix:** they focus through `sfe_pane_focus_view`, which already picks
  the message when it is showing.
- **Test:** `/ui/keys-in-message-view` (the focus must stay on screen after
  each of those keys; fails without the fix).

### M3. "Open With → Other Application" on a file just gone gave criticals

- **Found by:** the monkey, seed 2096012554, at action 357: Delete
  (to the trash) and then the action, before the file's row had left.
- **Cause:** `run_open_with_other` (`src/actions.c`) opened GTK 3's app
  chooser for the file; the chooser reads the file's type without checking
  that it could (`g_file_info_get_content_type(NULL)`). Also when another
  program deletes a file you have selected.
- **Fix:** the action does nothing for a file that no longer exists.
- **Test:** `/ui/open-with-gone-file` (fails without the fix).

### M2. Closing a split made GTK draw through a destroyed cell area

- **Found by:** the monkey, seed 3809231066, at action 90 (`close-pane`
  after `:select all`); every replay.
- **Cause:** `sfe_pane_dispose` dropped the folder model and then let GTK
  destroy the views, which were still showing the rows: during the
  teardown GTK drew a cell through its already destroyed cell area
  (`gtk_cell_area_apply_attributes: GTK_IS_CELL_AREA`).
- **Fix:** both views let go of the model first thing in dispose (and the
  pane forgets the destroyed views afterwards: dispose can run twice, which
  the first version of the fix tripped over).
- **Test:** `/ui/split-close-repeatedly` (split, select all, close, 20
  times; fails without the fix).

### M1. GLib critical "g_signal_handler_disconnect: handler_id > 0" (fixed)

- **Found by:** the monkey, seed 8, at action 273; for a long time not
  again, then 4 times in about 60 runs of 20000 actions with `-j` (2026-11-22):
  a busy machine makes it likely.
- **Cause:** a view moved to a new place (a split, `:visual swap`) while a
  scroll animation ran. GTK then jumps the view to where the animation was
  heading (`gtk_adjustment_enable_animation()` with the new frame clock),
  which tells sfe the view moved. Up from the empty space after the
  files, sfe set the view back down (`on_scroll_value()`, which keeps that
  space when the view re-measures) from inside that jump; that ends GTK's
  animation, and GTK then ended it a second time, disconnecting handler 0.
- **How it was found:** no symbols exist for this GTK (Artix builds), but
  gdb disables address randomisation, so the address from a crash's
  backtrace could be disassembled at a breakpoint: the code that logs is
  GTK's animation teardown, and a gdb breakpoint on
  `gdk_frame_clock_end_updating` showed sfe's handler ending it first.
  Running monkeys under gdb, 24 at a time, gave the backtraces.
- **Fix:** `pane.c` sets the view back from an idle callback
  (`on_pull_back()`), after GTK's own change, checking again there whether
  it is still needed.
- **Test:** `/ui/split-during-scroll`: in the empty space after 80 files,
  an animated Page Up, then `:split horizontal` before its first frame.
  Fails without the fix (the critical), passes with it.

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
- The monkey ran `:q` while a context menu was open; with the menu holding
  the input that cannot happen (a user's keys go to the menu), and the next
  key reached the closed view's menu (`WIDGET_REALIZED_FOR_EVENT`). While a
  menu or other grab is active, the monkey now only presses keys.
- After `:q` closed the last window, the monkey reopened it and waited for
  files to show; by then its folder was unreadable (the Properties dialog)
  or empty, so it timed out (seed 3419482463, action 1262). Reopening now
  restores the folder's permissions and waits only for the window.
- With `-j`, runs failed with "Unable to read from broadway server":
  two runs could pick the same display, and the second broadwayd (which
  fails to bind only after printing that it listens) could take longer
  than the 200 ms check under load; the run then drew on the other run's
  display and lost it when that run ended. Each display now has a lock
  file, held for the whole run. Since then, about one run in 40 (of
  10000 actions, `-j 24`) still loses its display or dies: the test now
  reports how broadwayd ended, and it was SIGKILL, also to `test-ui`
  itself. Not memory (the kernel's `oom_kill` count is 0 for the whole
  uptime), and strace on 48 runs showed no process of the test sending
  any SIGKILL: something outside the test (not found; tracing other
  processes' signals needs root). `tools/ui-monkey.sh` reports these runs
  as killed from outside and does not count them.
- With broadwayd's errors now in the log: on the ASan build under full
  load (2 of 48 runs of 6000 actions), broadwayd printed "can't write to
  client" twice and then aborted on its own assertion (`client_free`:
  `client->disconnect_idle == 0`), taking the run's display. A broadwayd
  bug; the script reports these runs apart too.

## Round 8 (2026-11-22, 14400 s, all 11 targets with `-p`)

### 11. A TOML escape above U+10FFFF brought back the Settings abort (`config`)

- **Found by:** `fuzz-config` (round trip: the written config did not
  parse, "(line 91) not valid UTF-8"), after 27 minutes.
- **Input:** a string with the escape `\U001FFFF0` (above U+10FFFF, the
  last Unicode code point).
- **Cause:** the vendored tomlc17's `ucs_to_utf8` encoded code points up
  to U+1FFFFF as four bytes; above U+10FFFF those bytes are not UTF-8. The
  file itself is plain ASCII, so sfe's UTF-8 check (finding 1) passed, the
  config loaded, and the first Settings change aborted in
  `sfe_config_copy`, which writes the config out and parses it back — the
  3.2.4 crash by another route.
- **Fix:** `ucs_to_utf8` stops at U+10FFFF (a local change in
  `thirdparty/tomlc17/src/tomlc17.c`, marked "sfe:"), so such an escape is
  a parse error reported with its line, like any other mistake in the file.
- **Test:** `/config/escape-beyond-unicode` (fails without the fix).

Apart from that, all 11 targets ran the full 4 hours without a finding;
config, restarted with the fix, ran another 3.5 hours (21,273 inputs) clean.

## Round 7 (2026-11-22, 7200 s, all 11 targets with `-p`)

Nothing found. Corpora: config 16,653 inputs, thumbnailer 5,487, rename
5,223, cmdline 3,067, portalreq 2,708. Rounds 4 to 7 (90 minutes to 2 hours
each) found only finding 10; the parsers look solid, and the UI monkey is
where new bugs turn up now.

## Round 6 (2026-11-21, 3600 s, all 11 targets with `-p`)

### 10. Turning sfe on could write a portals.conf that nothing can load (`portalconf`)

- **Found by:** `fuzz-portalconf` (invariant: turned on, but the file does
  not use sfe), after 8 minutes.
- **Input:** no user file yet, and a system file whose `[preferred]`
  group (not its first) holds an `Encoding=` key.
- **Cause:** GKeyFile treats a key named `Encoding` as its own and checks
  it only in the first group. The system file loaded, sfe copied its
  `[preferred]` keys into a new file where `[preferred]` is the first
  group, and that file failed to load: in sfe, and in xdg-desktop-portal,
  which reads it with the same parser (file dialogs would stop following
  the file until it was fixed by hand).
- **Fix:** the `Encoding` key is not copied, and `sfe_portal_conf_set`
  now checks that the text loads again before writing it (an error
  instead of a broken file).
- **Test:** `/portalconf/seed-encoding-key`.

Apart from that, all 11 targets ran the full hour without a finding;
portalconf, restarted with the fix, ran another 50 minutes (9.8 million
inputs) clean.

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
