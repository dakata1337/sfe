#include "trash.h"

#include <gio/gunixmounts.h>
#include <string.h>
#include <unistd.h>

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

static bool dir_is_empty(GFile* dir)
{
    g_autoptr(GFileEnumerator) en = g_file_enumerate_children(dir, G_FILE_ATTRIBUTE_STANDARD_NAME,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
    g_autoptr(GFileInfo) first = en ? g_file_enumerator_next_file(en, NULL, NULL) : NULL;
    return first == NULL;
}

/* A drive's trash folder, as the spec allows it: .Trash/UID only when
 * .Trash is a real folder with the sticky bit (not a link someone made). */
static void add_drive_trashes(GList** out, const char* top)
{
    g_autofree char* shared = g_build_filename(top, ".Trash", NULL);
    g_autoptr(GFile) shared_dir = g_file_new_for_path(shared);
    g_autoptr(GFileInfo) st = g_file_query_info(shared_dir, G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_UNIX_MODE,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
    if (st != NULL && g_file_info_get_file_type(st) == G_FILE_TYPE_DIRECTORY
        && (g_file_info_get_attribute_uint32(st, G_FILE_ATTRIBUTE_UNIX_MODE) & 01000)) { /* sticky */
        g_autofree char* uid = g_strdup_printf("%u", (unsigned)getuid());
        g_autofree char* mine = g_build_filename(shared, uid, NULL);
        if (g_file_test(mine, G_FILE_TEST_IS_DIR) && !g_file_test(mine, G_FILE_TEST_IS_SYMLINK))
            *out = g_list_append(*out, g_file_new_for_path(mine));
    }
    g_autofree char* own_name = g_strdup_printf(".Trash-%u", (unsigned)getuid());
    g_autofree char* own = g_build_filename(top, own_name, NULL);
    if (g_file_test(own, G_FILE_TEST_IS_DIR) && !g_file_test(own, G_FILE_TEST_IS_SYMLINK))
        *out = g_list_append(*out, g_file_new_for_path(own));
}

GList* sfe_trash_dirs(void)
{
    GList* out = g_list_append(NULL, trash_dir());
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS /* renamed in GLib 2.84; the old names work on every version sfe takes */
    GList* mounts = g_unix_mounts_get(NULL);
    for (GList* l = mounts; l != NULL; l = l->next) {
        if (!g_unix_mount_is_system_internal(l->data))
            add_drive_trashes(&out, g_unix_mount_get_mount_path(l->data));
    }
    g_list_free_full(mounts, (GDestroyNotify)g_unix_mount_free);
    G_GNUC_END_IGNORE_DEPRECATIONS
    return out;
}

bool sfe_trash_is_empty(void)
{
    GList* dirs = sfe_trash_dirs();
    bool empty = true;
    for (GList* l = dirs; l != NULL && empty; l = l->next) {
        g_autoptr(GFile) files = g_file_get_child(l->data, "files");
        empty = dir_is_empty(files);
    }
    g_list_free_full(dirs, g_object_unref);
    return empty;
}

/* The trash folder an item is in: the parent of its files/ folder. */
static GFile* trash_of(GFile* item)
{
    g_autoptr(GFile) files = g_file_get_parent(item);
    GFile* trash = files ? g_file_get_parent(files) : NULL;
    return trash ? trash : trash_dir();
}

GFile* sfe_trash_info_file(GFile* item)
{
    g_autoptr(GFile) trash = trash_of(item);
    g_autofree char* base = g_file_get_basename(item);
    g_autofree char* name = g_strconcat(base, ".trashinfo", NULL);
    g_autoptr(GFile) info = g_file_get_child(trash, "info");
    return g_file_get_child(info, name);
}

/* The top folder of a drive's trash an item is in, or NULL (the home
 * trash, or not a trash): .Trash-UID's parent, or .Trash/UID's. */
static GFile* top_of(GFile* item)
{
    g_autoptr(GFile) trash = trash_of(item);
    g_autoptr(GFile) home = trash_dir();
    g_autofree char* name = g_file_get_basename(trash);
    g_autofree char* own = g_strdup_printf(".Trash-%u", (unsigned)getuid());
    g_autofree char* uid = g_strdup_printf("%u", (unsigned)getuid());
    if (g_file_equal(trash, home))
        return NULL;
    if (g_strcmp0(name, own) == 0)
        return g_file_get_parent(trash);
    g_autoptr(GFile) shared = g_file_get_parent(trash);
    g_autofree char* shared_name = shared ? g_file_get_basename(shared) : NULL;
    if (g_strcmp0(name, uid) == 0 && g_strcmp0(shared_name, ".Trash") == 0)
        return g_file_get_parent(shared);
    return NULL;
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
     * trash's top directory: a drive's trash (.Trash-UID or .Trash/UID)
     * names its files from the drive's top folder. */
    g_autofree char* raw = g_key_file_get_string(kf, "Trash Info", "Path", error);
    if (raw == NULL)
        return NULL;
    g_autofree char* original = g_uri_unescape_string(raw, NULL);
    g_autoptr(GFile) top = original && !g_path_is_absolute(original) ? top_of(item) : NULL;
    GFile* location = original == NULL ? NULL
        : top != NULL                    ? g_file_resolve_relative_path(top, original)
        : g_path_is_absolute(original)   ? g_file_new_for_path(original)
                                         : NULL;
    if (location == NULL || (top != NULL && !g_file_has_prefix(location, top))) { /* (no way out with "..") */
        g_clear_object(&location);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Unusable trash information: Path=%s", raw);
    }
    return location;
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

/* Both folders whole, so stray info files go too; every drive's trash. */
GList* sfe_trash_everything(void)
{
    GList* dirs = sfe_trash_dirs();
    GList* out = NULL;
    for (GList* l = dirs; l != NULL; l = l->next) {
        g_autoptr(GFile) files = g_file_get_child(l->data, "files");
        g_autoptr(GFile) info = g_file_get_child(l->data, "info");
        out = children(info, children(files, out));
    }
    g_list_free_full(dirs, g_object_unref);
    return g_list_reverse(out);
}

GHashTable* sfe_trash_index(GList* dirs)
{
    GHashTable* found = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
    g_autoptr(GHashTable) dates = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    for (GList* l = dirs; l != NULL; l = l->next) {
        g_autoptr(GFile) files = g_file_get_child(l->data, "files");
        g_autoptr(GFileEnumerator) en = g_file_enumerate_children(files, G_FILE_ATTRIBUTE_STANDARD_NAME,
            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
        GFileInfo* info;
        while (en != NULL && (info = g_file_enumerator_next_file(en, NULL, NULL)) != NULL) {
            g_autoptr(GFile) item = g_file_get_child(files, g_file_info_get_name(info));
            g_object_unref(info);
            g_autoptr(GFile) from = sfe_trash_original_location(item, NULL);
            g_autofree char* path = from ? g_file_get_path(from) : NULL;
            if (path == NULL)
                continue;
            /* DeletionDate=YYYY-MM-DDThh:mm:ss compares as text: the newest wins */
            g_autoptr(GFile) info_file = sfe_trash_info_file(item);
            g_autofree char* info_path = g_file_get_path(info_file);
            g_autoptr(GKeyFile) kf = g_key_file_new();
            char* date = g_key_file_load_from_file(kf, info_path, G_KEY_FILE_NONE, NULL)
                ? g_key_file_get_string(kf, "Trash Info", "DeletionDate", NULL)
                : NULL;
            const char* best = g_hash_table_lookup(dates, path);
            if (g_hash_table_contains(found, path) && g_strcmp0(date, best) < 0) {
                g_free(date);
                continue;
            }
            g_hash_table_replace(found, g_strdup(path), g_object_ref(item));
            g_hash_table_replace(dates, g_strdup(path), date);
        }
    }
    return found;
}
