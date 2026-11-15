#include "config.h"
#include "util.h"

#include <glib/gstdio.h>
#include <string.h>

static SfeConfig* parse(const char* text)
{
    g_autoptr(GError) error = NULL;
    SfeConfig* cfg = sfe_config_parse(text, strlen(text), &error);
    g_assert_no_error(error);
    g_assert_nonnull(cfg);
    return cfg;
}

static void test_defaults_round_trip(void)
{
    g_autoptr(SfeConfig) def = sfe_config_new();
    g_autofree char* text = sfe_config_to_string(def);
    g_autoptr(SfeConfig) back = parse(text);
    g_autofree char* text2 = sfe_config_to_string(back);

    g_assert_cmpstr(text, ==, text2);
}

static void test_values_round_trip(void)
{
    g_autoptr(SfeConfig) cfg = sfe_config_new();

    g_free(cfg->start_path);
    cfg->start_path = g_strdup("/tmp/with \"quotes\" and \\ backslash\tand ünicode");
    cfg->show_hidden = true;
    cfg->ui_scale = 1.25;
    cfg->view = SFE_VIEW_LIST;
    cfg->icon_size = 96;
    cfg->sort_by = SFE_SORT_MODIFIED;
    cfg->sort_order = SFE_SORT_DESCENDING;
    g_ptr_array_add(cfg->bookmarks, g_strdup("~/Downloads"));
    g_ptr_array_add(cfg->bookmarks, g_strdup("/mnt/data"));
    g_hash_table_replace(cfg->open_with, g_strdup("mp4"), g_strdup("mpv %f"));
    g_hash_table_replace(cfg->open_with, g_strdup("tar.gz"), g_strdup("file-roller %f"));
    sfe_config_add_user_action(cfg, "Terminal", "foot -D %d", SFE_ACTION_FILES);
    sfe_config_add_user_action(cfg, "Terminal", "foot -D %f", SFE_ACTION_DIRECTORIES);

    g_autofree char* text = sfe_config_to_string(cfg);
    g_autoptr(SfeConfig) back = parse(text);

    g_assert_cmpstr(back->start_path, ==, cfg->start_path);
    g_assert_true(back->show_hidden);
    g_assert_cmpfloat(back->ui_scale, ==, 1.25);
    g_assert_cmpint(back->view, ==, SFE_VIEW_LIST);
    g_assert_cmpint(back->icon_size, ==, 96);
    g_assert_cmpint(back->sort_by, ==, SFE_SORT_MODIFIED);
    g_assert_cmpint(back->sort_order, ==, SFE_SORT_DESCENDING);
    g_assert_nonnull(strstr(text, "sort_order = \"descending\""));
    g_assert_cmpuint(back->bookmarks->len, ==, 2);
    g_assert_cmpstr(g_ptr_array_index(back->bookmarks, 1), ==, "/mnt/data");
    g_assert_cmpstr(g_hash_table_lookup(back->open_with, "tar.gz"), ==, "file-roller %f");
    g_assert_cmpuint(back->actions->len, ==, 2);
    SfeUserAction* files = g_ptr_array_index(back->actions, 0);
    SfeUserAction* dirs = g_ptr_array_index(back->actions, 1);
    g_assert_cmpstr(files->command, ==, "foot -D %d");
    g_assert_cmpuint(files->applies_to, ==, SFE_ACTION_FILES);
    g_assert_cmpstr(dirs->command, ==, "foot -D %f");
    g_assert_cmpuint(dirs->applies_to, ==, SFE_ACTION_DIRECTORIES);

    g_autoptr(SfeConfig) copy = sfe_config_copy(cfg);
    g_autofree char* copy_text = sfe_config_to_string(copy);
    g_assert_cmpstr(copy_text, ==, text);
}

/* Bad values are ignored with a warning instead of failing the whole file. */
static void test_invalid_values_keep_defaults(void)
{
    if (g_test_subprocess()) {
        g_log_set_always_fatal(G_LOG_FATAL_MASK); /* report warnings, don't abort */
        g_autoptr(SfeConfig) cfg = parse(
            "[general]\n"
            "show_hidden = \"yes\"\n"
            "ui_scale = 10.0\n"
            "icon_size = 64.5\n"
            "view = \"columns\"\n"
            "sort_by = \"SIZE\"\n"
            "typo_key = 1\n"
            "[open]\n"
            "\".PDF\" = \"zathura %f\"\n");
        g_assert_false(cfg->show_hidden);
        g_assert_cmpfloat(cfg->ui_scale, ==, 1.0);
        g_assert_cmpint(cfg->icon_size, ==, 64);
        g_assert_cmpint(cfg->view, ==, SFE_VIEW_GRID);
        g_assert_cmpint(cfg->sort_by, ==, SFE_SORT_SIZE);
        g_assert_cmpstr(g_hash_table_lookup(cfg->open_with, "pdf"), ==, "zathura %f");

        /* Each skipped entry is kept for the notification, with its line. */
        g_assert_cmpuint(cfg->problems->len, ==, 5);
        g_assert_cmpstr(g_ptr_array_index(cfg->problems, 0), ==, "line 2: invalid value for general.show_hidden, using the default");
        g_assert_cmpstr(g_ptr_array_index(cfg->problems, 4), ==, "line 7: unknown option general.typo_key");
        return;
    }
    g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
    g_test_trap_assert_passed();
    g_test_trap_assert_stderr("*invalid value for general.show_hidden*");
    g_test_trap_assert_stderr("*unknown option general.typo_key*");
}

