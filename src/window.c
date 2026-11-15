#include "window.h"

#include "actions.h"
#include "cmdbar.h"
#include "commands.h"
#include "keymap.h"
#include "opsbar.h"
#include "sidebar.h"
#include "split.h"
#include "util.h"
#include <string.h>

#define SIDEBAR_WIDTH 220 /* initial width */
#define SIDEBAR_MIN_WIDTH 150

struct _SfeWindow {
    GtkApplicationWindow parent;

    SfeSidebar* sidebar;
    SfeSplit* split; /* the directory panes */
    SfeOpsBar* ops_bar; /* running file operations */
    SfeCommandBar* cmdbar; /* the ":" command line */
    GtkWidget* showcmd; /* keys of a pending chord */
    SfeKeymap* keymap; /* normal-mode keys: defaults + config [keys] */
    GPtrArray* pending; /* tokens of the chord typed so far */
    char* chord_command; /* run if the chord times out (it is also a prefix) */
    guint chord_timeout;
    gulong config_handler;
};

G_DEFINE_TYPE(SfeWindow, sfe_window, GTK_TYPE_APPLICATION_WINDOW)

SfeSplit* sfe_window_get_split(SfeWindow* win)
{
    return win->split;
}

SfePane* sfe_window_get_active_pane(SfeWindow* win)
{
    return sfe_split_get_active(win->split);
}

void sfe_window_run_op(SfeWindow* win, SfeFileOp* op)
{
    /* Pastes arrive asynchronously (clipboard), possibly after the window
     * was closed: its progress bar is gone, so the job is dropped. */
    if (win->ops_bar == NULL || gtk_widget_in_destruction(GTK_WIDGET(win)))
        return;
    sfe_ops_bar_run(win->ops_bar, op);
}

/* ------------------------------------------------------------------------ */
/* Following the active pane                                                 */
/* ------------------------------------------------------------------------ */

static void update_title(SfeWindow* win)
{
    GFile* dir = sfe_pane_get_dir(sfe_window_get_active_pane(win));
    g_autofree char* name = g_file_get_basename(dir);
    g_autofree char* title = g_strdup_printf("%s — sfe", name);
    gtk_window_set_title(GTK_WINDOW(win), title);
}

/* The title and the sidebar always follow the active pane. */
static void follow_active_pane(SfeWindow* win)
{
    update_title(win);
    sfe_sidebar_set_location(win->sidebar, sfe_pane_get_dir(sfe_window_get_active_pane(win)));
}

static void on_directory_changed(SfeSplit* split, SfePane* pane, gpointer data)
{
    (void)split;
    SfeWindow* win = SFE_WINDOW(data);
    if (pane == sfe_window_get_active_pane(win))
        follow_active_pane(win);
}

static void on_active_changed(SfeSplit* split, gpointer data)
{
    (void)split;
    follow_active_pane(SFE_WINDOW(data));
}

static void on_set_focus(GtkWindow* window, GtkWidget* widget, gpointer data)
{
    (void)window;
    if (widget != NULL)
        sfe_split_track_focus(SFE_WINDOW(data)->split, widget);
}

static void on_sidebar_open(SfeSidebar* sidebar, GFile* dir, int where, gpointer data)
{
    (void)sidebar;
    SfeWindow* win = SFE_WINDOW(data);

    switch ((SfeOpenWhere)where) {
    case SFE_OPEN_NEW_WINDOW: gtk_window_present(GTK_WINDOW(sfe_window_new(sfe_app(), dir))); break;
    case SFE_OPEN_SPLIT: sfe_split_open(win->split, dir); break;
    case SFE_OPEN_HERE: sfe_pane_navigate(sfe_window_get_active_pane(win), dir); break;
    }
}

/* The default keys with config [keys] on top. */
static void rebuild_keymap(SfeWindow* win)
{
    sfe_keymap_free(win->keymap);
    win->keymap = sfe_keymap_new_default();
    const SfeConfig* cfg = sfe_config();
    for (guint i = 0; i < cfg->keys->len; i++) {
        const SfeKeyOverride* k = g_ptr_array_index(cfg->keys, i);
        g_autoptr(GError) error = NULL;
        if (!sfe_keymap_bind(win->keymap, k->sequence, k->command, &error))
            g_warning("config [keys]: %s", error->message);
    }
}

