#include "rename.h"
#include "util.h"

#include <glib/gstdio.h>

static char* apply(const SfeRenameRule* rule, const char* name, bool is_dir, int index)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(SfeRenamer) r = sfe_renamer_new(rule, &error);
    g_assert_no_error(error);
    return sfe_renamer_apply(r, name, is_dir, index);
}

#define ASSERT_RENAME(rule, name, is_dir, index, expect)                      \
    do {                                                                       \
        g_autofree char* got_ = apply(rule, name, is_dir, index);              \
        g_assert_cmpstr(got_, ==, expect);                                     \
    } while (0)

static void test_split_extension(void)
{
    struct {
        const char *name, *stem, *ext;
        bool dir;
    } cases[] = {
        { "photo.jpg", "photo", ".jpg", false },
        { "a.tar.gz", "a", ".tar.gz", false },
        { ".bashrc", ".bashrc", "", false },
        { "noext", "noext", "", false },
        { "my.dir", "my.dir", "", true },
        { "trailing.", "trailing.", "", false },
    };
    for (size_t i = 0; i < G_N_ELEMENTS(cases); i++) {
        g_autofree char* stem = NULL;
        g_autofree char* ext = NULL;
        sfe_split_extension(cases[i].name, cases[i].dir, &stem, &ext);
        g_assert_cmpstr(stem, ==, cases[i].stem);
        g_assert_cmpstr(ext, ==, cases[i].ext);
    }
}

static void test_template_and_counter(void)
{
    SfeRenameRule keep = { .counter_start = 1, .counter_step = 1 };
    ASSERT_RENAME(&keep, "photo.JPG", false, 0, "photo.JPG");

    SfeRenameRule numbered = { .template_ = "IMG_{n:03}{ext}", .counter_start = 1, .counter_step = 1 };
    ASSERT_RENAME(&numbered, "DSC1234.jpg", false, 0, "IMG_001.jpg");
    ASSERT_RENAME(&numbered, "DSC1235.jpg", false, 41, "IMG_042.jpg");

    SfeRenameRule step = { .template_ = "{name}-{n}{ext}", .counter_start = 10, .counter_step = 5 };
    ASSERT_RENAME(&step, "a.txt", false, 2, "a-20.txt");

    SfeRenameRule ext = { .template_ = "{name}.md", .counter_start = 1, .counter_step = 1 };
    ASSERT_RENAME(&ext, "notes.txt", false, 0, "notes.md");

    SfeRenameRule unknown = { .template_ = "{name}{oops}{ext}", .counter_start = 1, .counter_step = 1 };
    ASSERT_RENAME(&unknown, "a.txt", false, 0, "a{oops}.txt");

    /* Found by fuzzing: a long width overflowed atoi() into a negative
     * printf width, which padded to a billion characters. Capped at 32. */
    SfeRenameRule wide = { .template_ = "{n:1111111111111115}", .counter_start = 7, .counter_step = 1 };
    ASSERT_RENAME(&wide, "a", false, 0, "00000000000000000000000000000007");
}

static void test_find_replace(void)
{
    SfeRenameRule plain = { .find = "foo", .replace = "bar", .counter_step = 1 };
    ASSERT_RENAME(&plain, "Foo foo.foo", false, 0, "bar bar.foo"); /* stem only, case-insensitive */

    SfeRenameRule cased = { .find = "foo", .replace = "bar", .match_case = true, .counter_step = 1 };
    ASSERT_RENAME(&cased, "Foo foo.txt", false, 0, "Foo bar.txt");

    SfeRenameRule dots = { .find = ".", .replace = "_", .counter_step = 1 }; /* plain: not a regex */
    ASSERT_RENAME(&dots, "a.b.c.txt", false, 0, "a_b_c.txt");

    SfeRenameRule re = { .find = "(\\d+)-(\\d+)", .replace = "\\2-\\1", .regex = true, .counter_step = 1 };
    ASSERT_RENAME(&re, "shot 12-34.png", false, 0, "shot 34-12.png");

    g_autoptr(GError) error = NULL;
    SfeRenameRule bad = { .find = "(", .regex = true };
    g_assert_null(sfe_renamer_new(&bad, &error));
    g_assert_nonnull(error);
}

static void test_case(void)
{
    SfeRenameRule lower = { .case_mode = SFE_CASE_LOWER, .counter_step = 1 };
    ASSERT_RENAME(&lower, "HELLO World.TXT", false, 0, "hello world.TXT");

    SfeRenameRule upper = { .case_mode = SFE_CASE_UPPER, .counter_step = 1 };
    ASSERT_RENAME(&upper, "čаша.txt", false, 0, "ČАША.txt");

    SfeRenameRule title = { .case_mode = SFE_CASE_TITLE, .counter_step = 1 };
    ASSERT_RENAME(&title, "the QUICK-brown fox.mp3", false, 0, "The Quick-Brown Fox.mp3");
    ASSERT_RENAME(&title, "my.folder", true, 0, "My.Folder");
}

/* ------------------------------------------------------------------------ */

