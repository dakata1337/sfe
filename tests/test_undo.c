#include "rename.h"
#include "trash.h"
#include "undo.h"

#include <glib/gstdio.h>
#include <string.h>

static char* root; /* temporary; also XDG_DATA_HOME, so the real trash is never touched */

static GFile* at(const char* rel)
{
    g_autofree char* path = g_build_filename(root, rel, NULL);
    return g_file_new_for_path(path);
}

static GFile* make_file(const char* rel, const char* text)
{
    GFile* file = at(rel);
    g_autofree char* path = g_file_get_path(file);
    g_autofree char* dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_file_set_contents(path, text, -1, NULL);
    return file;
}

static bool there(const char* rel)
{
    g_autoptr(GFile) file = at(rel);
    return g_file_query_exists(file, NULL);
}

static void on_question(SfeFileOp* op, int kind, GFile* src, GFile* dest, const char* error, gpointer data)
{
    (void)kind, (void)src, (void)dest, (void)error, (void)data;
    sfe_file_op_answer(op, SFE_ANSWER_RENAME, true); /* keep both */
}

static void on_finished(SfeFileOp* op, GError* error, gpointer data)
{
    (void)op;
    g_assert_no_error(error);
    g_main_loop_quit(data);
}

static void run(SfeFileOp* op)
{
    GMainLoop* loop = g_main_loop_new(NULL, FALSE);
    g_signal_connect(op, "question", G_CALLBACK(on_question), NULL);
    g_signal_connect(op, "finished", G_CALLBACK(on_finished), loop);
    sfe_file_op_start(op);
    g_main_loop_run(loop);
    g_main_loop_unref(loop);
}

/* Runs a job and records it, like the window does. */
static SfeUndoEntry* run_recorded(SfeOpKind kind, GList* sources, GFile* dest)
{
    g_autoptr(SfeFileOp) op = sfe_file_op_new(kind, sources, dest);
    run(op);
    return sfe_undo_record_op(op);
}

