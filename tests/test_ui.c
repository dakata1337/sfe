#define _DEFAULT_SOURCE /* symlink() */
/* Widget tests: the real application and windows, driven the way a user
 * drives them (key presses, : commands, the Settings widgets), on a
 * headless GTK display (broadwayd) in a throwaway home folder.
 *
 * Skipped (exit 77) without broadwayd. Run alone with
 *   meson test -C build ui -v
 * and in a decimal-comma language / another theme by the "ui-bg" and
 * "ui-theme" tests (tests/meson.build). */
#include "actions.h"
#include "cmdline.h"
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
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

static char* root; /* HOME, XDG_* and the test folders are in here */
static char* tools; /* monkey only: its data dirs and programs, out of its reach */
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

/* Found by the UI monkey: closing a split whose views still showed their
 * rows made GTK draw through a destroyed cell area during the teardown. */
static void test_split_close_repeatedly(void)
{
    g_autofree char* dir = make_folder("split-close");
    SfeWindow* win = open_window(dir);
    SfeSplit* split = sfe_window_get_split(win);
    for (int i = 0; i < 20; i++) {
        run(win, "split horizontal");
        WAIT_UNTIL(sfe_split_get_n_panes(split) == 2);
        run(win, i % 3 ? "view list" : "view grid");
        WAIT_UNTIL(n_entries(active(win)) > 0);
        run(win, "select all"); /* (the monkey had everything selected) */
        run(win, "q");
        WAIT_UNTIL(sfe_split_get_n_panes(split) == 1);
    }
    close_window(win);
}

/* Found by the UI monkey: "Open With → Other Application" for a file that
 * was gone (still selected while its row went) gave GLib criticals from
 * GTK's app chooser. */
static void test_open_with_gone_file(void)
{
    g_autofree char* dir = make_folder("open-with");
    SfeWindow* win = open_window(dir);
    SfePane* pane = active(win);
    sfe_pane_reveal(pane, "a.txt"); /* selects it */
    GList* sel = sfe_pane_get_selected_files(pane);
    g_assert_cmpuint(g_list_length(sel), ==, 1);
    g_list_free_full(sel, g_object_unref);
    g_autofree char* path = g_build_filename(dir, "a.txt", NULL);
    g_remove(path);
    g_action_group_activate_action(G_ACTION_GROUP(win), "open-with-other", NULL); /* before the row goes */
    settle(100);
    GList* tops = gtk_window_list_toplevels();
    for (GList* l = tops; l != NULL; l = l->next)
        g_assert_false(GTK_IS_APP_CHOOSER_DIALOG(l->data));
    g_list_free(tops);
    close_window(win);
}

/* Found by the UI monkey: in a view showing a message instead of files
 * ("Cannot open this folder"), cursor keys gave the focus to the view
 * hidden behind it; later keys went to a widget nobody sees (and, when it
 * had never been realized, made GTK log a critical). The focus must stay on
 * something on screen. */
static void assert_focus_on_screen(SfeWindow* win, const char* after)
{
    GtkWidget* focus = gtk_window_get_focus(GTK_WINDOW(win));
    if (focus != NULL && !gtk_widget_is_drawable(focus))
        g_error("after %s the focus is on a hidden %s", after, G_OBJECT_TYPE_NAME(focus));
}

