#include "archive.h"
#include "fileops.h"

#include <glib/gstdio.h>
#include <string.h>

/* A fake PATH lookup: only the tools listed in `available` exist. */
static const char* available;

static char* fake_find(const char* name)
{
    g_auto(GStrv) tools = g_strsplit(available, " ", -1);
    for (char** t = tools; *t != NULL; t++) {
        if (strcmp(*t, name) == 0)
            return g_strconcat("/usr/bin/", name, NULL);
    }
    return NULL;
}

static void test_names(void)
{
    struct {
        const char *name, *folder;
    } cases[] = {
        { "photos.tar.gz", "photos" },
        { "backup.TGZ", "backup" },
        { "music.zip", "music" },
        { "v1.2.rar", "v1.2" },
        { "readme.txt", "readme.txt" }, /* not an archive: the name stays */
        { ".zip", ".zip" }, /* nothing before the extension */
    };
    for (size_t i = 0; i < G_N_ELEMENTS(cases); i++) {
        g_autofree char* folder = sfe_archive_folder_name(cases[i].name);
        g_assert_cmpstr(folder, ==, cases[i].folder);
    }
}

static void test_tool_choice(void)
{
    available = "tar unzip unrar";
    g_auto(GStrv) zip = sfe_archive_extract_argv("/x/a.zip", "/out", fake_find);
    g_assert_cmpstr(zip[0], ==, "/usr/bin/unzip");
    g_assert_cmpstr(zip[4], ==, "/out");

    g_auto(GStrv) tgz = sfe_archive_extract_argv("/x/a.tar.gz", "/out", fake_find);
    g_assert_cmpstr(tgz[0], ==, "/usr/bin/tar");
    g_assert_cmpstr(tgz[1], ==, "-xf");

    g_auto(GStrv) rar = sfe_archive_extract_argv("/x/a.rar", "/out", fake_find);
    g_assert_cmpstr(rar[0], ==, "/usr/bin/unrar");
    g_assert_cmpstr(rar[5], ==, "/out/");

    g_assert_null(sfe_archive_extract_argv("/x/a.7z", "/out", fake_find)); /* no 7z, no bsdtar */

    available = "bsdtar tar unzip 7z";
    g_auto(GStrv) zip2 = sfe_archive_extract_argv("/x/a.zip", "/out", fake_find);
    g_assert_cmpstr(zip2[0], ==, "/usr/bin/bsdtar"); /* preferred */
    g_auto(GStrv) sz = sfe_archive_extract_argv("/x/a.7z", "/out", fake_find);
    g_assert_cmpstr(sz[0], ==, "/usr/bin/7z");
    g_assert_cmpstr(sz[3], ==, "-o/out");
}

static void test_compress_choice(void)
{
    char* names[] = { "photos", "-odd", "@list", NULL };

    available = "tar zip";
    g_auto(GStrv) zip = sfe_archive_compress_argv("/x/a.zip", names, fake_find);
    g_assert_cmpstr(zip[0], ==, "/usr/bin/zip");
    g_assert_cmpstr(zip[3], ==, "/x/a.zip");
    g_assert_cmpstr(zip[4], ==, "photos");
    g_assert_cmpstr(zip[5], ==, "./-odd"); /* not an option */
    g_assert_cmpstr(zip[6], ==, "./@list"); /* not a list file */

    g_assert_null(sfe_archive_compress_argv("/x/a.tar.zst", names, fake_find)); /* GNU tar needs zstd */
    g_assert_null(sfe_archive_compress_argv("/x/a.7z", names, fake_find));
    g_assert_null(sfe_archive_compress_argv("/x/a.rar", names, fake_find)); /* never created */
    g_assert_null(sfe_archive_compress_argv("/x/notes.txt", names, fake_find));

    available = "tar zstd";
    g_auto(GStrv) zst = sfe_archive_compress_argv("/x/a.TAR.ZST", names, fake_find);
    g_assert_cmpstr(zst[0], ==, "/usr/bin/tar");
    g_assert_cmpstr(zst[1], ==, "-acf");

    available = "bsdtar tar zip 7z";
    g_auto(GStrv) zip2 = sfe_archive_compress_argv("/x/a.zip", names, fake_find);
    g_assert_cmpstr(zip2[0], ==, "/usr/bin/bsdtar"); /* preferred */
    g_auto(GStrv) sz = sfe_archive_compress_argv("/x/a.7z", names, fake_find);
    g_assert_cmpstr(sz[0], ==, "/usr/bin/7z");
    g_assert_cmpstr(sz[2], ==, "-t7z");
}

