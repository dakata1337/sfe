/* Folder models: panes on the same directory share its rows; a search or a
 * file-dialog filter gives a pane rows of its own; changes on disk reach
 * every pane. Needs a display (icons); skipped without one. */
#include "app.h"
#include "dirmodel.h"
#include <glib/gstdio.h>

/* dirmodel.c's icons read the config; the defaults do. */
const SfeConfig* sfe_config(void)
{
    static SfeConfig* cfg;
    if (cfg == NULL)
        cfg = sfe_config_new();
    return cfg;
}

typedef struct {
    int loaded;
    int added;
} Counts;

static void on_loaded(SfeDirModel* model, gpointer data)
{
    (void)model;
    ((Counts*)data)->loaded++;
}

static void on_added(SfeDirModel* model, const char* name, gpointer data)
{
    (void)model;
    (void)name;
    ((Counts*)data)->added++;
}

static gboolean timed_out(gpointer data)
{
    *(bool*)data = true;
    return G_SOURCE_REMOVE;
}

/* Runs the main loop until *value reaches `want` (or 5 s pass). */
static void wait_for(int* value, int want)
{
    bool late = false;
    guint timer = g_timeout_add(5000, timed_out, &late);
    while (*value < want && !late)
        g_main_context_iteration(NULL, TRUE);
    if (!late)
        g_source_remove(timer);
    g_assert_cmpint(*value, >=, want);
}

static int rows(SfeDirModel* model)
{
    GtkTreeModel* m = sfe_dir_model_get_model(model);
    return m ? gtk_tree_model_iter_n_children(m, NULL) : -1;
}

static void touch(const char* dir, const char* name)
{
    g_autofree char* path = g_build_filename(dir, name, NULL);
    g_assert_true(g_file_set_contents(path, "x", 1, NULL));
}

static SfeDirModel* open_model(GFile* dir, Counts* counts)
{
    SfeDirModel* model = sfe_dir_model_new();
    g_signal_connect(model, "loaded", G_CALLBACK(on_loaded), counts);
    g_signal_connect(model, "file-added", G_CALLBACK(on_added), counts);
    sfe_dir_model_load(model, dir);
    wait_for(&counts->loaded, 1);
    return model;
}

static void test_shared(void)
{
    g_autofree char* path = g_dir_make_tmp("sfe-dirmodel-XXXXXX", NULL);
    g_autofree char* other_path = g_dir_make_tmp("sfe-dirmodel-XXXXXX", NULL);
    for (int i = 0; i < 5; i++) {
        g_autofree char* name = g_strdup_printf("apple%d.txt", i);
        touch(path, name);
    }
    touch(path, "pear.txt");
    touch(other_path, "kiwi.txt");
    g_autoptr(GFile) dir = g_file_new_for_path(path);
    g_autoptr(GFile) other = g_file_new_for_path(other_path);

    Counts ca = { 0 }, cb = { 0 };
    SfeDirModel* a = open_model(dir, &ca);
    SfeDirModel* b = open_model(dir, &cb);

    /* The second pane shows the first one's rows: one copy. */
    g_assert_true(sfe_dir_model_get_model(a) == sfe_dir_model_get_model(b));
    g_assert_cmpint(rows(b), ==, 6);

    /* A search gives that pane rows of its own; the other keeps all. */
    sfe_dir_model_set_query(a, "pear");
    g_assert_cmpint(rows(a), ==, 1);
    g_assert_cmpint(rows(b), ==, 6);
    g_assert_true(sfe_dir_model_get_model(a) != sfe_dir_model_get_model(b));

    /* A new file reaches both: in the search only when it matches. */
    touch(path, "pear2.txt");
    touch(path, "plum.txt");
    wait_for(&cb.added, 2);
    wait_for(&ca.added, 1);
    g_assert_cmpint(rows(b), ==, 8);
    g_assert_cmpint(rows(a), ==, 2);
    int shown, matched, total;
    sfe_dir_model_get_counts(a, &shown, &matched, &total);
    g_assert_cmpint(shown, ==, 2);
    g_assert_cmpint(matched, ==, 2);
    g_assert_cmpint(total, ==, 8);

    /* A deleted one leaves both. */
    g_autofree char* pear = g_build_filename(path, "pear.txt", NULL);
    g_assert_cmpint(g_unlink(pear), ==, 0);
    while (rows(b) != 7 || rows(a) != 1)
        g_main_context_iteration(NULL, TRUE);
    sfe_dir_model_get_counts(a, &shown, &matched, &total);
    g_assert_cmpint(matched, ==, 1);

    /* The search ends: the shared rows again. */
    sfe_dir_model_set_query(a, NULL);
    g_assert_true(sfe_dir_model_get_model(a) == sfe_dir_model_get_model(b));

    /* One pane leaves: the other still follows the disk. */
    sfe_dir_model_load(a, other);
    wait_for(&ca.loaded, 4);
    g_assert_cmpint(rows(a), ==, 1);
    int before = cb.added;
    touch(path, "fig.txt");
    wait_for(&cb.added, before + 1);
    g_assert_cmpint(rows(b), ==, 8);

    /* A pane closed: a new one on the same folder still gets its files. */
    g_object_unref(b);
    Counts cc = { 0 };
    SfeDirModel* c = open_model(dir, &cc);
    g_assert_cmpint(rows(c), ==, 8);

    g_object_unref(a);
    g_object_unref(c);
    const char* const names[] = { "apple0.txt", "apple1.txt", "apple2.txt", "apple3.txt", "apple4.txt",
        "pear2.txt", "plum.txt", "fig.txt" };
    for (size_t i = 0; i < G_N_ELEMENTS(names); i++) {
        g_autofree char* p = g_build_filename(path, names[i], NULL);
        g_unlink(p);
    }
    g_autofree char* kiwi = g_build_filename(other_path, "kiwi.txt", NULL);
    g_unlink(kiwi);
    g_rmdir(path);
    g_rmdir(other_path);
}

int main(int argc, char** argv)
{
    g_test_init(&argc, &argv, NULL);
    if (!gtk_init_check(&argc, &argv)) {
        g_test_message("no display");
        return 77; /* skipped */
    }
    g_test_add_func("/dirmodel/shared", test_shared);
    return g_test_run();
}
