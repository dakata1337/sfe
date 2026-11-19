#include "portalreq.h"

#include <stdbool.h>
#include <string.h>

static char* option_bytestring(GVariant* options, const char* key)
{
    g_autoptr(GVariant) v = g_variant_lookup_value(options, key, G_VARIANT_TYPE_BYTESTRING);
    return v ? g_strdup(g_variant_get_bytestring(v)) : NULL;
}

static char* option_string(GVariant* options, const char* key)
{
    const char* s = NULL;
    return g_variant_lookup(options, key, "&s", &s) ? g_strdup(s) : NULL;
}

static bool option_bool(GVariant* options, const char* key)
{
    gboolean b = FALSE;
    return g_variant_lookup(options, key, "b", &b) && b;
}

static void add(GPtrArray* args, const char* option, const char* value)
{
    if (value == NULL || *value == '\0')
        return;
    g_ptr_array_add(args, g_strdup(option));
    g_ptr_array_add(args, g_strdup(value));
}

/* The filters to offer, and the index of the one to start with (-1: the first). */
static GVariant* offered_filters(GVariant* options, int* start)
{
    g_autoptr(GVariant) asked = g_variant_lookup_value(options, "filters", G_VARIANT_TYPE("a(sa(us))"));
    g_autoptr(GVariant) current = g_variant_lookup_value(options, "current_filter", G_VARIANT_TYPE("(sa(us))"));
    GVariantBuilder list;
    g_variant_builder_init(&list, G_VARIANT_TYPE("a(sa(us))"));
    gsize n = 0;
    *start = -1;
    for (gsize i = 0; asked != NULL && i < g_variant_n_children(asked); i++, n++) {
        g_autoptr(GVariant) f = g_variant_get_child_value(asked, i);
        if (*start < 0 && current != NULL && g_variant_equal(f, current))
            *start = (int)i;
        g_variant_builder_add_value(&list, f);
    }
    if (current != NULL && *start < 0) { /* not in the list, or no list: the program wants it */
        g_variant_builder_add_value(&list, current);
        *start = (int)n++;
    }
    GVariant* filters = g_variant_ref_sink(g_variant_builder_end(&list));
    if (n == 0)
        g_clear_pointer(&filters, g_variant_unref);
    return filters;
}

SfePortalRequest* sfe_portal_request_new(const char* method, const char* parent_window, const char* title,
    GVariant* options)
{
    SfePortalRequest* r = g_new0(SfePortalRequest, 1);
    r->args = g_ptr_array_new_with_free_func(g_free);
    g_autofree char* folder = option_bytestring(options, "current_folder");
    g_autofree char* name = NULL;
    bool folder_dialog = false;

    g_ptr_array_add(r->args, g_strdup("--choose"));
    if (g_strcmp0(method, "SaveFile") == 0) {
        g_ptr_array_add(r->args, g_strdup("save"));
        name = option_string(options, "current_name");
        g_autofree char* current = option_bytestring(options, "current_file"); /* replacing a file */
        if (current != NULL && *current != '\0') {
            g_free(folder);
            folder = g_path_get_dirname(current);
            g_free(name);
            name = g_path_get_basename(current);
        }
    } else if (g_strcmp0(method, "SaveFiles") == 0) { /* a folder for several files */
        g_ptr_array_add(r->args, g_strdup("folder"));
        folder_dialog = true;
        r->save_names = g_ptr_array_new_with_free_func(g_free);
        g_autoptr(GVariant) files = g_variant_lookup_value(options, "files", G_VARIANT_TYPE_BYTESTRING_ARRAY);
        for (gsize i = 0; files != NULL && i < g_variant_n_children(files); i++) {
            g_autoptr(GVariant) f = g_variant_get_child_value(files, i);
            g_ptr_array_add(r->save_names, g_path_get_basename(g_variant_get_bytestring(f)));
        }
    } else { /* OpenFile */
        folder_dialog = option_bool(options, "directory");
        g_ptr_array_add(r->args, g_strdup(folder_dialog ? "folder" : "open"));
        if (!folder_dialog && option_bool(options, "multiple"))
            g_ptr_array_add(r->args, g_strdup("--multiple"));
    }
    add(r->args, "--title", title);
    add(r->args, "--folder", folder);
    add(r->args, "--name", name);
    add(r->args, "--parent", parent_window); /* "wayland:HANDLE" / "x11:XID": sfe attaches to it */

    int start = -1;
    r->filters = folder_dialog ? NULL : offered_filters(options, &start);
    if (r->filters != NULL) {
        g_ptr_array_add(r->args, g_strdup("--filters"));
        g_ptr_array_add(r->args, g_variant_print(r->filters, TRUE));
        if (start >= 0) {
            g_ptr_array_add(r->args, g_strdup("--current-filter"));
            g_ptr_array_add(r->args, g_strdup_printf("%d", start));
        }
    }
    /* Extra options (check boxes, drop-downs), answered as "choices". */
    g_autoptr(GVariant) choices = g_variant_lookup_value(options, "choices", G_VARIANT_TYPE("a(ssa(ss)s)"));
    if (choices != NULL && g_variant_n_children(choices) > 0) {
        g_ptr_array_add(r->args, g_strdup("--choices"));
        g_ptr_array_add(r->args, g_variant_print(choices, TRUE));
    }
    g_autofree char* accept = option_string(options, "accept_label");
    add(r->args, "--accept-label", accept);
    return r;
}

