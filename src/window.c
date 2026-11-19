#include "window.h"
#include "theme.h"

#include "actions.h"
#include "cmdbar.h"
#include "commands.h"
#include "dialogs.h"
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>
#endif
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif
#include "keymap.h"
#include "opsbar.h"
#include "preview.h"
#include "sidebar.h"
#include "split.h"
#include "undo.h"
#include "util.h"
#include <stdio.h>
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
    GtkWidget* status; /* bottom left, above the command line: notice over mode */
    GtkWidget* notice; /* "Yanked 3 files (32 MB)", for a few seconds */
    guint notice_id; /* which notice is showing (sfe_window_show_status()) */
    guint notice_timeout;
    GtkWidget* mode; /* "VISUAL" while the active view is in visual mode */
    GtkWidget* content; /* panes over the operations bar (and the chooser bar) */
    const SfeChooser* chooser; /* chooser mode (sfe --choose), or NULL */
    GtkWidget* chooser_name; /* save: the file name entry */
    GPtrArray* filters; /* SfeFileFilter*, or NULL */
    SfeFileFilter* folder_filter; /* folder dialogs: folders only (instead of `filters`) */
    int filter_index; /* the one in use */
    GPtrArray* choice_widgets; /* GtkCheckButton / GtkComboBoxText, "sfe-choice-id" set */
    SfePreview* preview; /* the selected file, beside the views */
    bool chosen; /* the answer was printed */
    SfeKeymap* keymap; /* normal-mode keys: defaults + config [keys] */
    SfeKeymap* visual_keymap; /* looked up first in visual mode: [keys.visual] */
    GPtrArray* pending; /* tokens of the chord typed so far */
    char* chord_command; /* run if the chord times out (it is also a prefix) */
    guint chord_timeout;
    gulong config_handler;
};

G_DEFINE_TYPE(SfeWindow, sfe_window, GTK_TYPE_APPLICATION_WINDOW)

static void accept(SfeWindow* win);

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

static void apply_filter(SfeWindow* win);
static void apply_selection_mode(SfeWindow* win);

/* The chooser's preview follows the active view's selection. */
static void update_preview(SfeWindow* win)
{
    SfePane* pane = win->preview ? sfe_window_get_active_pane(win) : NULL;
    if (pane == NULL)
        return;
    GList* files = sfe_pane_get_selected_files(pane);
    sfe_preview_set_files(win->preview, files);
    g_list_free_full(files, g_object_unref);
}

static void on_selection_changed(SfeSplit* split, SfePane* pane, gpointer data)
{
    (void)split;
    SfeWindow* win = SFE_WINDOW(data);
    if (pane == sfe_window_get_active_pane(win))
        update_preview(win);
}

static void on_directory_changed(SfeSplit* split, SfePane* pane, gpointer data)
{
    (void)split;
    SfeWindow* win = SFE_WINDOW(data);
    if (win->filters != NULL || win->folder_filter != NULL) /* a view opened later gets the filter too */
        apply_filter(win);
    if (win->chooser != NULL)
        apply_selection_mode(win);
    if (pane == sfe_window_get_active_pane(win)) {
        follow_active_pane(win);
        update_preview(win);
    }
}

