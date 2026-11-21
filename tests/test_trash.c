#include "trash.h"

#include <glib/gstdio.h>
#include <string.h>

static char* root; /* a temporary XDG_DATA_HOME, so the real trash is never touched */

static GFile* make_file(const char* rel, const char* text)
{
    g_autofree char* path = g_build_filename(root, rel, NULL);
    g_autofree char* dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_file_set_contents(path, text, -1, NULL);
    return g_file_new_for_path(path);
}

/* g_file_trash() puts it into $XDG_DATA_HOME/Trash (same filesystem). */
static GFile* trash(GFile* file)
{
    g_autoptr(GError) error = NULL;
    g_autofree char* name = g_file_get_basename(file);
    g_assert_true(g_file_trash(file, NULL, &error));
    g_assert_no_error(error);
    g_autoptr(GFile) files = sfe_trash_files_dir();
    return g_file_get_child(files, name);
}

static void test_restore(void)
{
    g_assert_true(sfe_trash_is_empty());

    g_autoptr(GFile) file = make_file("docs/sub/notes.txt", "hello");
    g_autoptr(GFile) item = trash(file);
    g_assert_false(g_file_query_exists(file, NULL));
    g_assert_true(g_file_query_exists(item, NULL));
    g_assert_true(sfe_trash_contains(item));
    g_assert_false(sfe_trash_contains(file));
    g_assert_false(sfe_trash_is_empty());

    g_autoptr(GFile) dir = sfe_trash_files_dir();
    g_assert_true(sfe_trash_is_trash_dir(dir));

    g_autoptr(GFile) original = sfe_trash_original_location(item, NULL);
    g_assert_true(g_file_equal(original, file));

    /* Its folder is gone meanwhile: restoring recreates it. */
    g_autofree char* sub = g_build_filename(root, "docs", "sub", NULL);
    g_assert_cmpint(g_rmdir(sub), ==, 0);
    g_autoptr(GError) error = NULL;
    g_assert_true(sfe_trash_restore(item, &error));
    g_assert_no_error(error);
    g_assert_true(g_file_query_exists(file, NULL));
    g_autoptr(GFile) info = sfe_trash_info_file(item);
    g_assert_false(g_file_query_exists(info, NULL));
    g_assert_true(sfe_trash_is_empty());

    /* Never over something that took its place. */
    g_autoptr(GFile) again = trash(file);
    g_autoptr(GFile) newer = make_file("docs/sub/notes.txt", "newer");
    g_assert_false(sfe_trash_restore(again, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS);
    g_assert_true(g_file_query_exists(again, NULL));
}

static void test_delete_lists(void)
{
    g_autoptr(GFile) a = make_file("x/a.txt", "a");
    g_autoptr(GFile) b = make_file("x/b.txt", "b");
    g_autoptr(GFile) ta = trash(a);
    g_autoptr(GFile) tb = trash(b);

    /* Deleting from the trash takes the info files along. */
    GList one = { .data = ta };
    GList* with = sfe_trash_with_info(&one);
    g_assert_cmpuint(g_list_length(with), ==, 2);
    g_autoptr(GFile) info = sfe_trash_info_file(ta);
    g_assert_true(g_file_equal(with->next->data, info));
    g_list_free_full(with, g_object_unref);

    /* Outside the trash nothing is added. */
    GList plain = { .data = a };
    GList* same = sfe_trash_with_info(&plain);
    g_assert_cmpuint(g_list_length(same), ==, 1);
    g_list_free_full(same, g_object_unref);

    /* Emptying: everything in files/ and info/ (at least these two each). */
    GList* all = sfe_trash_everything();
    g_assert_cmpuint(g_list_length(all), >=, 4);
    for (GList* l = all; l != NULL; l = l->next)
        g_assert_true(g_file_delete(l->data, NULL, NULL));
    g_list_free_full(all, g_object_unref);
    g_assert_true(sfe_trash_is_empty());
}

int main(int argc, char** argv)
{
    root = g_dir_make_tmp("sfe-trash-XXXXXX", NULL);
    g_setenv("XDG_DATA_HOME", root, TRUE); /* before GLib reads it */
    g_setenv("HOME", root, TRUE); /* GLib uses the home trash only on $HOME's device (a tmpfs /tmp is not) */
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/trash/restore", test_restore);
    g_test_add_func("/trash/delete-lists", test_delete_lists);
    int status = g_test_run();

    g_autofree char* cmd = g_strdup_printf("rm -rf '%s'", root);
    g_spawn_command_line_sync(cmd, NULL, NULL, NULL, NULL);
    g_free(root);
    return status;
}
