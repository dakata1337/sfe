#pragma once

/* Bookmarks: sfe's own list (config [sidebar] bookmarks, editable) followed
 * by the GTK bookmarks file (~/.config/gtk-3.0/bookmarks, read-only), which
 * other file managers and file choosers share. */

#include "config.h"
#include <gio/gio.h>

typedef struct {
    GFile* file;
    char* label;
    int config_index; /* index into cfg->bookmarks, or -1 for a GTK bookmark */
} SfeBookmark;

/* All bookmarks to show, sfe's first. Duplicates are dropped.
 * `gtk_path` is the GTK bookmarks file, or NULL for the default location. */
GPtrArray* sfe_bookmarks_list(const SfeConfig* cfg, const char* gtk_path);

/* Index of `dir` in cfg->bookmarks, or -1. */
int sfe_bookmarks_find(const SfeConfig* cfg, GFile* dir);

/* Editing helpers; the caller saves the config afterwards. */
void sfe_bookmarks_add(SfeConfig* cfg, GFile* dir);
void sfe_bookmarks_remove(SfeConfig* cfg, int index);
void sfe_bookmarks_move(SfeConfig* cfg, int from, int to);