/* ------------------------------------------------------------------------ */
/* Real extraction through the file operation                                */
/* ------------------------------------------------------------------------ */

typedef struct {
    GMainLoop* loop;
    int errors;
} Run;

static void on_question(SfeFileOp* op, int kind, GFile* src, GFile* dest, const char* error, gpointer data)
{
    (void)kind, (void)src, (void)dest, (void)error;
    ((Run*)data)->errors++;
    sfe_file_op_answer(op, SFE_ANSWER_SKIP, false);
}

static void on_finished(SfeFileOp* op, GError* error, gpointer data)
{
    (void)op, (void)error;
    g_main_loop_quit(((Run*)data)->loop);
}

static int run_job(SfeOpKind kind, GList* sources, const char* dest)
{
    Run run = { .loop = g_main_loop_new(NULL, FALSE) };
    g_autoptr(GFile) d = g_file_new_for_path(dest);
    g_autoptr(SfeFileOp) op = sfe_file_op_new(kind, sources, d);

    g_signal_connect(op, "question", G_CALLBACK(on_question), &run);
    g_signal_connect(op, "finished", G_CALLBACK(on_finished), &run);
    sfe_file_op_start(op);
    g_main_loop_run(run.loop);
    g_main_loop_unref(run.loop);
    return run.errors;
}

static int extract(const char* archive, const char* dest)
{
    g_autoptr(GFile) a = g_file_new_for_path(archive);
    GList one = { .data = a };
    return run_job(SFE_OP_EXTRACT, &one, dest);
}

static bool run_cmd(const char* cmd, const char* cwd)
{
    g_auto(GStrv) argv = NULL;
    int status = 1;
    return g_shell_parse_argv(cmd, NULL, &argv, NULL)
        && g_spawn_sync(cwd, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
            NULL, NULL, NULL, NULL, &status, NULL)
        && g_spawn_check_wait_status(status, NULL);
}

static void test_extract(void)
{
    g_autofree char* tar = g_find_program_in_path("tar");
    g_autofree char* bsdtar = g_find_program_in_path("bsdtar");
    if (tar == NULL && bsdtar == NULL) {
        g_test_skip("neither tar nor bsdtar is installed");
        return;
    }

    g_autofree char* root = g_dir_make_tmp("sfe-archive-XXXXXX", NULL);
    g_autofree char* src = g_build_filename(root, "photos", NULL);
    g_autofree char* sub = g_build_filename(src, "sub", NULL);
    g_autofree char* file = g_build_filename(sub, "a.txt", NULL);
    g_mkdir_with_parents(sub, 0755);
    g_file_set_contents(file, "hello", -1, NULL);
    g_assert_true(run_cmd("tar -czf photos.tar.gz photos", root));
    g_assert_true(run_cmd("rm -r photos", root));

    g_autofree char* archive = g_build_filename(root, "photos.tar.gz", NULL);
    g_assert_cmpint(extract(archive, root), ==, 0);
    g_autofree char* out = g_build_filename(root, "photos", "photos", "sub", "a.txt", NULL);
    g_autofree char* text = NULL;
    g_assert_true(g_file_get_contents(out, &text, NULL, NULL));
    g_assert_cmpstr(text, ==, "hello");

    /* Extracting again keeps both: "photos (2)". */
    g_assert_cmpint(extract(archive, root), ==, 0);
    g_autofree char* second = g_build_filename(root, "photos (2)", NULL);
    g_assert_true(g_file_test(second, G_FILE_TEST_IS_DIR));

    /* A broken archive is reported and leaves no folder behind. */
    g_autofree char* bad = g_build_filename(root, "broken.tar.gz", NULL);
    g_file_set_contents(bad, "this is not an archive", -1, NULL);
    g_assert_cmpint(extract(bad, root), ==, 1);
    g_autofree char* bad_dir = g_build_filename(root, "broken", NULL);
    g_assert_false(g_file_test(bad_dir, G_FILE_TEST_EXISTS));

    g_autofree char* cleanup = g_strdup_printf("rm -rf '%s'", root);
    run_cmd(cleanup, NULL);
}

