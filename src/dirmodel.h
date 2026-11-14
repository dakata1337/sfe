#pragma once

/* The contents of one directory as a sorted GtkListStore, shared by the
 * grid and list views of a pane.
 *
 * The store holds only the files that are shown; every file (hidden ones
 * too) is remembered separately, so toggling hidden files adds or removes
 * rows in place. (A GtkTreeModelFilter is deliberately avoided: its
 * reference counting breaks when shown rows are deleted.)
 *
 * Every load builds a fresh store off-screen and then swaps it in, which
 * keeps big directories fast. Connect to "loaded" and call
 * sfe_dir_model_get_model() to attach the new model to the views. After
 * loading, a GFileMonitor keeps the store in sync with the disk. */

#include "config.h"
#include <gtk/gtk.h>

enum {
    SFE_COL_FILE, /* GFile* */
    SFE_COL_INFO, /* GFileInfo* */
    SFE_COL_NAME, /* display name */
    SFE_COL_ICON, /* cairo_surface_t*, grid size */
    SFE_COL_ICON_SMALL, /* cairo_surface_t*, list size */
    SFE_COL_SIZE_TEXT,
    SFE_COL_MTIME_TEXT,
    SFE_COL_TYPE_TEXT,
    SFE_COL_IS_DIR, /* gboolean */
    SFE_N_COLUMNS,
};

/* Attributes queried for every file. */
#define SFE_FILE_ATTRIBUTES                                                   \
    "standard::name,standard::display-name,standard::type,standard::size,"   \
    "standard::icon,standard::fast-content-type,standard::is-hidden,"        \
    "standard::is-backup,standard::is-symlink,standard::symlink-target,"     \
    "time::modified,access::can-read,access::can-write,unix::mode"

#define SFE_TYPE_DIR_MODEL (sfe_dir_model_get_type())
G_DECLARE_FINAL_TYPE(SfeDirModel, sfe_dir_model, SFE, DIR_MODEL, GObject)

SfeDirModel* sfe_dir_model_new(void);

/* Starts loading `dir`; cancels a load that is still running.
 * Emits "loaded" on success or "error" (GError*) on failure. When the
 * directory itself is deleted or unmounted, "gone" is emitted. Files that
 * appear later are announced with "file-added" (name). */
void sfe_dir_model_load(SfeDirModel* self, GFile* dir);
void sfe_dir_model_reload(SfeDirModel* self);

GFile* sfe_dir_model_get_dir(SfeDirModel* self);

/* The model the views display (owned by the dir model). */
GtkTreeModel* sfe_dir_model_get_model(SfeDirModel* self);

void sfe_dir_model_set_show_hidden(SfeDirModel* self, bool show);
void sfe_dir_model_set_sort(SfeDirModel* self, SfeSortKey key, GtkSortType order, bool dirs_first);
void sfe_dir_model_get_sort(SfeDirModel* self, SfeSortKey* key, GtkSortType* order);

/* Icon sizes in logical pixels, and the widget scale factor. */
void sfe_dir_model_set_icon_sizes(SfeDirModel* self, int grid, int list, int scale);

/* Finds the row of the file named `name` in the displayed model.
 * Returns NULL if it is absent or hidden. Free with gtk_tree_path_free(). */
GtkTreePath* sfe_dir_model_find(SfeDirModel* self, const char* name);

/* The file at a path of the displayed model (new reference). */
GFile* sfe_dir_model_file_at(SfeDirModel* self, GtkTreePath* path);
GFileInfo* sfe_dir_model_info_at(SfeDirModel* self, GtkTreePath* path);
