#include "fileops.h"

#include <glib/gstdio.h>

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

typedef struct {
    GMainLoop* loop;
    SfeOpAnswer answer; /* given to every question */
    bool apply_to_all;
    int questions;
    SfeOpQuestion last_kind;
    bool cancelled;
} Run;

static void on_question(SfeFileOp* op, int kind, GFile* src, GFile* dest, const char* error, gpointer data)
{
    (void)src, (void)dest, (void)error;
    Run* run = data;
    run->questions++;
    run->last_kind = kind;
    sfe_file_op_answer(op, run->answer, run->apply_to_all);
}

static void on_finished(SfeFileOp* op, GError* error, gpointer data)
{
    (void)op;
    Run* run = data;
    run->cancelled = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_main_loop_quit(run->loop);
}

/* Runs a job to completion, answering every question with `answer`. */
static Run run_op(SfeOpKind kind, GList* sources, GFile* dest, SfeOpAnswer answer, bool apply_to_all)
{
    Run run = { .loop = g_main_loop_new(NULL, FALSE), .answer = answer, .apply_to_all = apply_to_all };
    g_autoptr(SfeFileOp) op = sfe_file_op_new(kind, sources, dest);

    g_signal_connect(op, "question", G_CALLBACK(on_question), &run);
    g_signal_connect(op, "finished", G_CALLBACK(on_finished), &run);
    sfe_file_op_start(op);
    g_main_loop_run(run.loop);
    g_main_loop_unref(run.loop);
    return run;
}

static GList* one(GFile* file)
{
    return g_list_append(NULL, file);
}

static char* path(const char* root, const char* rel)
{
    return g_build_filename(root, rel, NULL);
}

static void write_file(const char* root, const char* rel, const char* text)
{
    g_autofree char* p = path(root, rel);
    g_autofree char* dir = g_path_get_dirname(p);
    g_mkdir_with_parents(dir, 0755);
    g_assert_true(g_file_set_contents(p, text, -1, NULL));
}

static char* read_file(const char* root, const char* rel)
{
    g_autofree char* p = path(root, rel);
    char* text = NULL;
    g_file_get_contents(p, &text, NULL, NULL);
    return text;
}

static bool exists(const char* root, const char* rel)
{
    g_autofree char* p = path(root, rel);
    return g_file_test(p, G_FILE_TEST_EXISTS);
}

static GFile* file_at(const char* root, const char* rel)
{
    g_autofree char* p = path(root, rel);
    return g_file_new_for_path(p);
}

static char* make_root(void)
{
    char* root = g_dir_make_tmp("sfe-ops-XXXXXX", NULL);
    g_assert_nonnull(root);
    return root;
}

static void remove_root(const char* root)
{
    g_autoptr(GFile) dir = g_file_new_for_path(root);
    g_autoptr(GList) list = one(dir);
    run_op(SFE_OP_DELETE, list, NULL, SFE_ANSWER_CANCEL, false);
    g_assert_false(g_file_test(root, G_FILE_TEST_EXISTS));
}

/* ------------------------------------------------------------------------ */
/* Tests                                                                     */
/* ------------------------------------------------------------------------ */

static void test_numbered_name(void)
{
    struct {
        const char *name, *expect;
        bool dir;
        int n;
    } cases[] = {
        { "a.txt", "a (2).txt", false, 2 },
        { "a (2).txt", "a (3).txt", false, 3 },
        { "photo", "photo (2)", false, 2 },
        { "dir.d", "dir.d (2)", true, 2 },
        { ".bashrc", ".bashrc (2)", false, 2 },
        { "x.tar.gz", "x (2).tar.gz", false, 2 },
        { "(1)", "(1) (2)", false, 2 },
    };
    for (size_t i = 0; i < G_N_ELEMENTS(cases); i++) {
        g_autofree char* got = sfe_numbered_name(cases[i].name, cases[i].dir, cases[i].n);
        g_assert_cmpstr(got, ==, cases[i].expect);
    }
}

static void test_copy_tree(void)
{
    g_autofree char* root = make_root();
    write_file(root, "src/a.txt", "A");
    write_file(root, "src/sub/b.txt", "B");
    g_autofree char* dst_path = path(root, "dst");
    g_mkdir(dst_path, 0755);

    g_autoptr(GFile) src = file_at(root, "src");
    g_autoptr(GFile) dst = file_at(root, "dst");
    g_autoptr(GList) list = one(src);
    Run run = run_op(SFE_OP_COPY, list, dst, SFE_ANSWER_CANCEL, false);

    g_assert_cmpint(run.questions, ==, 0);
    g_autofree char* b = read_file(root, "dst/src/sub/b.txt");
    g_assert_cmpstr(b, ==, "B");
    g_assert_true(exists(root, "src/a.txt")); /* source untouched */

    /* Copying into its own folder keeps both. */
    g_autoptr(GFile) file = file_at(root, "src/a.txt");
    g_autoptr(GFile) src_dir = file_at(root, "src");
    g_autoptr(GList) list2 = one(file);
    run_op(SFE_OP_COPY, list2, src_dir, SFE_ANSWER_CANCEL, false);
    g_autofree char* copy = read_file(root, "src/a (2).txt");
    g_assert_cmpstr(copy, ==, "A");

    remove_root(root);
}

