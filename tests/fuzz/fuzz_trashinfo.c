/* .trashinfo files in the home trash, which any program can write. Input:
 * the info file of one trashed item. Only reading is fuzzed:
 * sfe_trash_restore() would move files to whatever Path= says. */
#include "common.h"
#include "trash.h"

static GFile* item;
static char* info_path;

int LLVMFuzzerInitialize(int* argc, char*** argv)
{
    (void)argc, (void)argv;
    fuzz_init();
    const char* tmp = fuzz_tmpdir(); /* XDG_DATA_HOME: the trash is in here */
    char* files = g_build_filename(tmp, "Trash", "files", NULL);
    char* info = g_build_filename(tmp, "Trash", "info", NULL);
    g_mkdir_with_parents(files, 0700);
    g_mkdir_with_parents(info, 0700);
    char* path = g_build_filename(files, "item.txt", NULL);
    g_file_set_contents(path, "x", 1, NULL);
    item = g_file_new_for_path(path);
    info_path = g_build_filename(info, "item.txt.trashinfo", NULL);
    g_free(path);
    g_free(info);
    g_free(files);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    fuzz_write(info_path, data, size);
    GFile* original = sfe_trash_original_location(item, NULL);
    if (original != NULL) {
        char* path = g_file_get_path(original);
        FUZZ_CHECK(path != NULL && g_path_is_absolute(path), "original location is not an absolute path");
        GList* dirs = sfe_trash_dirs();
        GHashTable* index = sfe_trash_index(dirs);
        GFile* found = g_hash_table_lookup(index, path);
        FUZZ_CHECK(found != NULL && g_file_equal(found, item), "the item is not found by its own original location");
        g_hash_table_unref(index);
        g_list_free_full(dirs, g_object_unref);
        g_free(path);
        g_object_unref(original);
    }
    g_list_free_full(sfe_trash_everything(), g_object_unref);
    return 0;
}