static void on_active_changed(SfeSplit* split, gpointer data)
{
    (void)split;
    follow_active_pane(SFE_WINDOW(data));
    update_mode(SFE_WINDOW(data));
    update_preview(SFE_WINDOW(data));
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

/* The status corner shows while it has a notice or the mode. */
static void update_status(SfeWindow* win)
{
    gtk_widget_set_visible(win->status, gtk_widget_get_visible(win->notice) || gtk_widget_get_visible(win->mode));
}

static gboolean on_notice_timeout(gpointer data)
{
    SfeWindow* win = data;
    win->notice_timeout = 0;
    gtk_widget_hide(win->notice);
    update_status(win);
    return G_SOURCE_REMOVE;
}

guint sfe_window_show_status(SfeWindow* win, const char* text)
{
    gtk_label_set_text(GTK_LABEL(win->notice), text);
    gtk_widget_show(win->notice);
    update_status(win);
    g_clear_handle_id(&win->notice_timeout, g_source_remove);
    win->notice_timeout = g_timeout_add_seconds(5, on_notice_timeout, win);
    return ++win->notice_id;
}

void sfe_window_update_status(SfeWindow* win, guint id, const char* text)
{
    if (id == win->notice_id && gtk_widget_get_visible(win->notice))
        gtk_label_set_text(GTK_LABEL(win->notice), text);
}

/* Places the status corner above the command line while that is open (its
 * completion list grows upwards), else at the bottom. */
static gboolean on_overlay_position(GtkOverlay* overlay, GtkWidget* child, GdkRectangle* alloc, gpointer data)
{
    SfeWindow* win = data;
    if (child != win->status)
        return FALSE;
    GtkRequisition natural;
    gtk_widget_get_preferred_size(child, NULL, &natural);
    int h = gtk_widget_get_allocated_height(GTK_WIDGET(overlay));
    int w = gtk_widget_get_allocated_width(GTK_WIDGET(overlay));
    int below = 0;
    if (gtk_widget_get_visible(GTK_WIDGET(win->cmdbar))) {
        int min = 0, nat = 0;
        gtk_widget_get_preferred_height_for_width(GTK_WIDGET(win->cmdbar), w, &min, &nat);
        below = MIN(nat, h);
    }
    alloc->x = 0;
    alloc->width = MIN(natural.width, w);
    alloc->height = natural.height;
    alloc->y = MAX(0, h - below - natural.height);
    return TRUE;
}

/* "VISUAL" in the corner while the active view selects with v. */
static void update_mode(SfeWindow* win)
{
    SfePane* pane = sfe_window_get_active_pane(win);
    if (!sfe_pane_in_visual(pane)) {
        gtk_widget_hide(win->mode);
        update_status(win);
        return;
    }
    GList* files = sfe_pane_get_selected_files(pane);
    guint n = g_list_length(files);
    g_list_free_full(files, g_object_unref);
    g_autofree char* text = g_strdup_printf("-- VISUAL --  %u selected%s", n,
        sfe_pane_visual_drawing(pane) ? "" : "  (v starts a chunk)");
    gtk_label_set_text(GTK_LABEL(win->mode), text);
    gtk_widget_show(win->mode);
    update_status(win);
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
    bool visual = sfe_pane_in_visual(sfe_window_get_active_pane(win)); /* visual keys first */
    SfeKeysResult result = sfe_keymap_lookup_layered(visual ? win->visual_keymap : NULL, win->keymap, keys,
        win->pending->len, &command, &prefix);

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
    if (win->chooser != NULL && (event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter)
        && (event->state & gtk_accelerator_get_default_mod_mask()) == GDK_CONTROL_MASK) {
        accept(win); /* Ctrl+Enter: the Open / Save / Select button */
        return TRUE;
    }
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

void sfe_window_edit_command(SfeWindow* win, SfePane* pane, const char* text, int start, int end)
{
    if (pane != NULL && sfe_window_get_active_pane(win) != pane)
        sfe_split_focus(win->split, pane); /* commands run in the active view */
    sfe_command_bar_start_selected(win->cmdbar, text, start, end);
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
    g_clear_handle_id(&win->notice_timeout, g_source_remove);
    win->ops_bar = NULL; /* destroyed with the window's children */
    if (win->pending != NULL)
        drop_chord(win);
    g_clear_pointer(&win->pending, g_ptr_array_unref);
    g_clear_pointer(&win->keymap, sfe_keymap_free);
    g_clear_pointer(&win->visual_keymap, sfe_keymap_free);
    g_clear_pointer(&win->filters, g_ptr_array_unref);
    g_clear_pointer(&win->folder_filter, sfe_file_filter_free);
    g_clear_pointer(&win->choice_widgets, g_ptr_array_unref);
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
    g_signal_connect(win->split, "selection-changed", G_CALLBACK(on_selection_changed), win);
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
    gtk_style_context_add_class(gtk_widget_get_style_context(win->mode), "sfe-mode");
    gtk_label_set_xalign(GTK_LABEL(win->mode), 0);
    gtk_widget_set_no_show_all(win->mode, TRUE);
    win->notice = gtk_label_new(NULL);
    gtk_style_context_add_class(gtk_widget_get_style_context(win->notice), "sfe-notice");
    gtk_label_set_xalign(GTK_LABEL(win->notice), 0);
    gtk_label_set_ellipsize(GTK_LABEL(win->notice), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_no_show_all(win->notice, TRUE);
    win->status = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_style_context_add_class(gtk_widget_get_style_context(win->status), "sfe-showcmd");
    gtk_style_context_add_class(gtk_widget_get_style_context(win->status), "sfe-status");
    gtk_box_pack_start(GTK_BOX(win->status), win->notice, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(win->status), win->mode, FALSE, FALSE, 0);
    gtk_widget_set_no_show_all(win->status, TRUE);
    GtkWidget* overlay = gtk_overlay_new();
    gtk_container_add(GTK_CONTAINER(overlay), GTK_WIDGET(win->split));
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), GTK_WIDGET(win->cmdbar));
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), win->chord_box);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), win->status);
    gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(overlay), win->status, TRUE);
    g_signal_connect(overlay, "get-child-position", G_CALLBACK(on_overlay_position), win);

    /* sidebar | (panes over the operations bar) */
    GtkWidget* content = win->content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
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

