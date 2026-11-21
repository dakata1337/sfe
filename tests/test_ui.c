/* Widget tests: the real application and windows, driven the way a user
 * drives them (key presses, : commands, the Settings widgets), on a
 * headless GTK display (broadwayd) in a throwaway home folder.
 *
 * Skipped (exit 77) without broadwayd. Run alone with
 *   meson test -C build ui -v
 * and in a decimal-comma language / another theme by the "ui-bg" and
 * "ui-theme" tests (tests/meson.build). */
#include "actions.h"
#include "app.h"
#include "commands.h"
#include "pane.h"
#include "prefs.h"
#include "split.h"
#include "undo.h"
#include "window.h"

#include <glib/gstdio.h>
#include <locale.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

static char* root; /* HOME, XDG_* and the test folders are in here */
static GPid broadway;
static SfeApplication* app;

/* ------------------------------------------------------------------------ */
/* Harness                                                                   */
/* ------------------------------------------------------------------------ */

/* Runs the main loop until `cond` holds, at most 5 s. */
#define WAIT_UNTIL(cond)                                                       \
    do {                                                                       \
        gint64 end_ = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;             \
        while (!(cond)) {                                                      \
            if (g_get_monotonic_time() > end_)                                 \
                g_error("timed out waiting for: %s", #cond);                   \
            if (!g_main_context_iteration(NULL, FALSE))                        \
                g_usleep(2000);                                                \
        }                                                                      \
    } while (0)

/* Lets pending work (idle handlers, redraws, timeouts due) run. */
static void settle(int ms)
{
    gint64 end = g_get_monotonic_time() + ms * 1000;
    while (g_get_monotonic_time() < end)
        if (!g_main_context_iteration(NULL, FALSE))
            g_usleep(2000);
}

/* A fresh folder with a few files: a.txt b.txt c.png, and folder sub/. */
static char* make_folder(const char* name)
{
    char* dir = g_build_filename(root, name, NULL);
    char* sub = g_build_filename(dir, "sub", NULL);
    g_mkdir_with_parents(sub, 0755);
    const char* files[] = { "a.txt", "b.txt", "c.png" };
    for (size_t i = 0; i < G_N_ELEMENTS(files); i++) {
        g_autofree char* path = g_build_filename(dir, files[i], NULL);
        g_file_set_contents(path, files[i], -1, NULL);
    }
    g_free(sub);
    return dir;
}

static bool exists(const char* dir, const char* name)
{
    g_autofree char* path = g_build_filename(dir, name, NULL);
    return g_file_test(path, G_FILE_TEST_EXISTS);
}

static SfePane* active(SfeWindow* win)
{
    return sfe_window_get_active_pane(win);
}

static int n_entries(SfePane* pane)
{
    GList* files = sfe_pane_get_all_files(pane);
    int n = (int)g_list_length(files);
    g_list_free_full(files, g_object_unref);
    return n;
}

static bool showing(SfePane* pane, const char* dir)
{
    g_autofree char* path = g_file_get_path(sfe_pane_get_dir(pane));
    return g_strcmp0(path, dir) == 0;
}

static SfeWindow* open_window(const char* dir)
{
    g_autoptr(GFile) file = g_file_new_for_path(dir);
    SfeWindow* win = sfe_window_new(app, file);
    gtk_window_present(GTK_WINDOW(win));
    WAIT_UNTIL(gtk_widget_get_mapped(GTK_WIDGET(win)) && n_entries(active(win)) > 0);
    return win;
}

static void close_window(SfeWindow* win)
{
    gtk_widget_destroy(GTK_WIDGET(win));
    settle(50);
}

static void run(SfeWindow* win, const char* line)
{
    g_autoptr(GError) error = NULL;
    if (!sfe_command_run(win, line, &error))
        g_error(":%s failed: %s", line, error ? error->message : "?");
}

/* A key press and release, delivered like one from the display. */
static void key(SfeWindow* win, guint keyval, GdkModifierType mods)
{
    GdkDisplay* display = gtk_widget_get_display(GTK_WIDGET(win));
    GdkDevice* keyboard = gdk_seat_get_keyboard(gdk_display_get_default_seat(display));
    GdkKeymapKey* keys = NULL;
    int n = 0;
    guint16 code = 0;
    if (gdk_keymap_get_entries_for_keyval(gdk_keymap_get_for_display(display), keyval, &keys, &n) && n > 0)
        code = (guint16)keys[0].keycode;
    g_free(keys);
    for (int i = 0; i < 2; i++) {
        GdkEvent* e = gdk_event_new(i == 0 ? GDK_KEY_PRESS : GDK_KEY_RELEASE);
        e->key.window = g_object_ref(gtk_widget_get_window(GTK_WIDGET(win)));
        e->key.send_event = TRUE;
        e->key.time = GDK_CURRENT_TIME;
        e->key.state = mods;
        e->key.keyval = keyval;
        e->key.hardware_keycode = code;
        gdk_event_set_device(e, keyboard);
        gtk_main_do_event(e);
        gdk_event_free(e);
    }
    settle(20);
}

static void type(SfeWindow* win, const char* text)
{
    for (const char* p = text; *p; p = g_utf8_next_char(p))
        key(win, gdk_unicode_to_keyval(g_utf8_get_char(p)), 0);
}

/* ------------------------------------------------------------------------ */
/* Tests                                                                     */
/* ------------------------------------------------------------------------ */

static void test_opens_folder(void)
{
    g_autofree char* dir = make_folder("opens");
    SfeWindow* win = open_window(dir);
    g_assert_true(showing(active(win), dir));
    g_assert_cmpint(n_entries(active(win)), ==, 4);
    close_window(win);
}

static void test_commands(void)
{
    g_autofree char* dir = make_folder("commands");
    SfeWindow* win = open_window(dir);
    SfePane* pane = active(win);

    run(win, "mkdir New Folder"); /* one argument: the whole name */
    WAIT_UNTIL(n_entries(pane) == 5);
    g_assert_true(exists(dir, "New Folder"));

    run(win, "cd New\\ Folder"); /* quoted, as completion inserts it */
    g_autofree char* inside = g_build_filename(dir, "New Folder", NULL);
    WAIT_UNTIL(showing(pane, inside));
    run(win, "cd ..");
    WAIT_UNTIL(showing(pane, dir));

    guint steps = sfe_undo_entries()->len;
    run(win, "cp a.txt sub");
    g_autofree char* sub = g_build_filename(dir, "sub", NULL);
    WAIT_UNTIL(sfe_undo_entries()->len == steps + 1); /* the job finished (the file shows up before) */
    g_assert_true(exists(sub, "a.txt"));
    run(win, "undo");
    WAIT_UNTIL(!exists(sub, "a.txt"));
    g_assert_true(exists(dir, "a.txt"));
    g_assert_true(exists(dir, "New Folder")); /* only the copy was undone */
    close_window(win);
}

static void test_view_keys(void)
{
    g_autofree char* dir = make_folder("view-keys");
    SfeWindow* win = open_window(dir);
    key(win, GDK_KEY_2, GDK_CONTROL_MASK);
    g_assert_cmpint(sfe_pane_get_view(active(win)), ==, SFE_VIEW_LIST);
    key(win, GDK_KEY_1, GDK_CONTROL_MASK);
    g_assert_cmpint(sfe_pane_get_view(active(win)), ==, SFE_VIEW_GRID);
    run(win, "view list");
    g_assert_cmpint(sfe_pane_get_view(active(win)), ==, SFE_VIEW_LIST);
    close_window(win);
}

/* vim keys: G jumps to the last item (selecting just it), dd cuts it, :paste
 * moves it; :trash puts a file in the trash. */
static void test_vim_cut_paste_trash(void)
{
    g_autofree char* dir = make_folder("vim");
    g_autofree char* sub = g_build_filename(dir, "sub", NULL);
    SfeWindow* win = open_window(dir);
    SfePane* pane = active(win);
    sfe_pane_focus_view(pane);
    key(win, GDK_KEY_G, GDK_SHIFT_MASK); /* the last item: c.png (folders first) */
    GList* sel = sfe_pane_get_selected_files(pane);
    g_assert_cmpuint(g_list_length(sel), ==, 1);
    g_autofree char* name = g_file_get_basename(sel->data);
    g_list_free_full(sel, g_object_unref);
    g_assert_cmpstr(name, ==, "c.png");

    type(win, "dd");
    run(win, "paste sub");
    WAIT_UNTIL(exists(sub, "c.png") && !exists(dir, "c.png"));

    run(win, "trash a.txt");
    WAIT_UNTIL(!exists(dir, "a.txt"));
    g_autofree char* trashed = g_build_filename(root, "data", "Trash", "files", "a.txt", NULL);
    g_assert_true(g_file_test(trashed, G_FILE_TEST_EXISTS));
    close_window(win);
}

static void test_split(void)
{
    g_autofree char* dir = make_folder("split");
    SfeWindow* win = open_window(dir);
    SfeSplit* split = sfe_window_get_split(win);
    run(win, "split vertical");
    WAIT_UNTIL(sfe_split_get_n_panes(split) == 2);
    run(win, "q");
    WAIT_UNTIL(sfe_split_get_n_panes(split) == 1);
    close_window(win);
}

static void collect_rows(GtkWidget* w, gpointer list)
{
    if (GTK_IS_LIST_BOX_ROW(w))
        g_ptr_array_add(list, w);
    if (GTK_IS_CONTAINER(w))
        gtk_container_forall(GTK_CONTAINER(w), collect_rows, list);
}

static GtkWidget* find_class(GtkWidget* w, const char* name)
{
    if (gtk_style_context_has_class(gtk_widget_get_style_context(w), name))
        return w;
    GtkWidget* found = NULL;
    if (GTK_IS_CONTAINER(w)) {
        GList* kids = gtk_container_get_children(GTK_CONTAINER(w));
        for (GList* l = kids; l != NULL && found == NULL; l = l->next)
            found = find_class(l->data, name);
        g_list_free(kids);
    }
    return found;
}

/* Sidebar rows at rest are transparent, whatever the theme paints rows
 * with (the ui-theme test runs this with a Breeze-like theme). */
static void test_sidebar_rows_transparent(void)
{
    g_autofree char* dir = make_folder("sidebar");
    SfeWindow* win = open_window(dir);
    GtkWidget* sidebar = find_class(GTK_WIDGET(win), "sfe-sidebar");
    g_assert_nonnull(sidebar);
    g_autoptr(GPtrArray) rows = g_ptr_array_new();
    collect_rows(sidebar, rows);
    g_assert_cmpuint(rows->len, >, 2);
    for (guint i = 0; i < rows->len; i++) {
        GtkWidget* row = g_ptr_array_index(rows, i);
        GtkStyleContext* style = gtk_widget_get_style_context(row);
        GtkStateFlags state = gtk_style_context_get_state(style);
        if (state & (GTK_STATE_FLAG_SELECTED | GTK_STATE_FLAG_PRELIGHT))
            continue;
        GdkRGBA* bg = NULL;
        gtk_style_context_get(style, state, "background-color", &bg, NULL);
        g_assert_cmpfloat(bg->alpha, ==, 0.0);
        gdk_rgba_free(bg);
    }
    close_window(win);
}

/* Every widget in the Settings window that edits an option (found by
 * their "sfe-option" data), with its internal children. */
static void collect_options(GtkWidget* w, gpointer list)
{
    if (g_object_get_data(G_OBJECT(w), "sfe-option") != NULL
        && (GTK_IS_SWITCH(w) || GTK_IS_COMBO_BOX(w) || GTK_IS_SPIN_BUTTON(w)))
        g_ptr_array_add(list, w);
    if (GTK_IS_CONTAINER(w))
        gtk_container_forall(GTK_CONTAINER(w), collect_options, list);
}

static GtkWidget* settings_window(void)
{
    GList* tops = gtk_window_list_toplevels();
    GtkWidget* found = NULL;
    for (GList* l = tops; l != NULL; l = l->next)
        if (gtk_style_context_has_class(gtk_widget_get_style_context(l->data), "sfe-settings"))
            found = l->data;
    g_list_free(tops);
    return found;
}

/* Changes every option through its widget, as clicking would: each change
 * saves the config (the copy that aborted in decimal-comma languages) and
 * must leave a file that loads again with the new value. */
static void test_settings_every_option(void)
{
    g_autofree char* dir = make_folder("settings");
    SfeWindow* win = open_window(dir);
    sfe_show_preferences(GTK_WIDGET(win));
    WAIT_UNTIL(settings_window() != NULL);
    GtkWidget* settings = settings_window();

    g_autoptr(GPtrArray) widgets = g_ptr_array_new();
    collect_options(settings, widgets);
    g_assert_cmpuint(widgets->len, >, 10);
    g_autofree char* path = g_build_filename(root, "config", "sfe", "config.toml", NULL);
    for (guint i = 0; i < widgets->len; i++) {
        GtkWidget* w = g_ptr_array_index(widgets, i);
        const SfeOption* opt = g_object_get_data(G_OBJECT(w), "sfe-option");
        if (GTK_IS_SWITCH(w)) {
            gtk_switch_set_active(GTK_SWITCH(w), !gtk_switch_get_active(GTK_SWITCH(w)));
        } else if (GTK_IS_COMBO_BOX(w)) {
            GtkTreeModel* model = gtk_combo_box_get_model(GTK_COMBO_BOX(w));
            int n = gtk_tree_model_iter_n_children(model, NULL);
            gtk_combo_box_set_active(GTK_COMBO_BOX(w), (gtk_combo_box_get_active(GTK_COMBO_BOX(w)) + 1) % n);
        } else {
            gtk_spin_button_spin(GTK_SPIN_BUTTON(w), GTK_SPIN_STEP_FORWARD, 1);
        }
        settle(20);

        g_autoptr(GError) error = NULL;
        g_autofree char* text = NULL;
        g_assert_true(g_file_get_contents(path, &text, NULL, &error));
        g_autoptr(SfeConfig) saved = sfe_config_parse(text, strlen(text), &error);
        if (saved == NULL)
            g_error("after changing %s the saved config does not load: %s", opt->key, error->message);
    }
    gtk_widget_destroy(settings);
    close_window(win);
}

/* ------------------------------------------------------------------------ */

static void stop_broadway(void)
{
    if (broadway != 0) {
        kill(broadway, SIGTERM);
        g_spawn_close_pid(broadway);
        broadway = 0;
    }
}

/* In broadwayd: it goes when the test does, also after an abort. */
static void die_with_parent(gpointer data)
{
    (void)data;
#ifdef __linux__
    prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
}

/* broadwayd on a free display; false when it is not installed. It is
 * ready when it prints "Listening on ..." (its socket is abstract: no file
 * to wait for). Its output comes through a pipe, so it does not hold the
 * test's output open after we stop it. */
static bool start_broadway(void)
{
    g_autofree char* program = g_find_program_in_path("broadwayd");
    if (program == NULL)
        return false;
    int display = 100 + (int)(getpid() % 800);
    g_autofree char* arg = g_strdup_printf(":%d", display);
    char* argv[] = { program, arg, NULL };
    int out = -1;
    if (!g_spawn_async_with_pipes(NULL, argv, NULL, G_SPAWN_STDERR_TO_DEV_NULL | G_SPAWN_DO_NOT_REAP_CHILD,
            die_with_parent, NULL, &broadway, NULL, &out, NULL, NULL))
        return false;
    GIOChannel* channel = g_io_channel_unix_new(out);
    g_io_channel_set_close_on_unref(channel, TRUE);
    char* line = NULL;
    bool ready = g_io_channel_read_line(channel, &line, NULL, NULL, NULL) == G_IO_STATUS_NORMAL
        && line != NULL && g_str_has_prefix(line, "Listening on");
    g_free(line);
    g_io_channel_unref(channel);
    if (!ready) {
        stop_broadway();
        return false;
    }
    g_setenv("BROADWAY_DISPLAY", arg, TRUE);
    return true;
}

static void remove_tree(const char* path)
{
    GDir* dir = g_file_test(path, G_FILE_TEST_IS_SYMLINK) ? NULL : g_dir_open(path, 0, NULL);
    const char* name;
    while (dir != NULL && (name = g_dir_read_name(dir)) != NULL) {
        g_autofree char* child = g_build_filename(path, name, NULL);
        remove_tree(child);
    }
    if (dir != NULL)
        g_dir_close(dir);
    g_remove(path);
}

int main(int argc, char** argv)
{
    setlocale(LC_ALL, ""); /* (the ui-bg test runs this in a decimal-comma language) */
    root = g_dir_make_tmp("sfe-ui-XXXXXX", NULL);
    g_assert_nonnull(root); /* before GLib reads any of these: */
    g_setenv("HOME", root, TRUE);
    g_autofree char* config = g_build_filename(root, "config", NULL);
    g_autofree char* data = g_build_filename(root, "data", NULL);
    g_autofree char* cache = g_build_filename(root, "cache", NULL);
    g_autofree char* state = g_build_filename(root, "state", NULL);
    g_setenv("XDG_CONFIG_HOME", config, TRUE);
    g_setenv("XDG_DATA_HOME", data, TRUE);
    g_setenv("XDG_CACHE_HOME", cache, TRUE);
    g_setenv("XDG_STATE_HOME", state, TRUE);
    g_setenv("GSETTINGS_BACKEND", "memory", TRUE);
    g_setenv("DBUS_SESSION_BUS_ADDRESS", "disabled:", TRUE);
    g_setenv("GDK_BACKEND", "broadway", TRUE);
    g_setenv("NO_AT_BRIDGE", "1", TRUE);

    if (!start_broadway()) {
        g_print("1..0 # SKIP broadwayd is not installed or did not start\n");
        remove_tree(root);
        return 77;
    }
    g_test_init(&argc, &argv, NULL);

    app = sfe_application_new();
    g_application_set_flags(G_APPLICATION(app), g_application_get_flags(G_APPLICATION(app)) | G_APPLICATION_NON_UNIQUE);
    g_autoptr(GError) error = NULL;
    if (!g_application_register(G_APPLICATION(app), NULL, &error)) /* runs startup: GTK, config, theme */
        g_error("cannot start: %s", error->message);
    g_application_hold(G_APPLICATION(app));

    g_test_add_func("/ui/opens-folder", test_opens_folder);
    g_test_add_func("/ui/commands", test_commands);
    g_test_add_func("/ui/view-keys", test_view_keys);
    g_test_add_func("/ui/vim-cut-paste-trash", test_vim_cut_paste_trash);
    g_test_add_func("/ui/split", test_split);
    g_test_add_func("/ui/settings-every-option", test_settings_every_option);
    g_test_add_func("/ui/sidebar-rows-transparent", test_sidebar_rows_transparent);
    int status = g_test_run();

    stop_broadway();
    remove_tree(root);
    return status;
}
