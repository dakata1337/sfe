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

/* Emits "open-location" (GFile*) when the user selects a directory. */
GtkWidget* sfe_dir_tree_new(void);

/* Expands the tree down to `dir` and highlights it. */
void sfe_dir_tree_set_location(SfeDirTree* self, GFile* dir);

void sfe_dir_tree_set_show_hidden(SfeDirTree* self, bool show);