/* ------------------------------------------------------------------------ */
/* Chooser mode                                                              */
/* ------------------------------------------------------------------------ */

/* Prints the answer (file URIs, one per line) and ends: exit status 0. */
static void finish_choosing(SfeWindow* win, GList* files)
{
    /* "#" lines first: the filter in use and the options' values */
    if (win->filters != NULL && win->filters->len > 0) {
        g_autofree char* line = g_strdup_printf("#filter %d", win->filter_index);
        sfe_application_answer(sfe_app(), line);
    }
    for (guint i = 0; win->choice_widgets != NULL && i < win->choice_widgets->len; i++) {
        GtkWidget* w = g_ptr_array_index(win->choice_widgets, i);
        const char* id = g_object_get_data(G_OBJECT(w), "sfe-choice-id");
        const char* value = GTK_IS_TOGGLE_BUTTON(w)
            ? (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w)) ? "true" : "false")
            : gtk_combo_box_get_active_id(GTK_COMBO_BOX(w));
        g_autofree char* line = g_strdup_printf("#choice %s %s", id, value ? value : "");
        sfe_application_answer(sfe_app(), line);
    }
    for (GList* l = files; l != NULL; l = l->next) {
        g_autofree char* uri = g_file_get_uri(l->data);
        sfe_application_answer(sfe_app(), uri);
    }
    win->chosen = true;
    sfe_application_set_exit_status(sfe_app(), 0);
    g_application_quit(G_APPLICATION(sfe_app()));
}

static bool is_folder(GFile* file)
{
    return g_file_query_file_type(file, G_FILE_QUERY_INFO_NONE, NULL) == G_FILE_TYPE_DIRECTORY;
}

static void on_replace_confirmed(gpointer data)
{
    GFile* file = data;
    SfeWindow* win = g_object_get_data(G_OBJECT(file), "sfe-window");
    GList one = { .data = file };
    finish_choosing(win, &one);
}

/* Open / Save / Select. */
static void accept(SfeWindow* win)
{
    SfePane* pane = sfe_window_get_active_pane(win);
    GFile* dir = sfe_pane_get_dir(pane);
    GList* selected = sfe_pane_get_selected_files(pane);
    GList* files = NULL;
    GFile* folder = NULL; /* a selected folder (open: walk into it) */

    for (GList* l = selected; l != NULL; l = l->next) {
        if (is_folder(l->data))
            folder = folder ? folder : l->data;
        else
            files = g_list_append(files, l->data);
    }
    switch (win->chooser->mode) {
    case SFE_CHOOSE_OPEN:
        if (files == NULL && folder != NULL) {
            sfe_pane_navigate(pane, folder);
        } else if (files != NULL) {
            if (!win->chooser->multiple && files->next != NULL) {
                g_list_free(files->next); /* one file asked for: the first */
                files->next = NULL;
            }
            finish_choosing(win, files);
        }
        break;
    case SFE_CHOOSE_FOLDER: {
        GList one = { .data = folder ? folder : dir };
        finish_choosing(win, &one);
        break;
    }
    case SFE_CHOOSE_SAVE: {
        const char* name = gtk_entry_get_text(GTK_ENTRY(win->chooser_name));
        if (*name == '\0' || strchr(name, '/') != NULL)
            break;
        GFile* file = g_file_get_child_for_display_name(dir, name, NULL);
        if (file == NULL)
            break;
        if (g_file_query_exists(file, NULL)) { /* the program will overwrite it: ask */
            g_autofree char* question = g_strdup_printf("Replace “%s”?", name);
            g_object_set_data(G_OBJECT(file), "sfe-window", win);
            sfe_confirm(GTK_WIDGET(win), question, "A file with this name already exists here.", "Replace", true,
                on_replace_confirmed, file, g_object_unref);
            break;
        }
        GList one = { .data = file };
        finish_choosing(win, &one);
        g_object_unref(file);
        break;
    }
    }
    g_list_free(files);
    g_list_free_full(selected, g_object_unref);
}