static void undo(SfeUndoEntry* entry, guint expect_missing)
{
    g_autoptr(GError) error = NULL;
    SfeFileOp* job = NULL;
    guint missing = 99;
    g_assert_true(sfe_undo_apply(entry, &job, &missing, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(missing, ==, expect_missing);
    if (job != NULL) {
        run(job);
        g_object_unref(job);
    }
}

/* A copy that had to "keep both" is undone by trashing the copy only. */
static void test_copy(void)
{
    g_autoptr(GFile) src = make_file("copy/a.txt", "new");
    g_autoptr(GFile) old = make_file("copy/dest/a.txt", "old");
    g_autoptr(GFile) dest = at("copy/dest");
    GList* list = g_list_append(NULL, src);

    SfeUndoEntry* e = run_recorded(SFE_OP_COPY, list, dest);
    g_list_free(list);
    g_assert_nonnull(e);
    g_assert_true(there("copy/dest/a (2).txt"));
    g_autofree char* text = sfe_undo_describe(e);
    g_assert_true(g_str_has_prefix(text, "copy a.txt to "));

    undo(e, 0);
    g_assert_false(there("copy/dest/a (2).txt"));
    g_assert_true(there("copy/dest/a.txt"));
    g_assert_true(there("copy/a.txt"));
    g_assert_null(sfe_undo_find(0)); /* the history is empty again */
}

/* A folder moved into an existing one merges; undo moves back only what
 * came from it. */
static void test_move_merge(void)
{
    g_autoptr(GFile) x = make_file("move/src/dir/x", "x");
    g_autoptr(GFile) y = make_file("move/dst/dir/y", "y");
    g_autoptr(GFile) dir = at("move/src/dir");
    g_autoptr(GFile) dst = at("move/dst");
    GList* list = g_list_append(NULL, dir);

    SfeUndoEntry* e = run_recorded(SFE_OP_MOVE, list, dst);
    g_list_free(list);
    g_assert_nonnull(e);
    g_assert_cmpuint(e->pairs->len, ==, 1); /* dir/x, not dir */
    g_assert_true(there("move/dst/dir/x"));
    g_assert_false(there("move/src/dir"));

    undo(e, 0);
    g_assert_true(there("move/src/dir/x"));
    g_assert_false(there("move/dst/dir/x"));
    g_assert_true(there("move/dst/dir/y"));
}

static void test_trash(void)
{
    g_autoptr(GFile) file = make_file("trash/notes.txt", "hello");
    GList* list = g_list_append(NULL, file);
    SfeUndoEntry* e = run_recorded(SFE_OP_TRASH, list, NULL);
    g_list_free(list);
    g_assert_false(there("trash/notes.txt"));

    undo(e, 0);
    g_assert_true(there("trash/notes.txt"));

    /* Gone from the trash meanwhile: nothing to undo. */
    list = g_list_append(NULL, file);
    e = run_recorded(SFE_OP_TRASH, list, NULL);
    g_list_free(list);
    g_autoptr(GFile) item = sfe_trash_find(file);
    g_assert_nonnull(item);
    g_assert_true(g_file_delete(item, NULL, NULL));
    g_autoptr(GError) error = NULL;
    SfeFileOp* job = NULL;
    g_assert_false(sfe_undo_apply(e, &job, NULL, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_assert_null(sfe_undo_find(0));
}

/* Renames are undone together, swaps included. */
static void test_rename(void)
{
    g_autoptr(GFile) a = make_file("rename/a", "A");
    g_autoptr(GFile) b = make_file("rename/b", "B");
    GFile* files[] = { a, b };
    char* names[] = { "b", "a" };
    g_assert_true(sfe_rename_files(files, names, 2, NULL));

    GPtrArray* pairs = g_ptr_array_new();
    g_ptr_array_add(pairs, sfe_undo_pair_new(a, b));
    g_ptr_array_add(pairs, sfe_undo_pair_new(b, a));
    SfeUndoEntry* e = sfe_undo_record(SFE_UNDO_RENAME, pairs);
    g_autofree char* text = sfe_undo_describe(e);
    g_assert_cmpstr(text, ==, "rename 2 items");

    undo(e, 0);
    g_autofree char* content = NULL;
    g_autofree char* path = g_file_get_path(a);
    g_assert_true(g_file_get_contents(path, &content, NULL, NULL));
    g_assert_cmpstr(content, ==, "A");
}

/* New files go to the trash; ones already gone are counted. */
static void test_create(void)
{
    g_autoptr(GFile) made = make_file("create/new.txt", "");
    g_autoptr(GFile) gone = at("create/gone.txt");
    GPtrArray* pairs = g_ptr_array_new();
    g_ptr_array_add(pairs, sfe_undo_pair_new(NULL, made));
    g_ptr_array_add(pairs, sfe_undo_pair_new(NULL, gone));
    SfeUndoEntry* e = sfe_undo_record(SFE_UNDO_CREATE, pairs);
    undo(e, 1);
    g_assert_false(there("create/new.txt"));
}

static void test_history(void)
{
    sfe_undo_clear();
    g_assert_null(sfe_undo_find(0));
    g_assert_null(sfe_undo_record(SFE_UNDO_COPY, g_ptr_array_new())); /* nothing done, nothing kept */

    g_autoptr(GFile) f = at("history/f");
    GPtrArray* one = g_ptr_array_new();
    g_ptr_array_add(one, sfe_undo_pair_new(NULL, f));
    guint first_id = sfe_undo_record(SFE_UNDO_CREATE, one)->id;
    for (int i = 0; i < SFE_UNDO_MAX; i++) {
        GPtrArray* link = g_ptr_array_new();
        g_ptr_array_add(link, sfe_undo_pair_new(NULL, f));
        sfe_undo_record(SFE_UNDO_LINK, link);
    }
    GPtrArray* all = sfe_undo_entries();
    g_assert_cmpuint(all->len, ==, SFE_UNDO_MAX);
    g_assert_null(sfe_undo_find(first_id)); /* the oldest went */
    SfeUndoEntry* newest = sfe_undo_find(0);
    g_assert_cmpuint(newest->id, ==, first_id + SFE_UNDO_MAX);
    g_autofree char* text = sfe_undo_describe(newest);
    g_assert_cmpstr(text, ==, "link f");
    sfe_undo_clear();
}

int main(int argc, char** argv)
{
    root = g_dir_make_tmp("sfe-undo-XXXXXX", NULL);
    g_setenv("XDG_DATA_HOME", root, TRUE); /* before GLib reads it */
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/undo/copy", test_copy);
    g_test_add_func("/undo/move-merge", test_move_merge);
    g_test_add_func("/undo/trash", test_trash);
    g_test_add_func("/undo/rename", test_rename);
    g_test_add_func("/undo/create", test_create);
    g_test_add_func("/undo/history", test_history);
    int status = g_test_run();

    g_autofree char* cmd = g_strdup_printf("rm -rf '%s'", root);
    g_spawn_command_line_sync(cmd, NULL, NULL, NULL, NULL);
    g_free(root);
    return status;
}