static void apply_config(SfeWindow* win)
{
    rebuild_keymap(win);
    gtk_window_set_decorated(GTK_WINDOW(win), sfe_config()->decorations);
    sfe_actions_sync_state(win);
}

static void on_config_changed(SfeApplication* app, gpointer data)
{
    (void)app;
    apply_config(SFE_WINDOW(data));
}

/* ------------------------------------------------------------------------ */
/* Keys                                                                      */
/* ------------------------------------------------------------------------ */

/* Runs a command line from a key binding; errors go to the command bar. */
static void run_command(SfeWindow* win, const char* line)
{
    g_autoptr(GError) error = NULL;
    if (strcmp(line, "command_line") == 0)
        sfe_command_bar_start(win->cmdbar, NULL);
    else if (!sfe_command_run(win, line, &error))
        sfe_command_bar_show_error(win->cmdbar, error->message);
}

static void drop_chord(SfeWindow* win)
{
    g_ptr_array_set_size(win->pending, 0);
    g_clear_pointer(&win->chord_command, g_free);
    g_clear_handle_id(&win->chord_timeout, g_source_remove);
    gtk_widget_hide(win->showcmd);
}

/* A chord not finished within a second is dropped, or runs the binding it
 * already is ("g" bound while "gg" exists). */
static gboolean on_chord_timeout(gpointer data)
{
    SfeWindow* win = data;
    g_autofree char* command = g_steal_pointer(&win->chord_command);
    win->chord_timeout = 0;
    drop_chord(win);
    if (command != NULL)
        run_command(win, command);
    return G_SOURCE_REMOVE;
}

/* A key event as a keymap token ("j", "C-w", "Space"); NULL for modifiers
 * and keys without a name. */
static char* event_token(const GdkEventKey* event)
{
    static const struct {
        guint keyval;
        const char* name;
    } named[] = {
        { GDK_KEY_Return, "CR" }, { GDK_KEY_KP_Enter, "CR" }, { GDK_KEY_Escape, "Esc" },
        { GDK_KEY_BackSpace, "BS" }, { GDK_KEY_Tab, "Tab" }, { GDK_KEY_ISO_Left_Tab, "Tab" },
        { GDK_KEY_space, "Space" }, { GDK_KEY_Up, "Up" }, { GDK_KEY_Down, "Down" }, { GDK_KEY_Left, "Left" },
        { GDK_KEY_Right, "Right" }, { GDK_KEY_Home, "Home" }, { GDK_KEY_End, "End" }, { GDK_KEY_Delete, "Del" },
        { GDK_KEY_Page_Up, "PageUp" }, { GDK_KEY_Page_Down, "PageDown" },
    };
    bool ctrl = event->state & GDK_CONTROL_MASK;
    bool alt = event->state & GDK_MOD1_MASK;
    bool shift = event->state & GDK_SHIFT_MASK;
    char name[8] = { 0 };

    if (event->is_modifier)
        return NULL;
    for (size_t i = 0; i < G_N_ELEMENTS(named); i++) {
        if (event->keyval == named[i].keyval)
            return sfe_keymap_token(named[i].name, ctrl, alt, shift);
    }
    if (event->keyval >= GDK_KEY_F1 && event->keyval <= GDK_KEY_F24) {
        g_snprintf(name, sizeof name, "F%u", event->keyval - GDK_KEY_F1 + 1);
        return sfe_keymap_token(name, ctrl, alt, shift);
    }
    gunichar c = gdk_keyval_to_unicode(event->keyval);
    if (c < 0x20 || c == 0x7f)
        return NULL;
    g_unichar_to_utf8(c, name);
    return sfe_keymap_token(name, ctrl, alt, shift);
}