bool sfe_window_chooser_pick(SfeWindow* win, GFile* file)
{
    if (win->chooser == NULL || is_folder(file))
        return false;
    if (win->chooser->mode == SFE_CHOOSE_SAVE) { /* the name to save as */
        g_autofree char* base = g_file_get_basename(file);
        g_autofree char* name = g_filename_display_name(base);
        gtk_entry_set_text(GTK_ENTRY(win->chooser_name), name);
        gtk_widget_grab_focus(win->chooser_name);
        return true;
    }
    if (win->chooser->mode == SFE_CHOOSE_OPEN && win->chooser->multiple) {
        accept(win); /* Enter / double-click: everything selected */
    } else if (win->chooser->mode == SFE_CHOOSE_OPEN) {
        GList one = { .data = file };
        finish_choosing(win, &one);
    }
    return true;
}

static void on_accept(GtkButton* button, gpointer data)
{
    (void)button;
    accept(data);
}

static void on_cancel(GtkButton* button, gpointer data)
{
    (void)button;
    (void)data;
    g_application_quit(G_APPLICATION(sfe_app())); /* exit status stays 1 */
}

/* Attaches the dialog to the window of the program that asked (the portal
 * passes "wayland:HANDLE", an xdg-foreign handle, or "x11:XID"), so the
 * compositor treats it as that program's dialog. */
static void attach_to_parent(GtkWidget* widget, gpointer data)
{
    (void)data;
    SfeWindow* win = SFE_WINDOW(widget);
    const char* parent = win->chooser ? win->chooser->parent : NULL;
    GdkWindow* window = gtk_widget_get_window(widget);
    if (parent == NULL || window == NULL)
        return;
#ifdef GDK_WINDOWING_WAYLAND
    if (g_str_has_prefix(parent, "wayland:") && GDK_IS_WAYLAND_WINDOW(window)) {
        if (!gdk_wayland_window_set_transient_for_exported(window, (char*)parent + strlen("wayland:")))
            g_debug("chooser: cannot attach to %s", parent);
        return;
    }
#endif
#ifdef GDK_WINDOWING_X11
    if (g_str_has_prefix(parent, "x11:") && GDK_IS_X11_WINDOW(window)) {
        guint64 xid = g_ascii_strtoull(parent + strlen("x11:"), NULL, 16);
        GdkWindow* foreign = gdk_x11_window_foreign_new_for_display(gdk_window_get_display(window), (Window)xid);
        if (foreign != NULL) {
            gdk_window_set_transient_for(window, foreign);
            g_object_unref(foreign);
        }
    }
#endif
}

/* The type filter in use, for every view. */
static void apply_filter(SfeWindow* win)
{
    const SfeFileFilter* filter = win->folder_filter ? win->folder_filter
        : win->filters && win->filter_index >= 0 && win->filter_index < (int)win->filters->len
        ? g_ptr_array_index(win->filters, win->filter_index)
        : NULL;
    for (int i = 0; i < sfe_split_get_n_panes(win->split); i++)
        sfe_pane_set_file_filter(sfe_split_get_pane(win->split, i), filter);
}

/* A dialog for one file (or folder, or a name to save as) selects one
 * item at a time; only "open several files" selects more. */
static void apply_selection_mode(SfeWindow* win)
{
    bool single = !(win->chooser->mode == SFE_CHOOSE_OPEN && win->chooser->multiple);
    for (int i = 0; i < sfe_split_get_n_panes(win->split); i++)
        sfe_pane_set_single_selection(sfe_split_get_pane(win->split, i), single);
}

static void on_filter_changed(GtkComboBox* combo, gpointer data)
{
    SfeWindow* win = data;
    win->filter_index = gtk_combo_box_get_active(combo);
    apply_filter(win);
}

