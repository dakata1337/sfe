#pragma once

/* The left sidebar: places, bookmarks and devices on top, the directory tree
 * below, separated by a draggable divider. */

#include <gtk/gtk.h>

#define SFE_TYPE_SIDEBAR (sfe_sidebar_get_type())
G_DECLARE_FINAL_TYPE(SfeSidebar, sfe_sidebar, SFE, SIDEBAR, GtkPaned)

/* Emits "open-location" (GFile* dir, gboolean new_window). */
GtkWidget* sfe_sidebar_new(void);

/* The active pane's directory: highlighted in the list and the tree. */
void sfe_sidebar_set_location(SfeSidebar* self, GFile* dir);
