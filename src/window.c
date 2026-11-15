#include "window.h"
#include "theme.h"

#include "actions.h"
#include "cmdbar.h"
#include "commands.h"
#include "keymap.h"
#include "opsbar.h"
#include "sidebar.h"
#include "split.h"
#include "undo.h"
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
    GtkWidget* chord_box; /* the suggestions above showcmd */
    GtkWidget* hints; /* GtkGrid: what can follow the pending chord */
    guint hints_timeout;
    GtkWidget* mode; /* "VISUAL" while the active view is in visual mode */
    SfeKeymap* keymap; /* normal-mode keys: defaults + config [keys] */
    SfeKeymap* visual_keymap; /* looked up first in visual mode: [keys.visual] */
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

/* Finished jobs go into the undo history (but not the undoing ones). */
static void on_op_finished(SfeFileOp* op, GError* error, gpointer data)
{
    (void)error, (void)data;
    if (g_object_get_data(G_OBJECT(op), "sfe-no-undo") == NULL)
        sfe_undo_record_op(op);
}

void sfe_window_run_op(SfeWindow* win, SfeFileOp* op)
{
    /* Pastes arrive asynchronously (clipboard), possibly after the window
     * was closed: its progress bar is gone, so the job is dropped. */
    if (win->ops_bar == NULL || gtk_widget_in_destruction(GTK_WIDGET(win)))
        return;
    g_signal_connect(op, "finished", G_CALLBACK(on_op_finished), NULL);
    sfe_ops_bar_run(win->ops_bar, op);
}