/* "Images ▾": the filters the program offered. */
static GtkWidget* filter_combo(SfeWindow* win)
{
    g_autoptr(GError) error = NULL;
    win->filters = sfe_file_filters_parse(win->chooser->filters, &error);
    if (win->filters == NULL) {
        g_warning("chooser: filters: %s", error->message);
        return NULL;
    }
    if (win->filters->len == 0)
        return NULL;
    GtkWidget* combo = gtk_combo_box_text_new();
    for (guint i = 0; i < win->filters->len; i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), ((SfeFileFilter*)g_ptr_array_index(win->filters, i))->name);
    int start = win->chooser->current_filter;
    win->filter_index = start >= 0 && start < (int)win->filters->len ? start : 0;
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), win->filter_index);
    sfe_style_combo_box(combo);
    g_signal_connect(combo, "changed", G_CALLBACK(on_filter_changed), win);
    apply_filter(win);
    return combo;
}

/* The program's extra options: a check box for a yes / no one, else a
 * drop-down of its values. */
static void add_choices(SfeWindow* win, GtkBox* row)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) v = g_variant_parse(G_VARIANT_TYPE("a(ssa(ss)s)"), win->chooser->choices, NULL, NULL, &error);
    if (v == NULL) {
        g_warning("chooser: choices: %s", error->message);
        return;
    }
    win->choice_widgets = g_ptr_array_new();
    GVariantIter iter;
    const char *id, *label, *initial;
    GVariantIter* values;
    g_variant_iter_init(&iter, v);
    while (g_variant_iter_next(&iter, "(&s&sa(ss)&s)", &id, &label, &values, &initial)) {
        GtkWidget* widget;
        if (g_variant_iter_n_children(values) == 0) {
            widget = gtk_check_button_new_with_label(label);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), g_strcmp0(initial, "true") == 0);
            gtk_box_pack_start(row, widget, FALSE, FALSE, 0);
        } else {
            widget = gtk_combo_box_text_new();
            const char *value_id, *value_label;
            while (g_variant_iter_next(values, "(&s&s)", &value_id, &value_label))
                gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(widget), value_id, value_label);
            if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(widget), initial))
                gtk_combo_box_set_active(GTK_COMBO_BOX(widget), 0);
            sfe_style_combo_box(widget);
            gtk_box_pack_start(row, gtk_label_new(label), FALSE, FALSE, 0);
            gtk_box_pack_start(row, widget, FALSE, FALSE, 0);
        }
        g_variant_iter_free(values);
        g_object_set_data_full(G_OBJECT(widget), "sfe-choice-id", g_strdup(id), g_free);
        g_ptr_array_add(win->choice_widgets, widget);
    }
}