/* Normal mode: keys go through the keymap. Returns whether the key was
 * used; an unbound single key goes on to the shortcuts and the widget. */
static bool normal_mode_key(SfeWindow* win, const GdkEventKey* event, GtkWidget* focus)
{
    g_autofree char* token = event_token(event);
    if (token == NULL)
        return false;
    if (win->pending->len == 0 && GTK_IS_BUTTON(focus) && (strcmp(token, "Space") == 0 || strcmp(token, "CR") == 0))
        return false; /* buttons keep Space / Enter */
    if (win->pending->len > 0 && strcmp(token, "Esc") == 0) {
        drop_chord(win);
        return true;
    }

    g_ptr_array_add(win->pending, g_steal_pointer(&token));
    const char* command = NULL;
    bool prefix = false;
    SfeKeysResult result = sfe_keymap_lookup(win->keymap, (char* const*)win->pending->pdata, win->pending->len,
        &command, &prefix);

    if (result == SFE_KEYS_MATCH && !prefix) {
        g_autofree char* line = g_strdup(command);
        drop_chord(win);
        run_command(win, line);
        return true;
    }
    if (result != SFE_KEYS_NONE) { /* wait for the rest of the chord */
        g_autofree char* keys = sfe_keymap_format((char* const*)win->pending->pdata, win->pending->len);
        g_free(win->chord_command);
        win->chord_command = g_strdup(command);
        gtk_label_set_text(GTK_LABEL(win->showcmd), keys);
        gtk_widget_show(win->showcmd);
        g_clear_handle_id(&win->chord_timeout, g_source_remove);
        win->chord_timeout = g_timeout_add(1000, on_chord_timeout, win);
        return true;
    }
    bool was_chord = win->pending->len > 1; /* a broken chord eats its last key */
    drop_chord(win);
    return was_chord;
}

/* Text fields (the path bar, search, the command line) get keys first:
 * GTK3 would otherwise run window shortcuts before them, and Delete or
 * Ctrl+C in the path bar would act on files. Elsewhere keys go through the
 * normal-mode keymap, then the shortcuts. */
static gboolean sfe_window_key_press(GtkWidget* widget, GdkEventKey* event)
{
    SfeWindow* win = SFE_WINDOW(widget);
    GtkWidget* focus = gtk_window_get_focus(GTK_WINDOW(widget));

    if (!event->is_modifier)
        sfe_command_bar_dismiss(win->cmdbar);
    if (GTK_IS_EDITABLE(focus)) {
        drop_chord(win);
        if (gtk_window_propagate_key_event(GTK_WINDOW(widget), event))
            return TRUE;
    } else if (normal_mode_key(win, event, focus)) {
        return TRUE;
    }
    return GTK_WIDGET_CLASS(sfe_window_parent_class)->key_press_event(widget, event);
}

void sfe_window_toggle_sidebar(SfeWindow* win)
{
    GtkWidget* sidebar = GTK_WIDGET(win->sidebar);
    bool hide = gtk_widget_get_visible(sidebar);
    GtkWidget* focus = gtk_window_get_focus(GTK_WINDOW(win));

    gtk_widget_set_visible(sidebar, !hide);
    if (hide && focus != NULL && gtk_widget_is_ancestor(focus, sidebar))
        sfe_pane_focus_view(sfe_window_get_active_pane(win));
}

void sfe_window_toggle_tree(SfeWindow* win)
{
    gtk_widget_show(GTK_WIDGET(win->sidebar));
    sfe_sidebar_toggle_tree(win->sidebar);
}

void sfe_window_show_command_line(SfeWindow* win, const char* text)
{
    sfe_command_bar_start(win->cmdbar, text);
}

static void on_command_bar_done(SfeCommandBar* bar, gpointer data)
{
    (void)bar;
    sfe_pane_focus_view(sfe_window_get_active_pane(SFE_WINDOW(data)));
}

/* ------------------------------------------------------------------------ */
/* Construction                                                              */
/* ------------------------------------------------------------------------ */