static void test_keys_in_message_view(void)
{
    g_autofree char* dir = make_folder("message");
    g_autofree char* locked = g_build_filename(dir, "locked", NULL);
    g_mkdir(locked, 0000);
    SfeWindow* win = open_window(dir);
    run(win, "cd locked");
    settle(200);
    sfe_pane_focus_view(active(win));
    struct { guint keyval; GdkModifierType mods; const char* name; } keys[] = {
        { GDK_KEY_j, 0, "j" }, { GDK_KEY_k, 0, "k" }, { GDK_KEY_G, GDK_SHIFT_MASK, "G" },
        { GDK_KEY_g, 0, "g" }, { GDK_KEY_g, 0, "gg" }, { GDK_KEY_v, 0, "v" },
        { GDK_KEY_2, GDK_CONTROL_MASK, "Ctrl+2" }, { GDK_KEY_Menu, GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Ctrl+Shift+Menu" },
    };
    for (size_t i = 0; i < G_N_ELEMENTS(keys); i++) {
        key(win, keys[i].keyval, keys[i].mods);
        assert_focus_on_screen(win, keys[i].name);
    }
    g_chmod(locked, 0755);
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
/* Monkey: random but reproducible input (SFE_MONKEY_STEPS, SFE_MONKEY_SEED) */
/* ------------------------------------------------------------------------ */

/* Random keys, : commands and actions on a window over a test folder; after
 * every one sfe must still run and log no warning or critical (both are
 * fatal under g_test). Nothing can act outside the throwaway folder: PATH
 * is empty (no programs, no umount), GIO sees no applications, and a view
 * that leaves the sandbox is sent back after every single key. On failure
 * the seed and the last actions are printed: replay with the same seed. */

#define MONKEY_LOG 40
static char* monkey_log[MONKEY_LOG];
static guint monkey_logged;
static guint32 monkey_seed;

static void monkey_note(const char* format, ...) G_GNUC_PRINTF(1, 2);
static void monkey_note(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    g_free(monkey_log[monkey_logged % MONKEY_LOG]);
    monkey_log[monkey_logged % MONKEY_LOG] = g_strdup_vprintf(format, args);
    va_end(args);
    monkey_logged++;
}

static void monkey_dump(int sig)
{
    fprintf(stderr, "\n*** monkey: signal %d after %u actions, SFE_MONKEY_SEED=%u; last actions:\n", sig,
        monkey_logged, monkey_seed);
    guint first = monkey_logged > MONKEY_LOG ? monkey_logged - MONKEY_LOG : 0;
    for (guint i = first; i < monkey_logged; i++)
        fprintf(stderr, "  %5u  %s\n", i, monkey_log[i % MONKEY_LOG]);
    signal(sig, SIG_DFL);
    raise(sig);
}

/* The window keys go to: a dialog or other window on top, else `main`. */
static GtkWindow* key_target(SfeWindow* main)
{
    GList* tops = gtk_window_list_toplevels();
    GtkWindow* target = GTK_WINDOW(main);
    for (GList* l = tops; l != NULL; l = l->next) {
        GtkWindow* w = l->data;
        if (gtk_widget_get_mapped(GTK_WIDGET(w)) && gtk_window_get_window_type(w) == GTK_WINDOW_TOPLEVEL
            && w != GTK_WINDOW(main) && (gtk_window_get_modal(w) || gtk_window_get_transient_for(w) != NULL))
            target = w;
    }
    g_list_free(tops);
    return target;
}

static void send_key(GtkWindow* win, guint keyval, GdkModifierType mods)
{
    GdkWindow* gdk = gtk_widget_get_window(GTK_WIDGET(win));
    if (gdk == NULL)
        return;
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
        e->key.window = g_object_ref(gdk);
        e->key.send_event = TRUE;
        e->key.time = GDK_CURRENT_TIME;
        e->key.state = mods;
        e->key.keyval = keyval;
        e->key.hardware_keycode = code;
        gdk_event_set_device(e, keyboard);
        gtk_main_do_event(e);
        gdk_event_free(e);
    }
}

static bool inside(GFile* dir, const char* top)
{
    g_autoptr(GFile) root_file = g_file_new_for_path(top);
    return g_file_equal(dir, root_file) || g_file_has_prefix(dir, root_file);
}

/* Every view of every window back into the sandbox folder `top`. */
static void monkey_guard(const char* top)
{
    GList* tops = gtk_window_list_toplevels();
    for (GList* l = tops; l != NULL; l = l->next) {
        if (!SFE_IS_WINDOW(l->data))
            continue;
        SfeSplit* split = sfe_window_get_split(l->data);
        for (int i = 0; i < sfe_split_get_n_panes(split); i++) {
            SfePane* pane = sfe_split_get_pane(split, i);
            GFile* dir = sfe_pane_get_dir(pane);
            if (dir != NULL && !inside(dir, top)) {
                g_autoptr(GFile) back = g_file_new_for_path(top);
                sfe_pane_navigate(pane, back);
            }
        }
    }
    g_list_free(tops);
}

static const char* pick(GRand* r, const char* const* list, int n)
{
    return list[g_rand_int_range(r, 0, n)];
}

/* A name in the active view, or a new one (sometimes with a space). */
static char* random_name(GRand* r, SfePane* pane)
{
    if (g_rand_boolean(r)) {
        GList* files = sfe_pane_get_all_files(pane);
        guint n = g_list_length(files);
        char* name = n ? g_file_get_basename(g_list_nth_data(files, g_rand_int_range(r, 0, (int)n))) : NULL;
        g_list_free_full(files, g_object_unref);
        if (name != NULL) {
            char* quoted = sfe_cmdline_quote(name);
            g_free(name);
            return quoted;
        }
    }
    return g_rand_boolean(r) ? g_strdup_printf("f%u", g_rand_int_range(r, 0, 20))
                             : g_strdup_printf("new\\ %u.txt", g_rand_int_range(r, 0, 20));
}

static void monkey_command(GRand* r, SfeWindow* win)
{
    static const char* const fixed[] = { "cd sub", "cd ..", "cd", "yank", "cut", "paste", "undo", "split vertical",
        "split horizontal", "split next", "split previous", "split move right", "split move down", "q",
        "view grid", "view list", "sort name", "sort size descending", "sort modified", "sort type ascending",
        "sort", "hidden toggle", "sidebar toggle", "tree toggle", "select all", "select toggle", "cursor down",
        "cursor up", "cursor first", "cursor last", "visual", "visual swap", "visual off", "center", "settings",
        "settings keys", "bookmark", "unbookmark", "mass_rename", "mass_rename files", "trash", "delete",
        "rename", "command_line" };
    static const char* const with_name[] = { "mkdir", "touch", "trash", "cd", "yank", "cut", "bookmark",
        "rename" };
    g_autofree char* line = NULL;
    int kind = g_rand_int_range(r, 0, 10);
    SfePane* pane = active(win);
    if (kind < 5) {
        line = g_strdup(pick(r, fixed, G_N_ELEMENTS(fixed)));
    } else if (kind < 8) {
        g_autofree char* name = random_name(r, pane);
        line = g_strdup_printf("%s %s", pick(r, with_name, G_N_ELEMENTS(with_name)), name);
    } else {
        g_autofree char* a = random_name(r, pane);
        g_autofree char* b = random_name(r, pane);
        static const char* const two[] = { "cp", "mv", "rename", "ln" };
        line = g_strdup_printf("%s %s %s", pick(r, two, G_N_ELEMENTS(two)), a, b);
    }
    monkey_note(":%s", line);
    sfe_command_run(win, line, NULL); /* (errors are fine: random arguments) */
}

static void monkey_key(GRand* r, SfeWindow* win)
{
    static const guint named[] = { GDK_KEY_Escape, GDK_KEY_Return, GDK_KEY_BackSpace, GDK_KEY_Tab,
        GDK_KEY_ISO_Left_Tab, GDK_KEY_Up, GDK_KEY_Down, GDK_KEY_Left, GDK_KEY_Right, GDK_KEY_Home, GDK_KEY_End,
        GDK_KEY_Page_Up, GDK_KEY_Page_Down, GDK_KEY_Delete, GDK_KEY_F2, GDK_KEY_F5, GDK_KEY_F10, GDK_KEY_space,
        GDK_KEY_Menu };
    static const char printable[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:/.-?! \"'<>\\";
    guint keyval;
    if (g_rand_int_range(r, 0, 4) == 0)
        keyval = named[g_rand_int_range(r, 0, G_N_ELEMENTS(named))];
    else
        keyval = gdk_unicode_to_keyval((guchar)printable[g_rand_int_range(r, 0, sizeof printable - 1)]);
    static const GdkModifierType mods[] = { 0, 0, 0, 0, 0, 0, GDK_CONTROL_MASK, GDK_CONTROL_MASK, GDK_SHIFT_MASK,
        GDK_CONTROL_MASK | GDK_SHIFT_MASK, GDK_MOD1_MASK };
    GdkModifierType mod = mods[g_rand_int_range(r, 0, G_N_ELEMENTS(mods))];
    if (keyval >= 'A' && keyval <= 'Z')
        mod |= GDK_SHIFT_MASK;
    GtkWindow* target = key_target(win);
    GtkWidget* focus = gtk_window_get_focus(target);
    monkey_note("key %s%s%s%s to %s (focus %s)", mod & GDK_CONTROL_MASK ? "Ctrl+" : "",
        mod & GDK_MOD1_MASK ? "Alt+" : "", mod & GDK_SHIFT_MASK ? "Shift+" : "",
        gdk_keyval_name(keyval) ? gdk_keyval_name(keyval) : "?", G_OBJECT_TYPE_NAME(target),
        focus ? G_OBJECT_TYPE_NAME(focus) : "none");
    if (focus != NULL && !gtk_widget_get_realized(focus)) { /* a real display never delivers keys there */
        g_autofree char* where = gtk_widget_path_to_string(gtk_widget_get_path(focus));
        monkey_note("  the focus is not realized: %s", where);
    }
    send_key(target, keyval, mod);
}

static void monkey_action(GRand* r, SfeWindow* win)
{
    size_t n = 0;
    const SfeAction* actions = sfe_actions_get(&n);
    const char* name = actions[g_rand_int_range(r, 0, (int)n)].name;
    monkey_note("action %s", name);
    g_action_group_activate_action(G_ACTION_GROUP(win), name, NULL);
}

/* Files changing on disk under the view, as other programs do. */
static void monkey_disk(GRand* r, const char* dir)
{
    g_autofree char* path = g_strdup_printf("%s/disk%u", dir, g_rand_int_range(r, 0, 8));
    if (g_file_test(path, G_FILE_TEST_EXISTS)) {
        monkey_note("disk: remove %s", path);
        g_remove(path);
    } else {
        monkey_note("disk: create %s", path);
        g_file_set_contents(path, "x", 1, NULL);
    }
}

/* Windows beyond the first few (Ctrl+N), dialogs left open: closed now and
 * then, so input keeps reaching the main window. */
static SfeWindow* monkey_tidy(SfeWindow* main, const char* dir, bool close_dialogs)
{
    GList* tops = gtk_window_list_toplevels();
    for (GList* l = tops; l != NULL; l = l->next) /* (destroying one window destroys its dialogs too) */
        g_object_ref(l->data);
    bool main_alive = false;
    int windows = 0;
    for (GList* l = tops; l != NULL; l = l->next) {
        if (gtk_widget_in_destruction(l->data))
            continue;
        if (l->data == (gpointer)main)
            main_alive = true;
        if (SFE_IS_WINDOW(l->data) && ++windows > 3 && l->data != (gpointer)main)
            gtk_widget_destroy(l->data);
        else if (close_dialogs && !SFE_IS_WINDOW(l->data) && gtk_widget_get_mapped(l->data)
            && gtk_window_get_window_type(l->data) == GTK_WINDOW_TOPLEVEL)
            gtk_widget_destroy(l->data);
    }
    g_list_free_full(tops, g_object_unref);
    if (!main_alive) { /* :q on the last view closed it */
        monkey_note("(window closed: a new one)");
        main = open_window(dir);
    }
    return main;
}

/* Warnings stay fatal, except GTK's about icon loading: its image loaders
 * run in a sandbox with a timeout, which a busy machine can miss. */
static gboolean monkey_fatal(const char* domain, GLogLevelFlags level, const char* message, gpointer data)
{
    (void)domain, (void)level, (void)data;
    return strstr(message, "Could not load a pixbuf from icon theme") == NULL;
}

static void test_monkey(void)
{
    g_test_log_set_fatal_handler(monkey_fatal, NULL);
    guint steps = (guint)g_ascii_strtoull(g_getenv("SFE_MONKEY_STEPS"), NULL, 10);
    const char* seed = g_getenv("SFE_MONKEY_SEED");
    monkey_seed = seed ? (guint32)g_ascii_strtoull(seed, NULL, 10) : (guint32)g_get_real_time();
    g_test_message("SFE_MONKEY_SEED=%u, %u steps", monkey_seed, steps);
    signal(SIGABRT, monkey_dump);
    signal(SIGSEGV, monkey_dump);

    g_autofree char* dir = make_folder("monkey");
    SfeWindow* win = open_window(dir);
    GRand* r = g_rand_new_with_seed(monkey_seed);
    for (guint i = 0; i < steps; i++) {
        int kind = g_rand_int_range(r, 0, 100);
        if (kind >= 60 && kind < 90 && gtk_grab_get_current() != NULL)
            kind = 0; /* a menu has the input: a user can only press keys (which go to it) */
        if (kind < 60)
            monkey_key(r, win);
        else if (kind < 80)
            monkey_command(r, win);
        else if (kind < 90)
            monkey_action(r, win);
        else if (kind < 95)
            monkey_disk(r, dir);
        else
            send_key(key_target(win), GDK_KEY_Escape, 0);
        settle(g_rand_int_range(r, 2, 25));
        monkey_guard(dir); /* (not root: the config and trash are there) */
        win = monkey_tidy(win, dir, i % 50 == 49);
    }
    g_rand_free(r);
    signal(SIGABRT, SIG_DFL);
    signal(SIGSEGV, SIG_DFL);
    g_test_message("%u actions, all fine", monkey_logged);
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
    if (!g_file_test(path, G_FILE_TEST_IS_SYMLINK))
        g_chmod(path, 0700); /* (the monkey may have taken permissions away) */
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

    bool monkey = g_getenv("SFE_MONKEY_STEPS") != NULL;
    if (monkey) { /* GIO must see no applications: data dirs without them */
        tools = g_dir_make_tmp("sfe-ui-tools-XXXXXX", NULL);
        g_autofree char* share = g_build_filename(tools, "share", NULL);
        g_mkdir_with_parents(share, 0755);
        GDir* usr = g_dir_open("/usr/share", 0, NULL); /* all of /usr/share but the applications */
        const char* name;
        while (usr != NULL && (name = g_dir_read_name(usr)) != NULL) {
            if (strcmp(name, "applications") == 0)
                continue;
            g_autofree char* from = g_build_filename("/usr/share", name, NULL);
            g_autofree char* to = g_build_filename(share, name, NULL);
            symlink(from, to);
        }
        if (usr != NULL)
            g_dir_close(usr);
        g_setenv("XDG_DATA_DIRS", share, TRUE);
    }
    if (!start_broadway()) {
        g_print("1..0 # SKIP broadwayd is not installed or did not start\n");
        remove_tree(root);
        return 77;
    }
    g_test_init(&argc, &argv, NULL);
    if (monkey) { /* no programs: no xdg-open, umount, thumbnailers (after broadwayd started) */
        g_autofree char* bin = g_build_filename(tools, "programs", NULL);
        g_mkdir_with_parents(bin, 0755);
        g_autofree char* bwrap = g_find_program_in_path("bwrap"); /* except the image loaders' sandbox (glycin) */
        if (bwrap != NULL) {
            g_autofree char* link = g_build_filename(bin, "bwrap", NULL);
            symlink(bwrap, link);
        }
        g_setenv("PATH", bin, TRUE);
    }

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
    g_test_add_func("/ui/split-close-repeatedly", test_split_close_repeatedly);
    g_test_add_func("/ui/open-with-gone-file", test_open_with_gone_file);
    g_test_add_func("/ui/keys-in-message-view", test_keys_in_message_view);
    if (monkey)
        g_test_add_func("/ui/monkey", test_monkey);
    int status = g_test_run();

    stop_broadway();
    remove_tree(root);
    if (tools != NULL)
        remove_tree(tools);
    return status;
}
