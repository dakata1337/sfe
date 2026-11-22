/* Tests for history.c and opener.c. */

#include "history.h"
#include "opener.h"

static GFile* f(const char* path)
{
    return g_file_new_for_path(path);
}

static void assert_path(GFile* file, const char* expected)
{
    g_assert_nonnull(file);
    g_assert_cmpstr(g_file_peek_path(file), ==, expected);
    g_object_unref(file);
}

static void test_history(void)
{
    g_autoptr(SfeHistory) h = sfe_history_new();
    g_autoptr(GFile) a = f("/a");
    g_autoptr(GFile) b = f("/b");
    g_autoptr(GFile) c = f("/c");
    g_autoptr(GFile) d = f("/d");

    g_assert_false(sfe_history_can_back(h));
    g_assert_null(sfe_history_back(h, a));

    /* a -> b -> c */
    sfe_history_push(h, a);
    sfe_history_push(h, b);
    g_assert_true(sfe_history_can_back(h));
    g_assert_false(sfe_history_can_forward(h));

    assert_path(sfe_history_back(h, c), "/b");
    assert_path(sfe_history_back(h, b), "/a");
    g_assert_false(sfe_history_can_back(h));
    assert_path(sfe_history_forward(h, a), "/b");
    g_assert_true(sfe_history_can_forward(h));

    /* Navigating somewhere new from b drops the forward entry (c). */
    sfe_history_push(h, b);
    g_assert_false(sfe_history_can_forward(h));
    assert_path(sfe_history_back(h, d), "/b");
}

static void test_lookup(void)
{
    g_autoptr(GHashTable) t = g_hash_table_new(g_str_hash, g_str_equal);
    g_hash_table_insert(t, "gz", "gzip-viewer");
    g_hash_table_insert(t, "tar.gz", "archiver");
    g_hash_table_insert(t, "bashrc", "never");
    g_hash_table_insert(t, "mp4", "mpv %f");

    g_assert_cmpstr(sfe_opener_lookup(t, "backup.tar.gz"), ==, "archiver");
    g_assert_cmpstr(sfe_opener_lookup(t, "notes.gz"), ==, "gzip-viewer");
    g_assert_cmpstr(sfe_opener_lookup(t, "MOVIE.MP4"), ==, "mpv %f");
    g_assert_null(sfe_opener_lookup(t, ".bashrc"));
    g_assert_null(sfe_opener_lookup(t, "README"));
    g_assert_null(sfe_opener_lookup(t, ""));
}

static void test_build_argv(void)
{
    g_autoptr(GError) error = NULL;
    const char* tricky = "/tmp/it's a \"file\" $(rm -rf) %f.mp4";

    g_auto(GStrv) a = sfe_opener_build_argv("mpv --fs %f", tricky, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(g_strv_length(a), ==, 3);
    g_assert_cmpstr(a[0], ==, "mpv");
    g_assert_cmpstr(a[2], ==, tricky);

    g_auto(GStrv) b = sfe_opener_build_argv("zathura", "/x.pdf", &error);
    g_assert_no_error(error);
    g_assert_cmpuint(g_strv_length(b), ==, 2);
    g_assert_cmpstr(b[1], ==, "/x.pdf");

    g_auto(GStrv) c = sfe_opener_build_argv("app --file=%f", "/y", &error);
    g_assert_cmpstr(c[1], ==, "--file=/y");

    char** bad = sfe_opener_build_argv("unterminated \"quote", "/z", &error);
    g_assert_null(bad);
    g_assert_nonnull(error);
}

static void test_user_command(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GFile) dir = g_file_new_for_path("/home/me/My Dir");
    g_autoptr(GFile) a = g_file_new_for_path("/x/a b.txt");
    g_autoptr(GFile) b = g_file_new_for_path("/x/c.txt");
    GList* files = g_list_append(g_list_append(NULL, a), b);

    g_auto(GStrv) all = sfe_build_user_command("tar -czf out.tgz %f", files, dir, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(g_strv_length(all), ==, 5);
    g_assert_cmpstr(all[3], ==, "/x/a b.txt");
    g_assert_cmpstr(all[4], ==, "/x/c.txt");

    g_auto(GStrv) term = sfe_build_user_command("foot -D %d", files, dir, &error);
    g_assert_cmpstr(term[2], ==, "/home/me/My Dir");

    g_auto(GStrv) inner = sfe_build_user_command("app --in=%f --cwd=%d", files, dir, &error);
    g_assert_cmpstr(inner[1], ==, "--in=/x/a b.txt");
    g_assert_cmpstr(inner[2], ==, "--cwd=/home/me/My Dir");

    /* a folder named with "%f" in it is passed as it is */
    g_autoptr(GFile) odd = g_file_new_for_path("/home/me/100%fun");
    g_auto(GStrv) literal = sfe_build_user_command("echo %d %f", files, odd, &error);
    g_assert_cmpstr(literal[1], ==, "/home/me/100%fun");
    g_assert_cmpstr(literal[2], ==, "/x/a b.txt");

    g_auto(GStrv) none = sfe_build_user_command("echo %f", NULL, dir, &error);
    g_assert_cmpuint(g_strv_length(none), ==, 1);
    g_list_free(files);
}

int main(int argc, char** argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/history/back-forward", test_history);
    g_test_add_func("/opener/lookup", test_lookup);
    g_test_add_func("/opener/build-argv", test_build_argv);
    g_test_add_func("/opener/user-command", test_user_command);
    return g_test_run();
}
