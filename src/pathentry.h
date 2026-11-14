#pragma once

/* A GtkEntry for typing paths, with directory completion (Tab accepts the
 * inline suggestion). Listen to GtkEntry::activate to navigate. */

#include <gtk/gtk.h>

GtkWidget* sfe_path_entry_new(void);

/* Shows `dir` as text, discarding anything the user typed. */
void sfe_path_entry_set_dir(GtkWidget* entry, GFile* dir);

/* The typed text with "~" expanded (newly allocated). */
char* sfe_path_entry_get_path(GtkWidget* entry);

/* Marks the text as an invalid location until it is edited again. */
void sfe_path_entry_set_error(GtkWidget* entry, gboolean error);
