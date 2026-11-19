#pragma once

/* Rename dialogs: a simple name prompt for one file, and the mass rename
 * dialog (template, find/replace, case, counter, live preview). */

#include <gtk/gtk.h>
#include <stdbool.h>

/* Called with the new names (char*, same order as the files) after a
 * successful rename. */
typedef void (*SfeRenamedFunc)(GPtrArray* new_names, gpointer data);

/* Renames one file through a name prompt. */
void sfe_rename_one(GtkWidget* parent, GFile* file, SfeRenamedFunc done, gpointer data, GDestroyNotify destroy);

/* Which of the files take part in a mass rename. */
typedef enum {
    SFE_RENAME_NO_SWITCH = -1, /* all, and no "Include" switch */
    SFE_RENAME_ALL,
    SFE_RENAME_FILES,
    SFE_RENAME_FOLDERS,
} SfeRenameInclude;

/* Opens the mass rename dialog for `files` (GFile*, in display order).
 * Unless `include` is SFE_RENAME_NO_SWITCH, an "Include: All | Files |
 * Folders" switch (set to `include`) picks which of them take part. */
void sfe_rename_many(GtkWidget* parent, GList* files, SfeRenameInclude include, SfeRenamedFunc done,
    gpointer data, GDestroyNotify destroy);
