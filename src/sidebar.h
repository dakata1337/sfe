#pragma once

/* The left sidebar: places, bookmarks and devices on top, the directory tree
 * below, separated by a draggable divider. */

#include "dirtree.h"
#include <gtk/gtk.h>
#include <stdbool.h>

#define SFE_TYPE_SIDEBAR (sfe_sidebar_get_type())
G_DECLARE_FINAL_TYPE(SfeSidebar, sfe_sidebar, SFE, SIDEBAR, GtkPaned)

/* Emits "open-location" (GFile* dir, SfeOpenWhere where). */
GtkWidget* sfe_sidebar_new(void);

/* The active pane's directory: highlighted in the list and the tree. */
void sfe_sidebar_set_location(SfeSidebar* self, GFile* dir);

/* Lets the folder tree fill the sidebar, or brings the places back with the
 * divider where it was (the button in the tree's top bar). */
void sfe_sidebar_toggle_tree(SfeSidebar* self);
void sfe_sidebar_set_tree_full(SfeSidebar* self, bool full);
bool sfe_sidebar_get_tree_full(SfeSidebar* self);