static void test_conflicts(void)
{
    g_autofree char* root = make_root();
    write_file(root, "src/a.txt", "new");
    write_file(root, "dst/a.txt", "old");
    g_autoptr(GFile) a = file_at(root, "src/a.txt");
    g_autoptr(GFile) dst = file_at(root, "dst");
    g_autoptr(GList) list = one(a);

    Run skip = run_op(SFE_OP_COPY, list, dst, SFE_ANSWER_SKIP, false);
    g_assert_cmpint(skip.questions, ==, 1);
    g_assert_cmpint(skip.last_kind, ==, SFE_QUESTION_CONFLICT);
    g_autofree char* t1 = read_file(root, "dst/a.txt");
    g_assert_cmpstr(t1, ==, "old");

    run_op(SFE_OP_COPY, list, dst, SFE_ANSWER_RENAME, false);
    g_autofree char* t2 = read_file(root, "dst/a (2).txt");
    g_assert_cmpstr(t2, ==, "new");

    run_op(SFE_OP_COPY, list, dst, SFE_ANSWER_REPLACE, false);
    g_autofree char* t3 = read_file(root, "dst/a.txt");
    g_assert_cmpstr(t3, ==, "new");

    remove_root(root);
}

/* Folders merge; "apply to all" answers every later conflict at once. */
static void test_merge_apply_to_all(void)
{
    g_autofree char* root = make_root();
    write_file(root, "src/d/1.txt", "new1");
    write_file(root, "src/d/2.txt", "new2");
    write_file(root, "src/d/3.txt", "new3");
    write_file(root, "dst/d/1.txt", "old1");
    write_file(root, "dst/d/2.txt", "old2");
    g_autoptr(GFile) d = file_at(root, "src/d");
    g_autoptr(GFile) dst = file_at(root, "dst");
    g_autoptr(GList) list = one(d);

    Run run = run_op(SFE_OP_COPY, list, dst, SFE_ANSWER_REPLACE, true);
    g_assert_cmpint(run.questions, ==, 1);
    g_autofree char* t1 = read_file(root, "dst/d/1.txt");
    g_autofree char* t2 = read_file(root, "dst/d/2.txt");
    g_autofree char* t3 = read_file(root, "dst/d/3.txt");
    g_assert_cmpstr(t1, ==, "new1");
    g_assert_cmpstr(t2, ==, "new2");
    g_assert_cmpstr(t3, ==, "new3");

    remove_root(root);
}

static void test_move_and_delete(void)
{
    g_autofree char* root = make_root();
    write_file(root, "src/sub/x.txt", "X");
    write_file(root, "dst/keep.txt", "K");
    g_autoptr(GFile) sub = file_at(root, "src/sub");
    g_autoptr(GFile) dst = file_at(root, "dst");
    g_autoptr(GList) list = one(sub);

    run_op(SFE_OP_MOVE, list, dst, SFE_ANSWER_CANCEL, false);
    g_assert_false(exists(root, "src/sub"));
    g_autofree char* x = read_file(root, "dst/sub/x.txt");
    g_assert_cmpstr(x, ==, "X");

    /* A folder cannot go inside itself: reported as an error, nothing moves. */
    g_autoptr(GFile) moved = file_at(root, "dst/sub");
    g_autoptr(GList) self = one(dst);
    Run run = run_op(SFE_OP_MOVE, self, moved, SFE_ANSWER_SKIP, false);
    g_assert_cmpint(run.last_kind, ==, SFE_QUESTION_ERROR);
    g_assert_true(exists(root, "dst/keep.txt"));

    g_autoptr(GList) del = one(moved);
    run_op(SFE_OP_DELETE, del, NULL, SFE_ANSWER_CANCEL, false);
    g_assert_false(exists(root, "dst/sub"));
    g_assert_true(exists(root, "dst/keep.txt"));

    remove_root(root);
}

static void test_cancel_from_question(void)
{
    g_autofree char* root = make_root();
    write_file(root, "src/a.txt", "1");
    write_file(root, "src/b.txt", "2");
    write_file(root, "dst/a.txt", "old");
    g_autoptr(GFile) a = file_at(root, "src/a.txt");
    g_autoptr(GFile) b = file_at(root, "src/b.txt");
    g_autoptr(GFile) dst = file_at(root, "dst");
    GList* list = g_list_append(g_list_append(NULL, a), b);

    Run run = run_op(SFE_OP_COPY, list, dst, SFE_ANSWER_CANCEL, false);
    g_assert_true(run.cancelled);
    g_assert_false(exists(root, "dst/b.txt")); /* stopped before b */
    g_list_free(list);

    remove_root(root);
}

int main(int argc, char** argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/fileops/numbered-name", test_numbered_name);
    g_test_add_func("/fileops/copy-tree", test_copy_tree);
    g_test_add_func("/fileops/conflicts", test_conflicts);
    g_test_add_func("/fileops/merge-apply-to-all", test_merge_apply_to_all);
    g_test_add_func("/fileops/move-and-delete", test_move_and_delete);
    g_test_add_func("/fileops/cancel", test_cancel_from_question);
    return g_test_run();
}
