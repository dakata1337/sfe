#include "bookmarks.h"

#include "util.h"
#include <string.h>

static void bookmark_free(gpointer p)
{
    SfeBookmark* b = p;
    g_object_unref(b->file);
    g_free(b->label);
    g_free(b);
}

static GFile* config_entry_file(const char* entry)
{
    g_autofree char* path = sfe_expand_home(entry);
    return g_file_new_for_path(path);
}

static bool already_listed(GPtrArray* list, GFile* file)
{
    for (guint i = 0; i < list->len; i++) {
        SfeBookmark* b = g_ptr_array_index(list, i);
        if (g_file_equal(b->file, file))
            return true;
    }
    return false;
}

static void add_entry(GPtrArray* list, GFile* file, const char* label, int config_index)
{
    if (already_listed(list, file))
        return;

    SfeBookmark* b = g_new0(SfeBookmark, 1);
    b->file = g_object_ref(file);
    b->config_index = config_index;
    if (label != NULL && *label != '\0') {
        b->label = g_utf8_make_valid(label, -1); /* other programs write the GTK file; GTK needs UTF-8 */
    } else {
        g_autofree char* path = g_file_get_path(file);
        b->label = g_filename_display_basename(path ? path : "/");
    }
    g_ptr_array_add(list, b);
}

/* Lines look like "file:///home/me/Projects Optional Label". Only local
 * locations are kept: remote ones need gvfs, which sfe does not depend on. */
static void add_gtk_bookmarks(GPtrArray* list, const char* path)
{
    g_autofree char* text = NULL;
    if (!g_file_get_contents(path, &text, NULL, NULL))
        return;

    g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
    for (char** line = lines; *line != NULL; line++) {
        g_strstrip(*line);
        if (**line == '\0')
            continue;

        char* space = strchr(*line, ' ');
        const char* label = NULL;
        if (space != NULL) {
            *space = '\0';
            label = space + 1;
        }
        g_autoptr(GFile) file = g_file_new_for_uri(*line);
        if (g_file_is_native(file))
            add_entry(list, file, label, -1);
    }
}

GPtrArray* sfe_bookmarks_list(const SfeConfig* cfg, const char* gtk_path)
{
    GPtrArray* list = g_ptr_array_new_with_free_func(bookmark_free);

    for (guint i = 0; i < cfg->bookmarks->len; i++) {
        g_autoptr(GFile) file = config_entry_file(g_ptr_array_index(cfg->bookmarks, i));
        add_entry(list, file, NULL, (int)i);
    }

    if (cfg->show_gtk_bookmarks) {
        g_autofree char* fallback = g_build_filename(g_get_user_config_dir(), "gtk-3.0", "bookmarks", NULL);
        add_gtk_bookmarks(list, gtk_path ? gtk_path : fallback);
    }
    return list;
}

int sfe_bookmarks_find(const SfeConfig* cfg, GFile* dir)
{
    for (guint i = 0; i < cfg->bookmarks->len; i++) {
        g_autoptr(GFile) file = config_entry_file(g_ptr_array_index(cfg->bookmarks, i));
        if (g_file_equal(file, dir))
            return (int)i;
    }
    return -1;
}

void sfe_bookmarks_add(SfeConfig* cfg, GFile* dir)
{
    g_autofree char* path = g_file_get_path(dir);
    if (path != NULL && sfe_bookmarks_find(cfg, dir) < 0)
        g_ptr_array_add(cfg->bookmarks, sfe_contract_home(path));
}

void sfe_bookmarks_remove(SfeConfig* cfg, int index)
{
    if (index >= 0 && (guint)index < cfg->bookmarks->len)
        g_ptr_array_remove_index(cfg->bookmarks, index);
}

void sfe_bookmarks_move(SfeConfig* cfg, int from, int to)
{
    int n = (int)cfg->bookmarks->len;
    if (from < 0 || from >= n || to < 0 || to >= n || from == to)
        return;

    char* entry = g_ptr_array_steal_index(cfg->bookmarks, from);
    g_ptr_array_insert(cfg->bookmarks, to, entry);
}