/* sort_order: ascending unless set; any case; a bad value keeps the
 * default; files from before sort_order existed still load. */
static void test_sort_order(void)
{
    g_autoptr(SfeConfig) def = sfe_config_new();
    g_assert_cmpint(def->sort_order, ==, SFE_SORT_ASCENDING);
    g_autoptr(SfeConfig) old = parse("[general]\nsort_by = \"type\"\n");
    g_assert_cmpint(old->sort_by, ==, SFE_SORT_TYPE);
    g_assert_cmpint(old->sort_order, ==, SFE_SORT_ASCENDING);
    g_autoptr(SfeConfig) desc = parse("[general]\nsort_order = \"Descending\"\n");
    g_assert_cmpint(desc->sort_order, ==, SFE_SORT_DESCENDING);

    if (g_test_subprocess()) {
        g_log_set_always_fatal(G_LOG_FATAL_MASK);
        g_autoptr(SfeConfig) bad = parse("[general]\nsort_order = \"upwards\"\n");
        g_assert_cmpint(bad->sort_order, ==, SFE_SORT_ASCENDING);
        g_assert_cmpuint(bad->problems->len, ==, 1);
        return;
    }
    g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
    g_test_trap_assert_passed();
    g_test_trap_assert_stderr("*invalid value for general.sort_order*");
}

/* The names `:sort` and the menu use. */
static void test_sort_names(void)
{
    SfeSortKey key = SFE_SORT_NAME;
    SfeSortOrder order = SFE_SORT_ASCENDING;
    g_assert_true(sfe_sort_key_from_name("Modified", &key));
    g_assert_cmpint(key, ==, SFE_SORT_MODIFIED);
    g_assert_false(sfe_sort_key_from_name("date", &key));
    g_assert_false(sfe_sort_key_from_name(NULL, &key));
    g_assert_cmpint(key, ==, SFE_SORT_MODIFIED); /* untouched on failure */
    g_assert_true(sfe_sort_order_from_name("desc", &order));
    g_assert_cmpint(order, ==, SFE_SORT_DESCENDING);
    g_assert_true(sfe_sort_order_from_name("ASC", &order));
    g_assert_cmpint(order, ==, SFE_SORT_ASCENDING);
    g_assert_false(sfe_sort_order_from_name("down", &order));
    for (int k = SFE_SORT_NAME; k <= SFE_SORT_TYPE; k++) {
        SfeSortKey back;
        g_assert_true(sfe_sort_key_from_name(sfe_sort_key_name(k), &back));
        g_assert_cmpint(back, ==, k);
    }
    g_assert_cmpstr(sfe_sort_order_name(SFE_SORT_DESCENDING), ==, "descending");
    g_assert_cmpstr(sfe_sort_order_name(SFE_SORT_ASCENDING), ==, "ascending");
}

/* The older [[actions]] form still works and applies to both kinds. */
static void test_old_style_actions(void)
{
    g_autoptr(SfeConfig) cfg = parse("[[actions]]\nname = \"Edit\"\ncommand = \"vim %f\"\n");
    g_assert_cmpuint(cfg->actions->len, ==, 1);
    SfeUserAction* a = g_ptr_array_index(cfg->actions, 0);
    g_assert_cmpuint(a->applies_to, ==, SFE_ACTION_ANY);

    /* Saved as one entry per group. */
    g_autofree char* text = sfe_config_to_string(cfg);
    g_assert_nonnull(strstr(text, "[[actions.files]]"));
    g_assert_nonnull(strstr(text, "[[actions.directories]]"));
}