bool sfe_window_undo(SfeWindow* win, guint id)
{
    SfeUndoEntry* entry = sfe_undo_find(id);
    if (entry == NULL) {
        g_autofree char* text = id == 0 ? g_strdup("nothing to undo") : g_strdup_printf("no undo step %u", id);
        sfe_command_bar_show_error(win->cmdbar, text);
        return false;
    }
    g_autofree char* what = sfe_undo_describe(entry);
    g_autoptr(GError) error = NULL;
    SfeFileOp* job = NULL;
    guint missing = 0;
    if (!sfe_undo_apply(entry, &job, &missing, &error)) { /* (entry may be freed) */
        g_autofree char* text = g_strdup_printf("cannot undo %s: %s", what, error->message);
        sfe_command_bar_show_error(win->cmdbar, text);
        return false;
    }
    if (job != NULL) {
        g_object_set_data(G_OBJECT(job), "sfe-no-undo", GINT_TO_POINTER(1));
        sfe_window_run_op(win, job);
        g_object_unref(job);
    }
    g_autofree char* text = missing == 0 ? g_strdup_printf("undid: %s", what)
                                         : g_strdup_printf("undid: %s (%u no longer there)", what, missing);
    sfe_command_bar_show_message(win->cmdbar, text);
    return true;
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

static void update_mode(SfeWindow* win);

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
    update_mode(SFE_WINDOW(data));
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

/* The default keys with config [keys] / [keys.visual] on top. */
static void rebuild_keymap(SfeWindow* win)
{
    sfe_keymap_free(win->keymap);
    sfe_keymap_free(win->visual_keymap);
    win->keymap = sfe_keymap_new_default();
    win->visual_keymap = sfe_keymap_new_visual_default();
    const SfeConfig* cfg = sfe_config();
    for (guint i = 0; i < cfg->keys->len; i++) {
        const SfeKeyOverride* k = g_ptr_array_index(cfg->keys, i);
        g_autoptr(GError) error = NULL;
        if (!sfe_keymap_bind(k->visual ? win->visual_keymap : win->keymap, k->sequence, k->command, &error))
            g_warning("config [keys]: %s", error->message);
    }
}

/* "VISUAL" in the corner while the active view selects with v. */
static void update_mode(SfeWindow* win)
{
    SfePane* pane = sfe_window_get_active_pane(win);
    if (!sfe_pane_in_visual(pane)) {
        gtk_widget_hide(win->mode);
        return;
    }
    GList* files = sfe_pane_get_selected_files(pane);
    guint n = g_list_length(files);
    g_list_free_full(files, g_object_unref);
    g_autofree char* text = g_strdup_printf("-- VISUAL --  %u selected%s", n,
        sfe_pane_visual_drawing(pane) ? "" : "  (v starts a chunk)");
    gtk_label_set_text(GTK_LABEL(win->mode), text);
    gtk_widget_show(win->mode);
}

static void on_visual_changed(SfeSplit* split, SfePane* pane, gpointer data)
{
    (void)split, (void)pane;
    update_mode(SFE_WINDOW(data));
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
    g_clear_handle_id(&win->hints_timeout, g_source_remove);
    gtk_widget_hide(win->chord_box);
    gtk_widget_hide(win->hints);
}

/* Fills the suggestions with what can follow the keys typed so far. */
static void fill_hints(SfeWindow* win)
{
    char* const* keys = (char* const*)win->pending->pdata;
    GPtrArray* hints = NULL;
    if (sfe_pane_in_visual(sfe_window_get_active_pane(win)))
        hints = sfe_keymap_continuations(win->visual_keymap, keys, win->pending->len, NULL);
    hints = sfe_keymap_continuations(win->keymap, keys, win->pending->len, hints);

    g_autoptr(GList) old = gtk_container_get_children(GTK_CONTAINER(win->hints));
    for (GList* l = old; l != NULL; l = l->next)
        gtk_widget_destroy(l->data);
    for (guint i = 0; i < hints->len; i++) {
        const SfeKeyHint* h = g_ptr_array_index(hints, i);
        GtkWidget* key = gtk_label_new(h->keys);
        GtkWidget* what = gtk_label_new(h->help ? h->help : h->command);
        gtk_style_context_add_class(gtk_widget_get_style_context(key), "sfe-keyhint-key");
        gtk_label_set_xalign(GTK_LABEL(key), 0);
        gtk_label_set_xalign(GTK_LABEL(what), 0);
        gtk_grid_attach(GTK_GRID(win->hints), key, 0, (int)i, 1, 1);
        gtk_grid_attach(GTK_GRID(win->hints), what, 1, (int)i, 1, 1);
        gtk_widget_show(key);
        gtk_widget_show(what);
    }
    gtk_widget_show(win->hints); /* (no show_all: it is no-show-all) */
    g_ptr_array_unref(hints);
}

/* Half a second into a chord the suggestions appear, and the chord then
 * waits for its next key instead of timing out (unless the keys typed are a
 * binding of their own, which still runs after a second). */
static gboolean on_hints_timeout(gpointer data)
{
    SfeWindow* win = data;
    win->hints_timeout = 0;
    fill_hints(win);
    if (win->chord_command == NULL)
        g_clear_handle_id(&win->chord_timeout, g_source_remove);
    return G_SOURCE_REMOVE;
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
    char* const* keys = (char* const*)win->pending->pdata;
    SfeKeysResult result = SFE_KEYS_NONE;
    if (sfe_pane_in_visual(sfe_window_get_active_pane(win))) /* visual keys first */
        result = sfe_keymap_lookup(win->visual_keymap, keys, win->pending->len, &command, &prefix);
    if (result == SFE_KEYS_NONE)
        result = sfe_keymap_lookup(win->keymap, keys, win->pending->len, &command, &prefix);

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
        gtk_widget_show(win->chord_box);
        g_clear_handle_id(&win->chord_timeout, g_source_remove);
        g_clear_handle_id(&win->hints_timeout, g_source_remove);
        if (gtk_widget_get_visible(win->hints)) { /* already reading them: next level now */
            fill_hints(win);
            if (win->chord_command != NULL)
                win->chord_timeout = g_timeout_add(1000, on_chord_timeout, win);
        } else {
            win->chord_timeout = g_timeout_add(1000, on_chord_timeout, win);
            win->hints_timeout = g_timeout_add(500, on_hints_timeout, win);
        }
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

bool sfe_window_get_sidebar_visible(SfeWindow* win)
{
    return gtk_widget_get_visible(GTK_WIDGET(win->sidebar));
}

void sfe_window_set_sidebar_visible(SfeWindow* win, bool visible)
{
    GtkWidget* sidebar = GTK_WIDGET(win->sidebar);
    GtkWidget* focus = gtk_window_get_focus(GTK_WINDOW(win));

    gtk_widget_set_visible(sidebar, visible);
    if (!visible && focus != NULL && gtk_widget_is_ancestor(focus, sidebar))
        sfe_pane_focus_view(sfe_window_get_active_pane(win));
}

void sfe_window_toggle_sidebar(SfeWindow* win)
{
    sfe_window_set_sidebar_visible(win, !sfe_window_get_sidebar_visible(win));
}

bool sfe_window_get_tree_full(SfeWindow* win)
{
    return sfe_sidebar_get_tree_full(win->sidebar);
}

void sfe_window_set_tree_full(SfeWindow* win, bool full)
{
    gtk_widget_show(GTK_WIDGET(win->sidebar));
    sfe_sidebar_set_tree_full(win->sidebar, full);
}

void sfe_window_toggle_tree(SfeWindow* win)
{
    sfe_window_set_tree_full(win, !sfe_window_get_tree_full(win));
}

void sfe_window_show_message(SfeWindow* win, const char* text, bool error)
{
    if (error)
        sfe_command_bar_show_error(win->cmdbar, text);
    else
        sfe_command_bar_show_message(win->cmdbar, text);
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
    g_clear_pointer(&win->visual_keymap, sfe_keymap_free);
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
    gtk_widget_set_size_request(GTK_WIDGET(win->sidebar), sfe_theme_scaled(SIDEBAR_MIN_WIDTH), -1);
    g_signal_connect(win->sidebar, "open-location", G_CALLBACK(on_sidebar_open), win);

    win->split = sfe_split_new(dir);
    g_signal_connect(win->split, "directory-changed", G_CALLBACK(on_directory_changed), win);
    g_signal_connect(win->split, "active-changed", G_CALLBACK(on_active_changed), win);
    g_signal_connect(win->split, "visual-changed", G_CALLBACK(on_visual_changed), win);
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
    win->hints = gtk_grid_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(win->hints), "sfe-keyhints");
    gtk_grid_set_column_spacing(GTK_GRID(win->hints), 16);
    gtk_grid_set_row_spacing(GTK_GRID(win->hints), 2);
    gtk_widget_set_no_show_all(win->hints, TRUE);
    win->chord_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(win->chord_box), win->hints, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(win->chord_box), win->showcmd, FALSE, FALSE, 0);
    gtk_widget_show(win->showcmd);
    gtk_widget_set_halign(win->chord_box, GTK_ALIGN_END);
    gtk_widget_set_valign(win->chord_box, GTK_ALIGN_END);
    gtk_widget_set_no_show_all(win->chord_box, TRUE);
    win->mode = gtk_label_new(NULL);
    gtk_style_context_add_class(gtk_widget_get_style_context(win->mode), "sfe-showcmd");
    gtk_style_context_add_class(gtk_widget_get_style_context(win->mode), "sfe-mode");
    gtk_widget_set_halign(win->mode, GTK_ALIGN_START);
    gtk_widget_set_valign(win->mode, GTK_ALIGN_END);
    gtk_widget_set_no_show_all(win->mode, TRUE);
    GtkWidget* overlay = gtk_overlay_new();
    gtk_container_add(GTK_CONTAINER(overlay), GTK_WIDGET(win->split));
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), GTK_WIDGET(win->cmdbar));
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), win->chord_box);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), win->mode);

    /* sidebar | (panes over the operations bar) */
    GtkWidget* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(content), overlay, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(content), GTK_WIDGET(win->ops_bar), FALSE, FALSE, 0);

    GtkWidget* paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_pack1(GTK_PANED(paned), GTK_WIDGET(win->sidebar), FALSE, FALSE);
    gtk_paned_pack2(GTK_PANED(paned), content, TRUE, FALSE);
    gtk_paned_set_position(GTK_PANED(paned), sfe_theme_scaled(SIDEBAR_WIDTH));
    gtk_container_add(GTK_CONTAINER(win), paned);
    gtk_widget_show_all(paned);

    sfe_sidebar_set_location(win->sidebar, dir);
    update_title(win);
    apply_config(win);
    win->config_handler = g_signal_connect(app, "config-changed", G_CALLBACK(on_config_changed), win);
    sfe_pane_focus_view(sfe_window_get_active_pane(win));
    return win;
}
