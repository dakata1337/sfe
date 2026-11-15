#pragma once

/* File type filters of a file dialog ("Images", "All files"), as the
 * desktop portal sends them: a(sa(us)), each a name and rules, a rule
 * being (0, glob) such as "*.png" or (1, MIME type) such as "image/png"
 * (a type ending in a slash and a star matches the whole family). Globs
 * are shell patterns ("*.[pP][nN][gG]" works too) and ignore case.
 * GTK-free and unit-tested. */

#include <glib.h>
#include <stdbool.h>

typedef struct {
    char* name;
    GPtrArray* globs; /* char*, fnmatch patterns */
    GPtrArray* mime_types; /* char* */
    bool folders_only; /* a folder dialog: no files at all */
} SfeFileFilter;

/* The filter of a folder dialog: folders only. */
SfeFileFilter* sfe_file_filter_new_folders_only(void);

void sfe_file_filter_free(gpointer filter);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(SfeFileFilter, sfe_file_filter_free)

/* Filters from GVariant text of type a(sa(us)) (SfeFileFilter*, in order);
 * NULL with an error when it cannot be read. */
GPtrArray* sfe_file_filters_parse(const char* text, GError** error);

/* Whether a file named `name` with `content_type` (may be NULL) passes. */
bool sfe_file_filter_matches(const SfeFileFilter* filter, const char* name, const char* content_type);

/* Whether a dialog with `filter` (NULL: none) lists this entry: folders
 * always (to walk into), files when they match and the dialog is not a
 * folder dialog. */
bool sfe_file_filter_shows(const SfeFileFilter* filter, const char* name, const char* content_type, bool is_dir);
