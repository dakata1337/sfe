#pragma once

/* Renaming logic shared by the rename dialogs and (later) the command line.
 * GTK-free, so it is unit-tested.
 *
 * A new name is built from a rule:
 *   1. the old name is split into {name} (stem) and {ext} (".jpg", ".tar.gz")
 *   2. find / replace is applied to the stem (plain text or a regex with
 *      \1-style references; case-insensitive unless match_case)
 *   3. the case of the stem is changed
 *   4. the template is expanded: {name}, {ext}, {n} (the counter) and
 *      {n:3} / {n:03} (the counter zero-padded to 3 digits)
 * Renaming a set of files is done in two passes through temporary names, so
 * swaps (a <-> b) and case-only changes on case-insensitive drives work. */

#include <gio/gio.h>
#include <stdbool.h>

typedef enum {
    SFE_CASE_KEEP,
    SFE_CASE_LOWER,
    SFE_CASE_UPPER,
    SFE_CASE_TITLE,
} SfeCaseMode;

typedef struct {
    const char* template_; /* NULL or "" means "{name}{ext}" */
    const char* find; /* NULL or "" replaces nothing */
    const char* replace;
    bool regex;
    bool match_case;
    SfeCaseMode case_mode;
    int counter_start;
    int counter_step;
} SfeRenameRule;

typedef struct SfeRenamer SfeRenamer;

/* Compiles a rule; fails only for an invalid regular expression. */
SfeRenamer* sfe_renamer_new(const SfeRenameRule* rule, GError** error);
void sfe_renamer_free(SfeRenamer* renamer);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(SfeRenamer, sfe_renamer_free)

/* The new name of the `index`-th file (0-based, for the counter). */
char* sfe_renamer_apply(SfeRenamer* renamer, const char* name, bool is_dir, int index);

typedef enum {
    SFE_RENAME_OK,
    SFE_RENAME_UNCHANGED,
    SFE_RENAME_EMPTY,
    SFE_RENAME_INVALID, /* contains '/', or is "." / ".." */
    SFE_RENAME_DUPLICATE, /* two files would get the same name */
    SFE_RENAME_EXISTS, /* another file already has that name */
} SfeRenameStatus;

/* Checks the planned renames (`files[i]` -> `names[i]`, in the same folder)
 * and fills `status`. Returns the number of problems (not counting
 * unchanged names). */
int sfe_rename_check(GFile** files, char** names, int n, SfeRenameStatus* status);

/* Renames the files. On failure, files already renamed are moved back. */
bool sfe_rename_files(GFile** files, char** names, int n, GError** error);

/* Short human-readable text for a status. */
const char* sfe_rename_status_text(SfeRenameStatus status);
