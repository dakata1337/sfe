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

/* Opens the mass rename dialog for `files` (GFile*, in display order).
 * With `include_switch` (renaming a whole folder), an "Include: All |
 * Files | Folders" switch picks which of them take part. */
void sfe_rename_many(GtkWidget* parent, GList* files, bool include_switch, SfeRenamedFunc done,
    gpointer data, GDestroyNotify destroy);