/* Every format with an installed tool: compress two items, extract the
 * archive again, compare. */
static void test_compress(void)
{
    g_autofree char* root = g_dir_make_tmp("sfe-compress-XXXXXX", NULL);
    g_autofree char* dir = g_build_filename(root, "photos", "sub", NULL);
    g_autofree char* inner = g_build_filename(dir, "a.txt", NULL);
    g_autofree char* odd = g_build_filename(root, "-odd *name.txt", NULL);
    g_mkdir_with_parents(dir, 0755);
    g_file_set_contents(inner, "hello", -1, NULL);
    g_file_set_contents(odd, "odd", -1, NULL);

    g_autoptr(GFile) photos = g_file_new_build_filename(root, "photos", NULL);
    g_autoptr(GFile) odd_file = g_file_new_for_path(odd);
    GList second = { .data = odd_file };
    GList sources = { .data = photos, .next = &second };
    size_t n = 0;
    const SfeArchiveFormat* formats = sfe_archive_formats(&n);
    int tried = 0;

    for (size_t i = 0; i < n; i++) {
        if (!sfe_archive_can_compress(formats[i].ext))
            continue;
        tried++;
        g_autofree char* name = g_strconcat("out", formats[i].ext, NULL);
        g_autofree char* archive = g_build_filename(root, name, NULL);
        g_assert_cmpint(run_job(SFE_OP_COMPRESS, &sources, archive), ==, 0);
        g_assert_true(g_file_test(archive, G_FILE_TEST_IS_REGULAR));

        if (sfe_archive_can_extract(name)) {
            g_assert_cmpint(extract(archive, root), ==, 0);
            g_autofree char* back = g_build_filename(root, "out", "photos", "sub", "a.txt", NULL);
            g_autofree char* back_odd = g_build_filename(root, "out", "-odd *name.txt", NULL);
            g_autofree char* text = NULL;
            g_assert_true(g_file_get_contents(back, &text, NULL, NULL));
            g_assert_cmpstr(text, ==, "hello");
            g_assert_true(g_file_test(back_odd, G_FILE_TEST_IS_REGULAR));
            g_autofree char* rm = g_strdup_printf("rm -rf '%s/out'", root);
            run_cmd(rm, NULL);
        }

        /* An existing archive is never added to. */
        g_assert_cmpint(run_job(SFE_OP_COMPRESS, &sources, archive), ==, 0);
        g_autofree char* second_name = g_strconcat("out (2)", formats[i].ext, NULL);
        g_autofree char* second_path = g_build_filename(root, second_name, NULL);
        g_assert_true(g_file_test(second_path, G_FILE_TEST_IS_REGULAR));
    }
    if (tried == 0)
        g_test_skip("no archive tool is installed");

    g_autofree char* cleanup = g_strdup_printf("rm -rf '%s'", root);
    run_cmd(cleanup, NULL);
}

int main(int argc, char** argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/archive/names", test_names);
    g_test_add_func("/archive/tool-choice", test_tool_choice);
    g_test_add_func("/archive/extract", test_extract);
    g_test_add_func("/archive/compress-choice", test_compress_choice);
    g_test_add_func("/archive/compress", test_compress);
    return g_test_run();
}