static char* make_dir(void)
{
    char* dir = g_dir_make_tmp("sfe-rename-XXXXXX", NULL);
    g_assert_nonnull(dir);
    return dir;
}

static GFile* touch(const char* dir, const char* name, const char* text)
{
    g_autofree char* path = g_build_filename(dir, name, NULL);
    g_assert_true(g_file_set_contents(path, text, -1, NULL));
    return g_file_new_for_path(path);
}

static char* read_name(const char* dir, const char* name)
{
    g_autofree char* path = g_build_filename(dir, name, NULL);
    char* text = NULL;
    g_file_get_contents(path, &text, NULL, NULL);
    return text;
}

static void remove_all(const char* dir)
{
    g_autoptr(GDir) d = g_dir_open(dir, 0, NULL);
    const char* name;
    while (d != NULL && (name = g_dir_read_name(d)) != NULL) {
        g_autofree char* path = g_build_filename(dir, name, NULL);
        g_remove(path);
    }
    g_rmdir(dir);
}

static void test_check(void)
{
    g_autofree char* dir = make_dir();
    GFile* files[] = { touch(dir, "a", "A"), touch(dir, "b", "B"), touch(dir, "c", "C") };
    g_autoptr(GFile) other = touch(dir, "taken", "T");
    SfeRenameStatus st[3];

    char* swap[] = { "b", "a", "c" }; /* a <-> b is fine; c unchanged */
    g_assert_cmpint(sfe_rename_check(files, swap, 3, st), ==, 0);
    g_assert_cmpint(st[0], ==, SFE_RENAME_OK);
    g_assert_cmpint(st[2], ==, SFE_RENAME_UNCHANGED);

    char* dup[] = { "x", "x", "c" };
    g_assert_cmpint(sfe_rename_check(files, dup, 3, st), ==, 2);
    g_assert_cmpint(st[0], ==, SFE_RENAME_DUPLICATE);

    char* onto_unchanged[] = { "c", "b", "c" }; /* c keeps its name */
    sfe_rename_check(files, onto_unchanged, 3, st);
    g_assert_cmpint(st[0], ==, SFE_RENAME_DUPLICATE);

    char* exists[] = { "taken", "", "sub/dir" };
    g_assert_cmpint(sfe_rename_check(files, exists, 3, st), ==, 3);
    g_assert_cmpint(st[0], ==, SFE_RENAME_EXISTS);
    g_assert_cmpint(st[1], ==, SFE_RENAME_EMPTY);
    g_assert_cmpint(st[2], ==, SFE_RENAME_INVALID);

    for (int i = 0; i < 3; i++)
        g_object_unref(files[i]);
    remove_all(dir);
}

static void test_rename_swap_and_cycle(void)
{
    g_autofree char* dir = make_dir();
    GFile* files[] = { touch(dir, "one", "1"), touch(dir, "two", "2"), touch(dir, "three", "3") };

    char* cycle[] = { "two", "three", "one" }; /* one->two, two->three, three->one */
    g_assert_true(sfe_rename_files(files, cycle, 3, NULL));
    g_autofree char* t1 = read_name(dir, "two");
    g_autofree char* t2 = read_name(dir, "three");
    g_autofree char* t3 = read_name(dir, "one");
    g_assert_cmpstr(t1, ==, "1");
    g_assert_cmpstr(t2, ==, "2");
    g_assert_cmpstr(t3, ==, "3");

    /* A failing rename (target is a non-empty folder name clash) rolls back. */
    g_autofree char* sub = g_build_filename(dir, "blocker", NULL);
    g_mkdir(sub, 0755);
    g_autoptr(GFile) inner = touch(sub, "x", "x");
    g_autofree char* p_two = g_build_filename(dir, "two", NULL);
    g_autofree char* p_three = g_build_filename(dir, "three", NULL);
    g_autoptr(GFile) f_two = g_file_new_for_path(p_two);
    g_autoptr(GFile) f_three = g_file_new_for_path(p_three);
    GFile* again[] = { f_two, f_three };
    char* names[] = { "renamed", "blocker" };
    g_autoptr(GError) error = NULL;
    g_assert_false(sfe_rename_files(again, names, 2, &error));
    g_assert_nonnull(error);
    g_autofree char* back = read_name(dir, "two");
    g_assert_cmpstr(back, ==, "1"); /* rolled back */
    g_autofree char* gone = read_name(dir, "renamed");
    g_assert_null(gone);

    for (int i = 0; i < 3; i++)
        g_object_unref(files[i]);
    g_remove(g_file_peek_path(inner));
    g_rmdir(sub);
    remove_all(dir);
}

int main(int argc, char** argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/rename/split-extension", test_split_extension);
    g_test_add_func("/rename/template-counter", test_template_and_counter);
    g_test_add_func("/rename/find-replace", test_find_replace);
    g_test_add_func("/rename/case", test_case);
    g_test_add_func("/rename/check", test_check);
    g_test_add_func("/rename/swap-and-cycle", test_rename_swap_and_cycle);
    return g_test_run();
}
