#pragma once

/* A lazily loaded tree of directories that follows a location.
 *
 * The root is the place that contains the location: the home directory for
 * anything below it, otherwise the root of the mounted filesystem (a USB
 * drive, or "/"). Directories load when expanded and update live. */

#include <gtk/gtk.h>
#include <stdbool.h>

#define SFE_TYPE_DIR_TREE (sfe_dir_tree_get_type())
G_DECLARE_FINAL_TYPE(SfeDirTree, sfe_dir_tree, SFE, DIR_TREE, GtkTreeView)

/* Where a folder from the sidebar opens. */
typedef enum {
    SFE_OPEN_HERE, /* in the active view */
    SFE_OPEN_NEW_WINDOW,
    SFE_OPEN_SPLIT, /* a new right / right-bottom view */
} SfeOpenWhere;

/* Signals:
 *   "open-location" (GFile*, SfeOpenWhere)   the user selected a directory
 *                                           (middle click: a new window)
 *   "folder-menu" (GFile*, GdkEvent*)        right click / Menu key on a
 *                                           directory (event NULL from the
 *                                           keyboard) */
GtkWidget* sfe_dir_tree_new(void);

/* Expands the tree down to `dir` and highlights it. */
void sfe_dir_tree_set_location(SfeDirTree* self, GFile* dir);

/* Scrolls the highlighted folder to the middle (after the tree changed size). */
void sfe_dir_tree_center(SfeDirTree* self);

void sfe_dir_tree_set_show_hidden(SfeDirTree* self, bool show);

/* The highlighted row, in the tree's window coordinates (to anchor a menu
 * opened from the keyboard); false if none is visible. */
bool sfe_dir_tree_get_selected_area(SfeDirTree* self, GdkRectangle* area);