/* [keys]: kept in order, saved and read back; non-strings are problems. */
static void test_keys(void)
{
    if (g_test_subprocess()) {
        g_log_set_always_fatal(G_LOG_FATAL_MASK);
        g_autoptr(SfeConfig) bad = parse("[keys]\nq = 3\n\"<C-bogus>\" = \"x\"\n");
        g_assert_cmpuint(bad->keys->len, ==, 0);
        g_assert_cmpuint(bad->problems->len, ==, 2);
        return;
    }
    g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
    g_test_trap_assert_passed();
    g_test_trap_assert_stderr("*keys.\"q\" must be a command*");
    g_test_trap_assert_stderr("*unknown key <C-bogus>*");

    g_autoptr(SfeConfig) cfg = parse(
        "[keys]\n"
        "\"<C-w>v\" = \"split vertical\"\n"
        "x = \"\"\n");
    g_assert_cmpuint(cfg->keys->len, ==, 2);
    SfeKeyOverride* first = g_ptr_array_index(cfg->keys, 0);
    g_assert_cmpstr(first->sequence, ==, "<C-w>v");
    g_assert_cmpstr(first->command, ==, "split vertical");
    SfeKeyOverride* unbind = g_ptr_array_index(cfg->keys, 1);
    g_assert_cmpstr(unbind->command, ==, "");

    g_autofree char* text = sfe_config_to_string(cfg);
    g_autoptr(SfeConfig) back = parse(text);
    g_assert_cmpuint(back->keys->len, ==, 2);
    SfeKeyOverride* again = g_ptr_array_index(back->keys, 0);
    g_assert_cmpstr(again->sequence, ==, "<C-w>v");
    g_assert_cmpuint(back->problems->len, ==, 0);
}

/* [keys.visual]: its own bindings; the same key may differ from [keys]. */
static void test_visual_keys(void)
{
    g_autoptr(SfeConfig) cfg = parse(
        "[keys]\n"
        "y = \"open\"\n"
        "[keys.visual]\n"
        "y = \"cut\"\n");
    g_assert_cmpuint(cfg->keys->len, ==, 2);
    g_assert_cmpuint(cfg->problems->len, ==, 0);
    SfeKeyOverride* normal = g_ptr_array_index(cfg->keys, 0);
    SfeKeyOverride* visual = g_ptr_array_index(cfg->keys, 1);
    g_assert_false(normal->visual);
    g_assert_true(visual->visual);
    g_assert_cmpstr(visual->command, ==, "cut");

    g_autofree char* text = sfe_config_to_string(cfg);
    g_assert_nonnull(strstr(text, "[keys.visual]"));
    g_autoptr(SfeConfig) back = parse(text);
    g_assert_cmpuint(back->keys->len, ==, 2);
    g_assert_cmpuint(back->problems->len, ==, 0);
    g_assert_true(((SfeKeyOverride*)g_ptr_array_index(back->keys, 1))->visual);
}

static void test_syntax_error(void)
{
    g_autoptr(GError) error = NULL;
    const char* text = "[general\nshow_hidden = true\n";
    SfeConfig* cfg = sfe_config_parse(text, strlen(text), &error);

    g_assert_null(cfg);
    g_assert_error(error, SFE_CONFIG_ERROR, 0);
}

static void test_missing_file_gives_defaults(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(SfeConfig) cfg = sfe_config_load("/nonexistent/sfe/config.toml", &error);

    g_assert_no_error(error);
    g_assert_cmpstr(cfg->start_path, ==, "~");
}

static void test_save_and_load(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char* dir = g_dir_make_tmp("sfe-test-XXXXXX", &error);
    g_assert_no_error(error);
    g_autofree char* path = g_build_filename(dir, "nested", "config.toml", NULL);
    g_autoptr(SfeConfig) cfg = sfe_config_new();
    cfg->thumbnail_max_mb = 7;

    g_assert_true(sfe_config_save(cfg, path, &error));
    g_assert_no_error(error);
    g_autoptr(SfeConfig) back = sfe_config_load(path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(back->thumbnail_max_mb, ==, 7);

    g_autofree char* nested = g_path_get_dirname(path);
    g_remove(path);
    g_rmdir(nested);
    g_rmdir(dir);
}

static void test_home_paths(void)
{
    const char* home = g_get_home_dir();
    g_autofree char* docs = g_build_filename(home, "Documents", NULL);
    g_autofree char* a = sfe_expand_home("~");
    g_autofree char* b = sfe_expand_home("~/Documents");
    g_autofree char* c = sfe_expand_home("~user/x");
    g_autofree char* d = sfe_contract_home(docs);

    g_assert_cmpstr(a, ==, home);
    g_assert_cmpstr(b, ==, docs);
    g_assert_cmpstr(c, ==, "~user/x");
    g_assert_cmpstr(d, ==, "~/Documents");
}

int main(int argc, char** argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/config/defaults-round-trip", test_defaults_round_trip);
    g_test_add_func("/config/values-round-trip", test_values_round_trip);
    g_test_add_func("/config/invalid-values", test_invalid_values_keep_defaults);
    g_test_add_func("/config/sort-order", test_sort_order);
    g_test_add_func("/config/sort-names", test_sort_names);
    g_test_add_func("/config/old-style-actions", test_old_style_actions);
    g_test_add_func("/config/keys", test_keys);
    g_test_add_func("/config/visual-keys", test_visual_keys);
    g_test_add_func("/config/syntax-error", test_syntax_error);
    g_test_add_func("/config/missing-file", test_missing_file_gives_defaults);
    g_test_add_func("/config/save-and-load", test_save_and_load);
    g_test_add_func("/util/home-paths", test_home_paths);
    return g_test_run();
}
