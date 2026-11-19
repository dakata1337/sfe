#define _DEFAULT_SOURCE /* symlink() */
#include "sizecount.h"

#include <glib/gstdio.h>
#include <stdlib.h>
#include <unistd.h>

static char* root;

static void write_file(const char* rel, gsize size)
{
    g_autofree char* path = g_build_filename(root, rel, NULL);
    g_autofree char* data = g_malloc0(size + 1);
    g_assert_true(g_file_set_contents(path, data, (gssize)size, NULL));
}

static void make_dir(const char* rel)
{
    g_autofree char* path = g_build_filename(root, rel, NULL);
    g_assert_cmpint(g_mkdir_with_parents(path, 0755), ==, 0);
}

static GList* files_of(const char* const* rels)
{
    GList* files = NULL;
    for (int i = 0; rels[i] != NULL; i++) {
        g_autofree char* path = g_build_filename(root, rels[i], NULL);
        files = g_list_append(files, g_file_new_for_path(path));
    }
    return files;
}

static void setup(void)
{
    root = g_dir_make_tmp("sfe-sizecount-XXXXXX", NULL);
    make_dir("dir/sub");
    write_file("a", 100);
    write_file("dir/b", 20);
    write_file("dir/sub/c", 3);
    g_autofree char* link = g_build_filename(root, "dir/link", NULL);
    g_assert_cmpint(symlink("/usr", link), ==, 0); /* not followed */
}

static void test_count(void)
{
    const char* const rels[] = { "a", "dir", NULL };
    GList* files = files_of(rels);
    SfeSizeCount count;
    sfe_size_count(files, NULL, &count);
    g_assert_cmpuint(count.files, ==, 1);
    g_assert_cmpuint(count.folders, ==, 1);
    g_assert_cmpuint(count.bytes, >=, 123); /* plus the link's own size */
    g_assert_cmpuint(count.bytes, <, 200);
    g_autofree char* items = sfe_size_count_items(&count);
    g_assert_cmpstr(items, ==, "2 items");
    g_list_free_full(files, g_object_unref);
}

static void test_items(void)
{
    SfeSizeCount one_file = { 0, 1, 0 }, files = { 0, 3, 0 }, one_folder = { 0, 0, 1 }, folders = { 0, 0, 2 };
    g_autofree char* a = sfe_size_count_items(&one_file);
    g_autofree char* b = sfe_size_count_items(&files);
    g_autofree char* c = sfe_size_count_items(&one_folder);
    g_autofree char* d = sfe_size_count_items(&folders);
    g_assert_cmpstr(a, ==, "1 file");
    g_assert_cmpstr(b, ==, "3 files");
    g_assert_cmpstr(c, ==, "1 folder");
    g_assert_cmpstr(d, ==, "2 folders");
}

static void on_counted(GObject* source, GAsyncResult* res, gpointer data)
{
    (void)source;
    SfeSizeCount count;
    g_assert_true(sfe_size_count_finish(res, &count, NULL));
    g_assert_cmpuint(count.bytes, ==, 3);
    g_main_loop_quit(data);
}

static void test_async(void)
{
    const char* const rels[] = { "dir/sub", NULL };
    GList* files = files_of(rels);
    GMainLoop* loop = g_main_loop_new(NULL, FALSE);
    sfe_size_count_async(files, NULL, on_counted, loop);
    g_list_free_full(files, g_object_unref); /* copied */
    g_main_loop_run(loop);
    g_main_loop_unref(loop);
}

static void on_cancelled(GObject* source, GAsyncResult* res, gpointer data)
{
    (void)source;
    SfeSizeCount count;
    g_autoptr(GError) error = NULL;
    g_assert_false(sfe_size_count_finish(res, &count, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_main_loop_quit(data);
}

/* A count whose notice went away stops (the window cancels it). */
static void test_cancel(void)
{
    const char* const rels[] = { "dir", NULL };
    GList* files = files_of(rels);
    g_autoptr(GCancellable) cancel = g_cancellable_new();
    g_cancellable_cancel(cancel);
    GMainLoop* loop = g_main_loop_new(NULL, FALSE);
    sfe_size_count_async(files, cancel, on_cancelled, loop);
    g_list_free_full(files, g_object_unref);
    g_main_loop_run(loop);
    g_main_loop_unref(loop);
}

int main(int argc, char** argv)
{
    g_test_init(&argc, &argv, NULL);
    setup();
    g_test_add_func("/sizecount/count", test_count);
    g_test_add_func("/sizecount/items", test_items);
    g_test_add_func("/sizecount/async", test_async);
    g_test_add_func("/sizecount/cancel", test_cancel);
    int result = g_test_run();
    g_autofree char* cmd = g_strdup_printf("rm -rf '%s'", root);
    g_assert_cmpint(system(cmd), ==, 0);
    return result;
}