void sfe_portal_request_free(SfePortalRequest* r)
{
    if (r == NULL)
        return;
    g_ptr_array_unref(r->args);
    g_clear_pointer(&r->filters, g_variant_unref);
    g_clear_pointer(&r->save_names, g_ptr_array_unref);
    g_free(r);
}

/* `name` in `folder`, or "name (2).ext", "name (3).ext" ... when that exists
 * or is in `taken` (names already handed out; the result is added). */
static GFile* free_child(GFile* folder, const char* name, GHashTable* taken)
{
    const char* dot = strrchr(name, '.');
    if (dot == name)
        dot = NULL; /* ".bashrc": no extension */
    g_autofree char* stem = dot ? g_strndup(name, dot - name) : g_strdup(name);
    const char* ext = dot ? dot : "";
    for (int n = 1;; n++) {
        g_autofree char* candidate = n == 1 ? g_strdup(name) : g_strdup_printf("%s (%d)%s", stem, n, ext);
        GFile* file = g_file_get_child(folder, candidate);
        if (!g_hash_table_contains(taken, candidate) && !g_file_query_exists(file, NULL)) {
            g_hash_table_add(taken, g_steal_pointer(&candidate));
            return file;
        }
        g_object_unref(file);
    }
}

GVariant* sfe_portal_request_results(const SfePortalRequest* r, const char* output)
{
    GVariantBuilder uris, choices;
    g_variant_builder_init(&uris, G_VARIANT_TYPE_STRING_ARRAY);
    g_variant_builder_init(&choices, G_VARIANT_TYPE("a(ss)"));
    bool any_choice = false;
    int filter = -1;
    g_auto(GStrv) lines = g_strsplit(output ? output : "", "\n", -1);
    for (int i = 0; lines[i] != NULL; i++) {
        if (*lines[i] == '\0')
            continue;
        if (g_str_has_prefix(lines[i], "#filter ")) { /* the filter in use */
            filter = (int)g_ascii_strtoll(lines[i] + strlen("#filter "), NULL, 10);
            continue;
        }
        if (g_str_has_prefix(lines[i], "#choice ")) { /* "#choice ID VALUE" */
            g_auto(GStrv) parts = g_strsplit(lines[i] + strlen("#choice "), " ", 2);
            if (parts[0] != NULL) {
                g_variant_builder_add(&choices, "(ss)", parts[0], parts[1] ? parts[1] : "");
                any_choice = true;
            }
            continue;
        }
        if (lines[i][0] == '#')
            continue;
        if (r->save_names == NULL) {
            g_variant_builder_add(&uris, "s", lines[i]);
            continue;
        }
        /* SaveFiles: the user chose a folder; each file goes into it, under a
         * free name: the program writes without asking, so an existing file
         * (or an earlier one of the same name) gets "name (2).ext". */
        g_autoptr(GFile) folder = g_file_new_for_uri(lines[i]);
        g_autoptr(GHashTable) taken = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        for (guint k = 0; k < r->save_names->len; k++) {
            g_autoptr(GFile) file = free_child(folder, g_ptr_array_index(r->save_names, k), taken);
            g_autofree char* uri = g_file_get_uri(file);
            g_variant_builder_add(&uris, "s", uri);
        }
        break;
    }
    GVariantBuilder results;
    g_variant_builder_init(&results, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&results, "{sv}", "uris", g_variant_builder_end(&uris));
    GVariant* choice_list = g_variant_builder_end(&choices);
    if (any_choice)
        g_variant_builder_add(&results, "{sv}", "choices", choice_list);
    else
        g_variant_unref(g_variant_ref_sink(choice_list));
    if (r->filters != NULL && filter >= 0 && filter < (int)g_variant_n_children(r->filters))
        g_variant_builder_add(&results, "{sv}", "current_filter", g_variant_get_child_value(r->filters, filter));
    return g_variant_builder_end(&results);
}
