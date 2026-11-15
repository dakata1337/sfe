#define _GNU_SOURCE /* FNM_CASEFOLD */
#include "filefilter.h"

#include <fnmatch.h>
#include <gio/gio.h>
#include <string.h>

void sfe_file_filter_free(gpointer p)
{
    SfeFileFilter* f = p;
    if (f == NULL)
        return;
    g_free(f->name);
    g_ptr_array_unref(f->globs);
    g_ptr_array_unref(f->mime_types);
    g_free(f);
}

GPtrArray* sfe_file_filters_parse(const char* text, GError** error)
{
    g_autoptr(GVariant) v = g_variant_parse(G_VARIANT_TYPE("a(sa(us))"), text, NULL, NULL, error);
    if (v == NULL)
        return NULL;
    GPtrArray* filters = g_ptr_array_new_with_free_func(sfe_file_filter_free);
    GVariantIter iter;
    const char* name;
    GVariantIter* rules;
    g_variant_iter_init(&iter, v);
    while (g_variant_iter_next(&iter, "(&sa(us))", &name, &rules)) {
        SfeFileFilter* f = g_new0(SfeFileFilter, 1);
        f->name = g_strdup(name);
        f->globs = g_ptr_array_new_with_free_func(g_free);
        f->mime_types = g_ptr_array_new_with_free_func(g_free);
        guint32 kind;
        const char* rule;
        while (g_variant_iter_next(rules, "(u&s)", &kind, &rule)) {
            if (kind == 0) {
                g_ptr_array_add(f->globs, g_strdup(rule));
            } else {
                g_ptr_array_add(f->mime_types, g_strdup(rule));
            }
        }
        g_variant_iter_free(rules);
        g_ptr_array_add(filters, f);
    }
    return filters;
}

static bool mime_matches(const char* pattern, const char* content_type)
{
    g_autofree char* mime = g_content_type_get_mime_type(content_type);
    if (mime == NULL)
        return false;
    if (g_str_has_suffix(pattern, "/*")) /* a whole family, e.g. every image type */
        return strncmp(mime, pattern, strlen(pattern) - 1) == 0;
    return strcmp(mime, pattern) == 0 || g_content_type_is_a(content_type, pattern);
}

bool sfe_file_filter_matches(const SfeFileFilter* filter, const char* name, const char* content_type)
{
    /* fnmatch, not GPatternSpec: programs send character classes
     * ("*.[pP][nN][gG]", as the portal spec suggests for any case). */
    for (guint i = 0; name != NULL && i < filter->globs->len; i++) {
        if (fnmatch(g_ptr_array_index(filter->globs, i), name, FNM_CASEFOLD) == 0)
            return true;
    }
    for (guint i = 0; content_type != NULL && i < filter->mime_types->len; i++) {
        if (mime_matches(g_ptr_array_index(filter->mime_types, i), content_type))
            return true;
    }
    return false;
}