static void sfe_window_dispose(GObject* object)
{
    SfeWindow* win = SFE_WINDOW(object);

    if (win->config_handler != 0) {
        g_signal_handler_disconnect(sfe_app(), win->config_handler);
        win->config_handler = 0;
    }
    win->ops_bar = NULL; /* destroyed with the window's children */
    if (win->pending != NULL)
        drop_chord(win);
    g_clear_pointer(&win->pending, g_ptr_array_unref);
    g_clear_pointer(&win->keymap, sfe_keymap_free);
    G_OBJECT_CLASS(sfe_window_parent_class)->dispose(object);
}

static void sfe_window_class_init(SfeWindowClass* klass)
{
    G_OBJECT_CLASS(klass)->dispose = sfe_window_dispose;
    GTK_WIDGET_CLASS(klass)->key_press_event = sfe_window_key_press;
}

static void sfe_window_init(SfeWindow* win)
{
    gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(win)), "sfe-window");
    gtk_window_set_default_size(GTK_WINDOW(win), 1100, 700);
    win->pending = g_ptr_array_new_with_free_func(g_free);
}

SfeWindow* sfe_window_new(SfeApplication* app, GFile* dir)
{
    SfeWindow* win = g_object_new(SFE_TYPE_WINDOW, "application", app, NULL);
    g_autoptr(GFile) start = NULL;

    if (dir == NULL) {
        g_autofree char* path = sfe_expand_home(sfe_config()->start_path);
        dir = start = g_file_new_for_path(path);
    }

    win->sidebar = SFE_SIDEBAR(sfe_sidebar_new());
    gtk_widget_set_size_request(GTK_WIDGET(win->sidebar), SIDEBAR_MIN_WIDTH, -1);
    g_signal_connect(win->sidebar, "open-location", G_CALLBACK(on_sidebar_open), win);

    win->split = sfe_split_new(dir);
    g_signal_connect(win->split, "directory-changed", G_CALLBACK(on_directory_changed), win);
    g_signal_connect(win->split, "active-changed", G_CALLBACK(on_active_changed), win);
    g_signal_connect_after(win, "set-focus", G_CALLBACK(on_set_focus), win);

    win->ops_bar = SFE_OPS_BAR(sfe_ops_bar_new());
    sfe_actions_add_to_window(win);

    /* The command line and the chord keys float over the bottom of the
     * panes, so opening them does not move the files around. */
    win->cmdbar = sfe_command_bar_new(win);
    g_signal_connect(win->cmdbar, "done", G_CALLBACK(on_command_bar_done), win);
    win->showcmd = gtk_label_new(NULL);
    gtk_style_context_add_class(gtk_widget_get_style_context(win->showcmd), "sfe-showcmd");
    gtk_widget_set_halign(win->showcmd, GTK_ALIGN_END);
    gtk_widget_set_valign(win->showcmd, GTK_ALIGN_END);
    gtk_widget_set_no_show_all(win->showcmd, TRUE);
    GtkWidget* overlay = gtk_overlay_new();
    gtk_container_add(GTK_CONTAINER(overlay), GTK_WIDGET(win->split));
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), GTK_WIDGET(win->cmdbar));
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), win->showcmd);

    /* sidebar | (panes over the operations bar) */
    GtkWidget* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(content), overlay, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(content), GTK_WIDGET(win->ops_bar), FALSE, FALSE, 0);

    GtkWidget* paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_pack1(GTK_PANED(paned), GTK_WIDGET(win->sidebar), FALSE, FALSE);
    gtk_paned_pack2(GTK_PANED(paned), content, TRUE, FALSE);
    gtk_paned_set_position(GTK_PANED(paned), SIDEBAR_WIDTH);
    gtk_container_add(GTK_CONTAINER(win), paned);
    gtk_widget_show_all(paned);

    sfe_sidebar_set_location(win->sidebar, dir);
    update_title(win);
    apply_config(win);
    win->config_handler = g_signal_connect(app, "config-changed", G_CALLBACK(on_config_changed), win);
    sfe_pane_focus_view(sfe_window_get_active_pane(win));
    return win;
}
