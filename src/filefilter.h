#pragma once

/* File type filters of a file dialog ("Images", "All files"), as the
 * desktop portal sends them: a(sa(us)), each a name and rules, a rule
 * being (0, glob) such as "*.png" or (1, MIME type) such as "image/png"
 * (a type ending in a slash and a star matches the whole family). Globs
 * ignore case. GTK-free and unit-tested. */

#include <glib.h>
#include <stdbool.h>

typedef struct {
    char* name;
    GPtrArray* globs; /* GPatternSpec*, lower case */
    GPtrArray* mime_types; /* char* */
} SfeFileFilter;

void sfe_file_filter_free(gpointer filter);

/* Filters from GVariant text of type a(sa(us)) (SfeFileFilter*, in order);
 * NULL with an error when it cannot be read. */
GPtrArray* sfe_file_filters_parse(const char* text, GError** error);

/* Whether a file named `name` with `content_type` (may be NULL) passes. */
bool sfe_file_filter_matches(const SfeFileFilter* filter, const char* name, const char* content_type);
