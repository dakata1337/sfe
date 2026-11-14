#include "bookmarks.h"

#include <glib/gstdio.h>

static SfeBookmark* at(GPtrArray* list, guint i)
{
    return g_ptr_array_index(list, i);
}

static void test_list(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char* dir = g_dir_make_tmp("sfe-bm-XXXXXX", &error);
    g_autofree char* gtk_file = g_build_filename(dir, "bookmarks", NULL);
    g_autoptr(SfeConfig) cfg = sfe_config_new();
    const char* home = g_get_home_dir();
    g_autofree char* docs = g_build_filename(home, "Documents", NULL);
    g_autofree char* contents = g_strdup_printf(
        "file:///srv/projects Work Stuff\n"
        "file://%s\n" /* duplicate of the sfe bookmark: dropped */
        "sftp://host/path Remote\n" /* not local: dropped */
        "\n"
        "file:///tmp\n",
        docs);

    g_assert_no_error(error);
    g_assert_true(g_file_set_contents(gtk_file, contents, -1, NULL));
    g_ptr_array_add(cfg->bookmarks, g_strdup("~/Documents"));
    g_ptr_array_add(cfg->bookmarks, g_strdup("/mnt/data"));

    g_autoptr(GPtrArray) list = sfe_bookmarks_list(cfg, gtk_file);
    g_assert_cmpuint(list->len, ==, 4);
    g_assert_cmpstr(g_file_peek_path(at(list, 0)->file), ==, docs);
    g_assert_cmpstr(at(list, 0)->label, ==, "Documents");
    g_assert_cmpint(at(list, 0)->config_index, ==, 0);
    g_assert_cmpint(at(list, 1)->config_index, ==, 1);
    g_assert_cmpstr(at(list, 2)->label, ==, "Work Stuff");
    g_assert_cmpint(at(list, 2)->config_index, ==, -1);
    g_assert_cmpstr(at(list, 3)->label, ==, "tmp");

    cfg->show_gtk_bookmarks = false;
    g_autoptr(GPtrArray) own = sfe_bookmarks_list(cfg, gtk_file);
    g_assert_cmpuint(own->len, ==, 2);

    g_remove(gtk_file);
    g_rmdir(dir);
}

static void test_edit(void)
{
    g_autoptr(SfeConfig) cfg = sfe_config_new();
    g_autofree char* music_path = g_build_filename(g_get_home_dir(), "Music", NULL);
    g_autoptr(GFile) music = g_file_new_for_path(music_path);
    g_autoptr(GFile) a = g_file_new_for_path("/a");
    g_autoptr(GFile) b = g_file_new_for_path("/b");

    sfe_bookmarks_add(cfg, music);
    sfe_bookmarks_add(cfg, music); /* no duplicates */
    sfe_bookmarks_add(cfg, a);
    sfe_bookmarks_add(cfg, b);
    g_assert_cmpuint(cfg->bookmarks->len, ==, 3);
    g_assert_cmpstr(g_ptr_array_index(cfg->bookmarks, 0), ==, "~/Music"); /* stored portable */
    g_assert_cmpint(sfe_bookmarks_find(cfg, b), ==, 2);

    sfe_bookmarks_move(cfg, 2, 0); /* b, ~/Music, a */
    g_assert_cmpstr(g_ptr_array_index(cfg->bookmarks, 0), ==, "/b");
    g_assert_cmpstr(g_ptr_array_index(cfg->bookmarks, 2), ==, "/a");
    sfe_bookmarks_move(cfg, 0, 7); /* out of range: ignored */
    g_assert_cmpstr(g_ptr_array_index(cfg->bookmarks, 0), ==, "/b");

    sfe_bookmarks_remove(cfg, sfe_bookmarks_find(cfg, music));
    g_assert_cmpuint(cfg->bookmarks->len, ==, 2);
    g_assert_cmpint(sfe_bookmarks_find(cfg, music), ==, -1);
}

int main(int argc, char** argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/bookmarks/list", test_list);
    g_test_add_func("/bookmarks/edit", test_edit);
    return g_test_run();
}
