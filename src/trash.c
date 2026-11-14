#include "trash.h"

#include <string.h>

static GFile* trash_dir(void)
{
    g_autofree char* path = g_build_filename(g_get_user_data_dir(), "Trash", NULL);
    return g_file_new_for_path(path);
}

GFile* sfe_trash_files_dir(void)
{
    g_autoptr(GFile) trash = trash_dir();
    return g_file_get_child(trash, "files");
}

bool sfe_trash_is_trash_dir(GFile* dir)
{
    g_autoptr(GFile) files = sfe_trash_files_dir();
    return dir != NULL && g_file_equal(dir, files);
}

bool sfe_trash_contains(GFile* file)
{
    g_autoptr(GFile) parent = file ? g_file_get_parent(file) : NULL;
    return parent != NULL && sfe_trash_is_trash_dir(parent);
}

bool sfe_trash_is_empty(void)
{
    g_autoptr(GFile) files = sfe_trash_files_dir();
    g_autoptr(GFileEnumerator) en = g_file_enumerate_children(files, G_FILE_ATTRIBUTE_STANDARD_NAME,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
    g_autoptr(GFileInfo) first = en ? g_file_enumerator_next_file(en, NULL, NULL) : NULL;
    return first == NULL;
}

GFile* sfe_trash_info_file(GFile* item)
{
    g_autoptr(GFile) trash = trash_dir();
    g_autofree char* base = g_file_get_basename(item);
    g_autofree char* name = g_strconcat(base, ".trashinfo", NULL);
    g_autoptr(GFile) info = g_file_get_child(trash, "info");
    return g_file_get_child(info, name);
}

GFile* sfe_trash_original_location(GFile* item, GError** error)
{
    g_autoptr(GFile) info = sfe_trash_info_file(item);
    g_autofree char* path = g_file_get_path(info);
    g_autoptr(GKeyFile) kf = g_key_file_new();

    if (path == NULL || !g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        g_autofree char* name = g_file_get_basename(item);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
            "“%s” has no trash information, so its original location is unknown.", name);
        return NULL;
    }
    /* The value is URI-escaped; relative paths are relative to the
     * trash's top directory (never the case for the home trash). */
    g_autofree char* raw = g_key_file_get_string(kf, "Trash Info", "Path", error);
    if (raw == NULL)
        return NULL;
    g_autofree char* original = g_uri_unescape_string(raw, NULL);
    if (original == NULL || !g_path_is_absolute(original)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Unusable trash information: Path=%s", raw);
        return NULL;
    }
    return g_file_new_for_path(original);
}

bool sfe_trash_restore(GFile* item, GError** error)
{
    g_autoptr(GFile) original = sfe_trash_original_location(item, error);
    if (original == NULL)
        return false;

    if (g_file_query_exists(original, NULL)) {
        g_autofree char* path = g_file_get_parse_name(original);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS, "“%s” already exists.", path);
        return false;
    }
    g_autoptr(GFile) parent = g_file_get_parent(original);
    GError* local = NULL;
    if (parent != NULL && !g_file_make_directory_with_parents(parent, NULL, &local)) {
        if (!g_error_matches(local, G_IO_ERROR, G_IO_ERROR_EXISTS)) {
            g_propagate_error(error, local);
            return false;
        }
        g_clear_error(&local);
    }
    if (!g_file_move(item, original, G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_ALL_METADATA, NULL, NULL, NULL, error))
        return false;
    g_autoptr(GFile) info = sfe_trash_info_file(item);
    g_file_delete(info, NULL, NULL);
    return true;
}

GList* sfe_trash_with_info(GList* items)
{
    GList* out = NULL;
    for (GList* l = items; l != NULL; l = l->next) {
        out = g_list_prepend(out, g_object_ref(l->data));
        if (sfe_trash_contains(l->data)) {
            GFile* info = sfe_trash_info_file(l->data);
            if (g_file_query_exists(info, NULL))
                out = g_list_prepend(out, info);
            else
                g_object_unref(info);
        }
    }
    return g_list_reverse(out);
}

static GList* children(GFile* dir, GList* list)
{
    g_autoptr(GFileEnumerator) en = g_file_enumerate_children(dir, G_FILE_ATTRIBUTE_STANDARD_NAME,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
    GFileInfo* info;
    while (en != NULL && (info = g_file_enumerator_next_file(en, NULL, NULL)) != NULL) {
        list = g_list_prepend(list, g_file_get_child(dir, g_file_info_get_name(info)));
        g_object_unref(info);
    }
    return list;
}

/* Both folders whole, so stray info files go too. */
GList* sfe_trash_everything(void)
{
    g_autoptr(GFile) trash = trash_dir();
    g_autoptr(GFile) files = g_file_get_child(trash, "files");
    g_autoptr(GFile) info = g_file_get_child(trash, "info");
    return g_list_reverse(children(info, children(files, NULL)));
}
