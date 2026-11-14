#pragma once

/* The properties dialog: name, type, location, size (counted in the
 * background for folders), dates, owner and editable permissions. */

#include <gtk/gtk.h>

/* Shows the properties of `files` (GFile*): one file, or a summary of
 * several. `parent` is any widget of the owning window. */
void sfe_show_properties(GtkWidget* parent, GList* files);