void sfe_window_set_chooser(SfeWindow* win, const SfeChooser* chooser)
{
    static const char* const titles[] = { "Open", "Save", "Select Folder" };
    static const char* const labels[] = { "Open", "Save", "Select" };
    win->chooser = chooser;
    apply_selection_mode(win);
    gtk_window_set_title(GTK_WINDOW(win), chooser->title ? chooser->title : titles[chooser->mode]);
    if (chooser->parent != NULL) {
        gtk_window_set_type_hint(GTK_WINDOW(win), GDK_WINDOW_TYPE_HINT_DIALOG);
        if (gtk_widget_get_realized(GTK_WIDGET(win)))
            attach_to_parent(GTK_WIDGET(win), NULL);
        else
            g_signal_connect_after(win, "realize", G_CALLBACK(attach_to_parent), NULL); /* (the GdkWindow exists then) */
    }
    gtk_window_set_default_size(GTK_WINDOW(win), chooser->mode == SFE_CHOOSE_OPEN ? 1060 : 900, 640);

    /* Opening files: the views get the preview on their right:
     * [views | preview]. Saving and folders need none. */
    if (chooser->mode == SFE_CHOOSE_OPEN) {
        GList* children = gtk_container_get_children(GTK_CONTAINER(win->content));
        GtkWidget* views = g_object_ref(children->data);
        g_list_free(children);
        gtk_container_remove(GTK_CONTAINER(win->content), views);
        GtkWidget* paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
        win->preview = SFE_PREVIEW(sfe_preview_new());
        gtk_paned_pack1(GTK_PANED(paned), views, TRUE, FALSE);
        gtk_paned_pack2(GTK_PANED(paned), GTK_WIDGET(win->preview), FALSE, FALSE);
        g_object_unref(views);
        gtk_box_pack_start(GTK_BOX(win->content), paned, TRUE, TRUE, 0);
        gtk_box_reorder_child(GTK_BOX(win->content), paned, 0);
        gtk_widget_show(paned);
        gtk_widget_show(GTK_WIDGET(win->preview));
        update_preview(win);
    }

    /* [Name  ____________]                      (save only)
     * [Images ▾] [options] hint ...  [Cancel] [Open] */
    GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(bar), "sfe-chooser-bar");
    if (chooser->mode == SFE_CHOOSE_SAVE) {
        GtkWidget* name_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        win->chooser_name = gtk_entry_new();
        gtk_entry_set_text(GTK_ENTRY(win->chooser_name), chooser->name ? chooser->name : "");
        gtk_entry_set_placeholder_text(GTK_ENTRY(win->chooser_name), "File name");
        g_signal_connect_swapped(win->chooser_name, "activate", G_CALLBACK(accept), win);
        gtk_box_pack_start(GTK_BOX(name_row), gtk_label_new("Name"), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(name_row), win->chooser_name, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(bar), name_row, FALSE, FALSE, 0);
    }
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    /* A folder dialog lists only folders: the program's file types (some
     * send them anyway) do not apply. */
    GtkWidget* combo = NULL;
    if (chooser->mode == SFE_CHOOSE_FOLDER) {
        win->folder_filter = sfe_file_filter_new_folders_only();
        apply_filter(win);
    } else if (chooser->filters != NULL) {
        combo = filter_combo(win);
    }
    if (combo != NULL)
        gtk_box_pack_start(GTK_BOX(row), combo, FALSE, FALSE, 0);
    if (chooser->choices != NULL)
        add_choices(win, GTK_BOX(row));
    if (chooser->mode != SFE_CHOOSE_SAVE) {
        /* the button's label as shown: "_Upload" → "Upload" */
        g_autofree char* button = g_strdup(chooser->accept_label ? chooser->accept_label : labels[chooser->mode]);
        char* underscore = strchr(button, '_');
        if (underscore != NULL)
            memmove(underscore, underscore + 1, strlen(underscore));
        g_autofree char* folder_hint = g_strdup_printf("Pick a folder, or %s for the one shown", button);
        GtkWidget* hint = gtk_label_new(chooser->mode == SFE_CHOOSE_FOLDER ? folder_hint
                : chooser->multiple ? "Pick files (Ctrl+click for several)" : "Pick a file");
        gtk_style_context_add_class(gtk_widget_get_style_context(hint), "sfe-placeholder");
        gtk_label_set_xalign(GTK_LABEL(hint), 0);
        gtk_label_set_ellipsize(GTK_LABEL(hint), PANGO_ELLIPSIZE_END);
        gtk_box_pack_start(GTK_BOX(row), hint, TRUE, TRUE, 0);
    }
    GtkWidget* cancel = gtk_button_new_with_label("Cancel");
    /* (the portal allows mnemonics: GTK programs send "_Open") */
    GtkWidget* ok = gtk_button_new_with_mnemonic(chooser->accept_label ? chooser->accept_label : labels[chooser->mode]);
    gtk_style_context_add_class(gtk_widget_get_style_context(ok), "suggested-action");
    g_signal_connect(cancel, "clicked", G_CALLBACK(on_cancel), win);
    g_signal_connect(ok, "clicked", G_CALLBACK(on_accept), win);
    gtk_box_pack_end(GTK_BOX(row), ok, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(row), cancel, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), row, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(win->content), bar, FALSE, FALSE, 0);
    gtk_widget_show_all(bar);
    if (win->chooser_name != NULL) {
        gtk_widget_grab_focus(win->chooser_name);
        /* "notes.txt": the name selected without the extension, ready to type over */
        const char* text = gtk_entry_get_text(GTK_ENTRY(win->chooser_name));
        const char* dot = strrchr(text, '.');
        gtk_editable_select_region(GTK_EDITABLE(win->chooser_name), 0,
            dot && dot != text ? (int)g_utf8_pointer_to_offset(text, dot) : -1);
    }
}
