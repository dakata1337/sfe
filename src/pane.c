#include "pane.h"
#include "theme.h"

#include "actions.h"
#include "app.h"
#include "ctxmenu.h"
#include "dialogs.h"
#include "dirmodel.h"
#include "fuzzy.h"
#include "history.h"
#include "opener.h"
#include "pathentry.h"
#include "search.h"
#include "thumbs.h"
#include "window.h"
#include <math.h>

#define LIST_ICON_SIZE 16
#define ROW_PADDING 4 /* vertical padding of list rows, in pixels */
#define MOUSE_BUTTON_BACK 8
#define MOUSE_BUTTON_FORWARD 9

struct _SfePane {
    GtkBox parent;

    SfeDirModel* model;
    SfeHistory* history;
    GFile* dir;
    SfeViewMode view;
    char* select_after_load; /* name of the entry to select once listed */
    GHashTable* reveal_names; /* just-created files to select when they appear, or NULL */
    bool reveal_replace; /* the next one revealed replaces the selection */

    /* Sort settings last applied from the config (every view sorts the
     * same way; column clicks change the config). */
    int cfg_sort_by;
    int cfg_sort_order;
    bool cfg_dirs_first;

    GtkWidget* back_button;
    GtkWidget* forward_button;
    GtkWidget* up_button;
    GtkWidget* entry;
    GtkWidget* view_button;
    GtkWidget* split_v_button;
    GtkWidget* split_h_button;
    GtkWidget* close_button;
    GtkWidget* stack;
    GtkWidget* overlay; /* stack + floating search bar */
    SfeSearchBar* search;
    GFile* search_base; /* where the search started (it may walk into folders) */
    guint search_timer; /* the typed text is searched for when typing pauses */
    char* filter; /* the query kept after Enter in the search, NULL when not filtered */
    GtkWidget* filter_badge; /* "Filtered: …" over the files, with a clear button */
    GtkWidget* filter_label;
    GtkWidget* grid; /* GtkIconView */
    GtkWidget* list; /* GtkTreeView */
    GtkWidget* message; /* GtkLabel for errors */
    GtkWidget* message_box; /* holds it; takes the focus so the pane can be active */
    GtkCellRenderer* grid_text;
    GtkCellRenderer* grid_icon;
    GtkTreeViewColumn* columns[4]; /* indexed by SfeSortKey */
    gulong config_handler;

    /* Thumbnails of the rows on screen (thumbs.c). */
    GCancellable* thumb_cancel; /* the requests for what was on screen last */
    guint thumb_source; /* a pending look at what is on screen */
    int cfg_thumbnails; /* the config value last applied, -1 at first */
    int cfg_folder_covers; /* the same */
    int thumb_size; /* SfeThumbSize in use, -1 at first */

    /* Room after the files, to right-click empty space. Per view (grid, list): */
    double scroll_wanted[2]; /* where it was scrolled to */
    double content_end[2]; /* where its last row ends */
    bool padding; /* our own changes to the adjustments */
    gint64 cursor_scroll_until; /* a keyboard cursor move scrolls until then (monotonic time) */
    /* A drag-select (rubber band) in progress, in content coordinates: GTK
     * draws it only over the files, sfe draws the rest. */
    bool band_pressed, band_active;
    bool band_own; /* grid band started on a file or below the items: GTK does not band there, we do */
    GHashTable* band_base; /* with Ctrl: row indexes (+1) selected before the band */
    double band_x1, band_y1, band_x2, band_y2;
    /* Drag and drop: a press on a selected file, which may become a drag
     * (in bin coordinates), and the drag coming in. */
    GtkTreePath* drag_path;
    double drag_x, drag_y;
    GdkDragContext* drop_ctx;
    GList* drop_files; /* GFile*, once its data arrived */
    bool drop_ready, drop_now;
    int drop_x, drop_y; /* widget coordinates */
    /* Grid: where a Shift-selection starts (the last item picked without
     * Shift), or NULL for the cursor. */
    GtkTreeRowReference* grid_anchor;

    /* List columns that make way for the names in a narrow view. */
    int name_width; /* widest name in pixels (capped), -1 to measure again */
    int column_width[4]; /* by SfeSortKey: what Size, Modified and Type need */
    int columns_for_width; /* list width the columns were fitted to */
    guint columns_source;

    /* Visual mode: the selection is `visual_base` (chunks already drawn,
     * as GFile*) plus the rows from `visual_anchor` to the cursor. */
    bool visual;
    GtkTreeRowReference* visual_anchor; /* NULL between chunks */
    GHashTable* visual_base; /* GFile* set */

    guint selection_source; /* a pending "selection-changed" */
    bool single; /* one item at a time (a file dialog for one file) */
};

G_DEFINE_TYPE(SfePane, sfe_pane, GTK_TYPE_BOX)

enum {
    SIGNAL_DIRECTORY_CHANGED,
    SIGNAL_VISUAL_CHANGED,
    SIGNAL_SPLIT,
    SIGNAL_CLOSE,
    SIGNAL_SELECTION_CHANGED,
    N_SIGNALS,
};
static guint signals[N_SIGNALS];

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

static GtkWidget* active_view(SfePane* self)
{
    return self->view == SFE_VIEW_GRID ? self->grid : self->list;
}

static bool focus_inside(SfePane* self)
{
    GtkWidget* toplevel = gtk_widget_get_toplevel(GTK_WIDGET(self));
    GtkWidget* focus = GTK_IS_WINDOW(toplevel) ? gtk_window_get_focus(GTK_WINDOW(toplevel)) : NULL;
    return focus != NULL && gtk_widget_is_ancestor(focus, GTK_WIDGET(self));
}

/* An error in place of the files ("Cannot open this folder"). The message
 * takes the focus the files had: a pane must stay focusable to be active
 * (keys, clicks, closing it), even in a folder it cannot read. */
static void show_message(SfePane* self, const char* text)
{
    GtkWidget* view = active_view(self);
    bool had_focus = gtk_widget_has_focus(view) || focus_inside(self);
    gtk_label_set_text(GTK_LABEL(self->message), text);
    gtk_stack_set_visible_child(GTK_STACK(self->stack), self->message_box);
    if (had_focus || gtk_style_context_has_class(gtk_widget_get_style_context(GTK_WIDGET(self)), "sfe-active"))
        gtk_widget_grab_focus(self->message_box);
}

static gboolean on_message_press(GtkWidget* box, GdkEventButton* event, gpointer data)
{
    (void)event, (void)data;
    gtk_widget_grab_focus(box); /* a click makes this pane the active one */
    return FALSE;
}

static void update_buttons(SfePane* self)
{
    g_autoptr(GFile) parent = self->dir ? g_file_get_parent(self->dir) : NULL;

    gtk_widget_set_sensitive(self->back_button, sfe_history_can_back(self->history));
    gtk_widget_set_sensitive(self->forward_button, sfe_history_can_forward(self->history));
    gtk_widget_set_sensitive(self->up_button, parent != NULL);
    gtk_button_set_image(GTK_BUTTON(self->view_button),
        sfe_scaled_icon_new(self->view == SFE_VIEW_GRID ? "view-list-symbolic" : "view-grid-symbolic", 16));
    gtk_widget_set_tooltip_text(self->view_button,
        self->view == SFE_VIEW_GRID ? "List view (Ctrl+2)" : "Grid view (Ctrl+1)");
}

static void update_sort_indicators(SfePane* self)
{
    SfeSortKey key;
    GtkSortType order;

    sfe_dir_model_get_sort(self->model, &key, &order);
    for (int i = 0; i < (int)G_N_ELEMENTS(self->columns); i++) {
        gtk_tree_view_column_set_sort_indicator(self->columns[i], i == (int)key);
        gtk_tree_view_column_set_sort_order(self->columns[i], order);
    }
}

/* Selects and reveals one row of the active view. */
static void forget_anchor(SfePane* self)
{
    g_clear_pointer(&self->grid_anchor, gtk_tree_row_reference_free);
}

#define CURSOR_SCROLL_USEC G_USEC_PER_SEC /* long enough for a scroll animation */

/* The cursor is about to move by the keyboard (or to a new file): the view
 * may scroll to it a moment later, outside the key event, which
 * on_scroll_value() would otherwise take for the view pulling itself back
 * from the empty space after the files, and undo (gg would select the first
 * row without scrolling up to it). */
static void cursor_scrolls(SfePane* self)
{
    self->scroll_wanted[0] = self->scroll_wanted[1] = 0;
    /* The scroll is animated: its later frames come without input too. */
    self->cursor_scroll_until = g_get_monotonic_time() + CURSOR_SCROLL_USEC;
}

static void select_path(SfePane* self, GtkTreePath* path)
{
    forget_anchor(self);
    cursor_scrolls(self);
    if (self->view == SFE_VIEW_GRID) {
        gtk_icon_view_unselect_all(GTK_ICON_VIEW(self->grid));
        gtk_icon_view_set_cursor(GTK_ICON_VIEW(self->grid), path, NULL, FALSE);
        gtk_icon_view_select_path(GTK_ICON_VIEW(self->grid), path);
        gtk_icon_view_scroll_to_path(GTK_ICON_VIEW(self->grid), path, TRUE, 0.5, 0.5);
    } else {
        gtk_tree_view_set_cursor(GTK_TREE_VIEW(self->list), path, NULL, FALSE);
        gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(self->list), path, NULL, TRUE, 0.5, 0);
    }
}

/* Gives the model only to the visible view, so the hidden one does no work. */
static void attach_model(SfePane* self)
{
    GtkTreeModel* model = sfe_dir_model_get_model(self->model);

    gtk_icon_view_set_model(GTK_ICON_VIEW(self->grid), self->view == SFE_VIEW_GRID ? model : NULL);
    gtk_tree_view_set_model(GTK_TREE_VIEW(self->list), self->view == SFE_VIEW_LIST ? model : NULL);
}

/* ------------------------------------------------------------------------ */
/* Navigation                                                                */
/* ------------------------------------------------------------------------ */

/* Shows `dir` without touching the history. */
/* Shows `dir` without touching the history or the search. */
static void forget_scroll(SfePane* self);

static void show_dir(SfePane* self, GFile* dir)
{
    g_debug("pane: show %s", g_file_peek_path(dir));
    sfe_pane_visual_end(self);
    forget_scroll(self);
    forget_anchor(self);
    g_set_object(&self->dir, dir);
    sfe_path_entry_set_dir(self->entry, dir);
    update_buttons(self);
    sfe_dir_model_load(self->model, dir);
    g_signal_emit(self, signals[SIGNAL_DIRECTORY_CHANGED], 0);
}

static void end_filter(SfePane* self, bool keep);

/* Shows `dir` without touching the history; a new folder starts unfiltered. */
static void go_to(SfePane* self, GFile* dir)
{
    if (gtk_widget_get_visible(GTK_WIDGET(self->search))) {
        gtk_widget_hide(GTK_WIDGET(self->search));
        sfe_search_bar_reset(self->search);
        g_clear_object(&self->search_base);
    }
    end_filter(self, false);
    sfe_dir_model_set_query(self->model, NULL);
    show_dir(self, dir);
}

void sfe_pane_navigate(SfePane* self, GFile* dir)
{
    if (self->dir != NULL && g_file_equal(self->dir, dir)) {
        sfe_pane_reload(self);
        return;
    }
    if (self->dir != NULL)
        sfe_history_push(self->history, self->dir);
    g_clear_pointer(&self->select_after_load, g_free);
    go_to(self, dir);
}

void sfe_pane_go_back(SfePane* self)
{
    g_autoptr(GFile) dir = sfe_history_back(self->history, self->dir);
    if (dir != NULL)
        go_to(self, dir);
}

void sfe_pane_go_forward(SfePane* self)
{
    g_autoptr(GFile) dir = sfe_history_forward(self->history, self->dir);
    if (dir != NULL)
        go_to(self, dir);
}

void sfe_pane_go_up(SfePane* self)
{
    g_autoptr(GFile) parent = self->dir ? g_file_get_parent(self->dir) : NULL;
    if (parent == NULL)
        return;
    /* Highlight the directory we came from. */
    g_autofree char* name = g_file_get_basename(self->dir);
    sfe_pane_navigate(self, parent);
    self->select_after_load = g_steal_pointer(&name);
}

void sfe_pane_reload(SfePane* self)
{
    sfe_dir_model_reload(self->model);
}

GFile* sfe_pane_get_dir(SfePane* self)
{
    return self->dir;
}

/* Opens a directory in this pane or a file in its application. */
static void activate_file(SfePane* self, GFile* file, GFileInfo* info)
{
    g_autoptr(GError) error = NULL;
    GtkWidget* toplevel = gtk_widget_get_toplevel(GTK_WIDGET(self));

    if (SFE_IS_WINDOW(toplevel) && sfe_window_chooser_pick(SFE_WINDOW(toplevel), file))
        return; /* a file dialog: picking is choosing */

    if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
        sfe_pane_navigate(self, file);
        return;
    }
    if (!sfe_open_file(sfe_config(), file, &error)) {
        g_autofree char* name = g_file_get_basename(file);
        g_autofree char* title = g_strdup_printf("Cannot open “%s”", name);
        sfe_show_error(GTK_WIDGET(self), title, error->message);
    }
}

static void activate_path(SfePane* self, GtkTreePath* path)
{
    g_autoptr(GFile) file = sfe_dir_model_file_at(self->model, path);
    g_autoptr(GFileInfo) info = sfe_dir_model_info_at(self->model, path);
    if (file != NULL && info != NULL)
        activate_file(self, file, info);
}

/* ------------------------------------------------------------------------ */
/* Model signals                                                             */
/* ------------------------------------------------------------------------ */

/* ------------------------------------------------------------------------ */
/* Search (Ctrl+F)                                                           */
/* ------------------------------------------------------------------------ */

static bool searching(SfePane* self)
{
    return gtk_widget_get_visible(GTK_WIDGET(self->search));
}

static int n_rows(SfePane* self)
{
    GtkTreeModel* model = sfe_dir_model_get_model(self->model);
    return model ? gtk_tree_model_iter_n_children(model, NULL) : 0;
}

/* The highlighted row: the view's cursor, else row 0. Free the result. */
static GtkTreePath* highlighted(SfePane* self)
{
    GtkTreePath* path = NULL;
    if (self->view == SFE_VIEW_GRID)
        gtk_icon_view_get_cursor(GTK_ICON_VIEW(self->grid), &path, NULL);
    else
        gtk_tree_view_get_cursor(GTK_TREE_VIEW(self->list), &path, NULL);
    if (path == NULL && n_rows(self) > 0)
        path = gtk_tree_path_new_first();
    return path;
}

static void select_index(SfePane* self, int index)
{
    GtkTreePath* path = gtk_tree_path_new_from_indices(index, -1);
    select_path(self, path);
    gtk_tree_path_free(path);
}

static void update_search_count(SfePane* self)
{
    int shown, matched, total;
    sfe_dir_model_get_counts(self->model, &shown, &matched, &total);
    sfe_search_bar_set_count(self->search, shown, matched, total);
}

void sfe_pane_start_search(SfePane* self)
{
    sfe_pane_visual_end(self);
    if (!searching(self)) {
        g_set_object(&self->search_base, self->dir);
        if (self->filter != NULL) { /* the filter goes back into the search */
            g_autofree char* text = g_steal_pointer(&self->filter);
            gtk_widget_hide(self->filter_badge);
            sfe_search_bar_set_text(self->search, text);
        }
    }
    sfe_search_bar_focus(self->search);
}

/* Hides the search bar and shows every file again. With `keep`, the
 * highlighted match stays selected in the full listing. With `go_back`, a
 * search that walked into a folder returns to where it started. */
static void apply_search(SfePane* self);

static void end_search_full(SfePane* self, bool keep, bool go_back)
{
    if (!searching(self))
        return;
    if (keep && self->search_timer != 0)
        apply_search(self); /* the match kept is one of what was typed */
    g_clear_handle_id(&self->search_timer, g_source_remove);

    g_autoptr(GFile) base = g_steal_pointer(&self->search_base);
    if (go_back && base != NULL && !g_file_equal(base, self->dir)) {
        go_to(self, base);
        sfe_pane_focus_view(self);
        return;
    }

    g_autofree char* name = NULL;
    if (keep) {
        GtkTreePath* path = highlighted(self);
        g_autoptr(GFile) file = path ? sfe_dir_model_file_at(self->model, path) : NULL;
        name = file ? g_file_get_basename(file) : NULL;
        if (path != NULL)
            gtk_tree_path_free(path);
    }
    gtk_widget_hide(GTK_WIDGET(self->search));
    sfe_search_bar_reset(self->search);
    sfe_dir_model_set_query(self->model, NULL);
    if (name != NULL)
        sfe_pane_reveal(self, name);
    sfe_pane_focus_view(self);
}

static void end_search(SfePane* self, bool keep)
{
    end_search_full(self, keep, true);
}

/* "src/pa" searches for "pa" inside the folder src (relative to where the
 * search started); "a/b/x" walks two levels. Only existing subfolders, and
 * never "." or ".." (the search only goes down). Returns the folder and
 * sets *query to the text after the last "/"; an invalid folder makes the
 * whole text the query, which matches nothing. */
static GFile* resolve_search_dir(GFile* base, const char* text, const char** query)
{
    const char* slash = strrchr(text, '/');
    GFile* dir = g_object_ref(base);

    *query = text;
    if (slash == NULL)
        return dir;

    g_autofree char* path = g_strndup(text, slash - text);
    g_auto(GStrv) parts = g_strsplit(path, "/", -1);
    for (char** part = parts; *part != NULL; part++) {
        if (**part == '\0')
            continue; /* "a//b" */
        GFile* child = strcmp(*part, ".") == 0 || strcmp(*part, "..") == 0 ? NULL : g_file_get_child(dir, *part);
        bool is_dir = child != NULL
            && g_file_query_file_type(child, G_FILE_QUERY_INFO_NONE, NULL) == G_FILE_TYPE_DIRECTORY;
        if (!is_dir) {
            g_clear_object(&child);
            g_object_unref(dir);
            return g_object_ref(base);
        }
        g_object_unref(dir);
        dir = child;
    }
    *query = slash + 1;
    return dir;
}

static void apply_search(SfePane* self)
{
    g_clear_handle_id(&self->search_timer, g_source_remove);
    const char* text = sfe_search_bar_get_text(self->search);
    const char* query = text;
    g_autoptr(GFile) dir = self->search_base ? resolve_search_dir(self->search_base, text, &query) : NULL;

    sfe_dir_model_set_query(self->model, query);
    if (dir != NULL && !g_file_equal(dir, self->dir))
        show_dir(self, dir); /* keeps the query while loading */
}

static gboolean on_search_timer(gpointer data)
{
    SfePane* self = data;
    self->search_timer = 0;
    apply_search(self);
    return G_SOURCE_REMOVE;
}

/* Keys that act on the matches need them up to date. */
static void flush_search(SfePane* self)
{
    if (self->search_timer != 0)
        apply_search(self);
}

/* Searching a big folder takes a while, so it waits until typing pauses
 * (search_delay_ms), and runs below drawing: the typed letter shows first. */
static void on_search_changed(SfeSearchBar* bar, const char* text, gpointer data)
{
    (void)bar;
    (void)text;
    SfePane* self = data;
    g_clear_handle_id(&self->search_timer, g_source_remove);
    self->search_timer = g_timeout_add_full(G_PRIORITY_DEFAULT_IDLE, (guint)MAX(0, sfe_config()->search_delay_ms),
        on_search_timer, self, NULL);
}

static void on_search_move(SfeSearchBar* bar, int delta, gpointer data)
{
    (void)bar;
    SfePane* self = data;
    flush_search(self);
    GtkTreePath* path = highlighted(self);
    int rows = n_rows(self);

    if (path == NULL || rows == 0)
        return;
    int index = CLAMP(gtk_tree_path_get_indices(path)[0] + delta, 0, rows - 1);
    gtk_tree_path_free(path);
    select_index(self, index);
}

static void on_search_close(SfeSearchBar* bar, gpointer data)
{
    (void)bar;
    end_search(SFE_PANE(data), true);
}

static void update_filter_badge(SfePane* self)
{
    if (self->filter == NULL)
        return;
    int shown, matched, total;
    sfe_dir_model_get_counts(self->model, &shown, &matched, &total);
    g_autofree char* count = matched > shown ? g_strdup_printf("best %d of %d", shown, matched)
                                             : g_strdup_printf("%d / %d", shown, total);
    g_autofree char* markup = g_markup_printf_escaped("Filtered: <b>%s</b>  <span alpha=\"60%%\">%s · Esc</span>",
        self->filter, count);
    gtk_label_set_markup(GTK_LABEL(self->filter_label), markup);
}

/* Shows every file again. With `keep`, the selected files stay selected in
 * the full listing. */
static void end_filter(SfePane* self, bool keep)
{
    if (self->filter == NULL)
        return;
    g_clear_pointer(&self->filter, g_free);
    gtk_widget_hide(self->filter_badge);
    if (!keep)
        return;
    GList* files = sfe_pane_get_selected_files(self);
    g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
    for (GList* l = files; l != NULL; l = l->next)
        g_ptr_array_add(names, g_file_get_basename(l->data));
    g_list_free_full(files, g_object_unref);
    sfe_dir_model_set_query(self->model, NULL);
    sfe_pane_reveal_many(self, (char* const*)names->pdata, names->len);
}

static void on_filter_clear(GtkButton* button, gpointer data)
{
    (void)button;
    SfePane* self = data;
    end_filter(self, true);
    sfe_pane_focus_view(self);
}

/* Enter: the matches stay as a filter of this view (the files can be worked
 * on as usual; Esc shows all again), the highlighted one selected. With
 * nothing typed, the search just closes where it is. */
static void search_activate(SfePane* self)
{
    flush_search(self);
    const char* query = NULL;
    g_autoptr(GFile) dir = resolve_search_dir(self->search_base ? self->search_base : self->dir,
        sfe_search_bar_get_text(self->search), &query);
    g_autofree char* filter = g_strdup(query);

    /* After walking into folders, Back returns to where the search started. */
    if (self->search_base != NULL && !g_file_equal(self->search_base, self->dir))
        sfe_history_push(self->history, self->search_base);
    if (*filter == '\0') {
        end_search_full(self, true, false);
        return;
    }
    g_clear_object(&self->search_base);
    gtk_widget_hide(GTK_WIDGET(self->search));
    sfe_search_bar_reset(self->search); /* the model keeps the query */
    self->filter = g_steal_pointer(&filter);
    update_filter_badge(self);
    gtk_widget_show(self->filter_badge);
    sfe_pane_focus_view(self);
}

/* Places the search card centred in the bottom third of the pane, the
 * filter badge in the bottom right corner. */
static gboolean on_overlay_position(GtkOverlay* overlay, GtkWidget* child, GdkRectangle* alloc, gpointer data)
{
    SfePane* self = data;
    int w = gtk_widget_get_allocated_width(GTK_WIDGET(overlay));
    int h = gtk_widget_get_allocated_height(GTK_WIDGET(overlay));
    if (child == self->filter_badge) { /* bottom right, clear of the scroll bar */
        GtkRequisition nat;
        gtk_widget_get_preferred_size(child, NULL, &nat);
        alloc->width = MIN(nat.width, w);
        alloc->height = nat.height;
        alloc->x = MAX(0, w - alloc->width - 16);
        alloc->y = MAX(0, h - alloc->height - 8);
        return TRUE;
    }
    int width = CLAMP(w * 6 / 10, MIN(300, w), 600);
    int min_h, nat_h;

    gtk_widget_get_preferred_height_for_width(child, width, &min_h, &nat_h);
    alloc->width = width;
    alloc->height = nat_h;
    alloc->x = (w - width) / 2;
    alloc->y = CLAMP(h * 5 / 6 - nat_h / 2, 0, MAX(0, h - nat_h));
    return TRUE;
}

static void schedule_thumbnails(SfePane* self);
static void names_changed(SfePane* self);

static void on_loaded(SfeDirModel* model, gpointer data)
{
    (void)model;
    SfePane* self = data;
    GtkWidget* view = active_view(self);
    bool had_focus = gtk_widget_has_focus(view) || gtk_widget_has_focus(self->message_box);

    forget_anchor(self); /* a new listing: the old rows are gone */
    attach_model(self);
    gtk_stack_set_visible_child(GTK_STACK(self->stack), gtk_widget_get_parent(view));
    update_sort_indicators(self);

    if (self->select_after_load != NULL) {
        GtkTreePath* path = sfe_dir_model_find(self->model, self->select_after_load);
        if (path != NULL) {
            select_path(self, path);
            gtk_tree_path_free(path);
        }
        g_clear_pointer(&self->select_after_load, g_free);
    }
    if (searching(self)) { /* the best match is highlighted */
        if (n_rows(self) > 0)
            select_index(self, 0);
        update_search_count(self);
    }
    update_filter_badge(self);
    if (had_focus)
        gtk_widget_grab_focus(view);
    schedule_thumbnails(self);
    names_changed(self);
}

static void on_load_error(SfeDirModel* model, GError* error, gpointer data)
{
    (void)model;
    SfePane* self = data;
    g_autofree char* text = g_strdup_printf("Cannot open this folder\n\n%s", error->message);
    show_message(self, text);
}

/* The directory was deleted or unmounted: go to the closest existing parent. */
static void on_gone(SfeDirModel* model, gpointer data)
{
    (void)model;
    SfePane* self = data;
    g_autoptr(GFile) dir = g_object_ref(self->dir);

    while (!g_file_query_exists(dir, NULL)) {
        GFile* parent = g_file_get_parent(dir);
        if (parent == NULL)
            break;
        g_object_unref(dir);
        dir = parent;
    }
    go_to(self, dir);
}

/* ------------------------------------------------------------------------ */
/* Config                                                                    */
/* ------------------------------------------------------------------------ */

/* Every grid item the same size, so GtkIconView does not
 * measure each name. Names get two lines, in the font in use: measured
 * again when the style changes (UI scale, font). */
static void fit_grid_items(SfePane* self)
{
    PangoLayout* two_lines = gtk_widget_create_pango_layout(self->grid, "Ag\nAg");
    int text_h, xpad, ypad;
    pango_layout_get_pixel_size(two_lines, NULL, &text_h);
    g_object_unref(two_lines);
    gtk_cell_renderer_get_padding(self->grid_text, &xpad, &ypad);
    /* Padding outside the wrap width, as GtkCellRendererText measures. */
    gtk_cell_renderer_set_fixed_size(self->grid_text,
        gtk_icon_view_get_item_width(GTK_ICON_VIEW(self->grid)) + 2 * xpad, text_h + 2 * ypad);
}

static void on_grid_style_updated(GtkWidget* grid, gpointer data)
{
    (void)grid;
    SfePane* self = data;
    if (self->grid != NULL) /* (not while it is being built) */
        fit_grid_items(self);
}

static void apply_icon_sizes(SfePane* self)
{
    const SfeConfig* cfg = sfe_config();
    int grid = (int)lround(cfg->icon_size * cfg->ui_scale);
    int list = (int)lround(LIST_ICON_SIZE * cfg->ui_scale);
    int item_width = MAX(grid + grid / 2, (int)lround(96 * cfg->ui_scale));

    sfe_dir_model_set_icon_sizes(self->model, grid, list, gtk_widget_get_scale_factor(GTK_WIDGET(self)));
    gtk_icon_view_set_item_width(GTK_ICON_VIEW(self->grid), item_width);
    g_object_set(self->grid_text, "wrap-width", item_width, NULL);
    gtk_cell_renderer_set_fixed_size(self->grid_icon, grid + 2, grid + 2);
    fit_grid_items(self);
}

/* ------------------------------------------------------------------------ */
/* Thumbnails                                                                */
/* ------------------------------------------------------------------------ */

#define THUMB_DELAY_MS 80 /* scrolling settles before asking */
#define THUMB_MARGIN 8 /* rows past the visible ones, for a little scrolling */

/* Icons are drawn from the model and items have a fixed size: a new
 * thumbnail needs a redraw, not a new layout of every item. */
static void on_thumbnails_changed(SfeDirModel* model, gpointer data)
{
    (void)model;
    gtk_widget_queue_draw(active_view(data));
}

static void on_thumbnail(GFile* file, GdkPixbuf* pixbuf, gpointer data)
{
    SfePane* self = data;
    sfe_dir_model_set_thumbnail(self->model, file, pixbuf);
}

/* Asks for the thumbnails of the rows on screen (and a few around them),
 * dropping the requests for rows that scrolled away. The queue serves the
 * newest requests first, so they are made bottom to top. */
static void request_thumbnails(SfePane* self)
{
    const SfeConfig* cfg = sfe_config();
    if (self->thumb_cancel != NULL)
        g_cancellable_cancel(self->thumb_cancel);
    g_clear_object(&self->thumb_cancel);
    if (!cfg->thumbnails || self->model == NULL)
        return;

    int pixels = (int)lround(cfg->icon_size * cfg->ui_scale) * gtk_widget_get_scale_factor(GTK_WIDGET(self));
    SfeThumbSize size = sfe_thumb_size_for(pixels);
    if ((int)size != self->thumb_size) { /* sharper ones needed: start over */
        if (self->thumb_size >= 0)
            sfe_dir_model_clear_thumbnails(self->model);
        self->thumb_size = (int)size;
    }

    GtkTreePath* start = NULL;
    GtkTreePath* end = NULL;
    bool visible = self->view == SFE_VIEW_GRID
        ? gtk_icon_view_get_visible_range(GTK_ICON_VIEW(self->grid), &start, &end)
        : gtk_tree_view_get_visible_range(GTK_TREE_VIEW(self->list), &start, &end);
    if (!visible && self->view == SFE_VIEW_LIST && n_rows(self) > 0) {
        /* The list gives no range when the bottom of the page is past its
         * last row (scrolled into the empty space): the rows on screen run
         * from the top one to the last. */
        gtk_tree_path_free(start);
        gtk_tree_path_free(end);
        start = end = NULL;
        visible = gtk_tree_view_get_visible_range(GTK_TREE_VIEW(self->list), &start, NULL);
        end = gtk_tree_path_new_from_indices(n_rows(self) - 1, -1);
    }
    if (!visible) {
        gtk_tree_path_free(start);
        gtk_tree_path_free(end);
        return;
    }
    int first = MAX(0, gtk_tree_path_get_indices(start)[0] - THUMB_MARGIN);
    int last = gtk_tree_path_get_indices(end)[0] + THUMB_MARGIN;
    gtk_tree_path_free(start);
    gtk_tree_path_free(end);
    g_debug("thumbnails: rows %d - %d", first, last);

    self->thumb_cancel = g_cancellable_new();
    GtkTreeModel* model = sfe_dir_model_get_model(self->model);
    SfeThumbQueue* queue = sfe_thumb_queue_get_default();
    guint64 max_bytes = (guint64)cfg->thumbnail_max_mb * 1024 * 1024;
    for (int i = last; i >= first; i--) {
        GtkTreeIter iter;
        if (model == NULL || !gtk_tree_model_iter_nth_child(model, &iter, NULL, i))
            continue;
        g_autoptr(GFile) file = NULL;
        g_autoptr(GFileInfo) info = NULL;
        gboolean is_dir = FALSE;
        gtk_tree_model_get(model, &iter, SFE_COL_FILE, &file, SFE_COL_INFO, &info, SFE_COL_IS_DIR, &is_dir, -1);
        if (file == NULL || !sfe_dir_model_wants_thumbnail(self->model, g_file_info_get_name(info)))
            continue;
        if (is_dir) { /* its cover picture, if it has one */
            gint64 mtime = (gint64)g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
            if (cfg->folder_covers && g_file_is_native(file) && !sfe_thumb_in_cache_dir(file))
                sfe_thumb_queue_request_folder(queue, file, mtime, size, self->thumb_cancel, on_thumbnail, self);
            else
                sfe_dir_model_set_thumbnail(self->model, file, NULL);
            continue;
        }
        const char* type = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE);
        g_autofree char* path = g_file_get_path(file);
        if (path == NULL || (guint64)g_file_info_get_size(info) > max_bytes || sfe_thumb_in_cache_dir(file)
            || !sfe_thumb_queue_supports(queue, type)) {
            sfe_dir_model_set_thumbnail(self->model, file, NULL); /* none to be had: not asked again */
            continue;
        }
        gint64 mtime = (gint64)g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
        sfe_thumb_queue_request(queue, file, type, mtime, size, self->thumb_cancel, on_thumbnail, self);
    }
}

static gboolean on_thumb_timeout(gpointer data)
{
    SfePane* self = data;
    self->thumb_source = 0;
    request_thumbnails(self);
    return G_SOURCE_REMOVE;
}

/* After scrolling, resizing, loading or a change of files. */
static void schedule_thumbnails(SfePane* self)
{
    if (self->thumb_source == 0)
        self->thumb_source = g_timeout_add(THUMB_DELAY_MS, on_thumb_timeout, self);
}

static void on_view_moved(GtkAdjustment* adjustment, gpointer data)
{
    (void)adjustment;
    schedule_thumbnails(data);
}

/* ------------------------------------------------------------------------ */
/* Room after the files                                                      */
/* ------------------------------------------------------------------------ */

/* Where the last row ends, in the view's scrolled coordinates (0 when
 * empty), and how tall a row is. */
static double content_bottom(SfePane* self, GtkWidget* view, GtkAdjustment* adjustment, double* row_height)
{
    *row_height = 0;
    int n = n_rows(self);
    if (n == 0 || gtk_widget_get_parent(gtk_widget_get_parent(view)) == NULL)
        return 0;
    GtkTreePath* last = gtk_tree_path_new_from_indices(n - 1, -1);
    GdkRectangle rect = { 0 };
    double bottom = 0;
    if (GTK_IS_ICON_VIEW(view)) {
        /* in widget coordinates: shifted by how far the view is scrolled */
        if (gtk_icon_view_get_cell_rect(GTK_ICON_VIEW(view), last, NULL, &rect)) {
            bottom = rect.y + rect.height + gtk_adjustment_get_value(adjustment);
            *row_height = rect.height + gtk_icon_view_get_row_spacing(GTK_ICON_VIEW(view));
        }
    } else {
        int tx = 0, ty = 0;
        gtk_tree_view_get_background_area(GTK_TREE_VIEW(view), last, NULL, &rect);
        gtk_tree_view_convert_bin_window_to_tree_coords(GTK_TREE_VIEW(view), rect.x, rect.y, &tx, &ty);
        bottom = ty + rect.height;
        *row_height = rect.height;
    }
    gtk_tree_path_free(last);
    return bottom;
}

#define PADDING_SHARE 3 /* a third of the view after the last row ... */
#define PADDING_ROWS_LIST 6 /* ... but at least this many rows */
#define PADDING_ROWS_GRID 1

/* The view sized its scroll range to the files: make it reach further, so
 * there is always empty space to right-click for the folder's menu. If the
 * view just cut the scroll position back (it re-measured while scrolled
 * into that space), put it back. */
static void on_scroll_range(GtkAdjustment* adjustment, gpointer data)
{
    SfePane* self = data;
    if (self->padding)
        return;
    int which = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(adjustment), "sfe-view-index"));
    GtkWidget* view = which == 0 ? self->grid : self->list;
    if (view != active_view(self))
        return;
    double page = gtk_adjustment_get_page_size(adjustment);
    double upper = gtk_adjustment_get_upper(adjustment);
    double row = 0;
    double bottom = content_bottom(self, view, adjustment, &row);
    double extra = MAX(page / PADDING_SHARE, row * (which == 0 ? PADDING_ROWS_GRID : PADDING_ROWS_LIST));
    double wanted_upper = bottom > 0 ? bottom + extra : upper;
    self->content_end[which] = bottom;

    self->padding = true;
    if (wanted_upper > upper + 0.5)
        gtk_adjustment_set_upper(adjustment, wanted_upper);
    double wanted = MIN(self->scroll_wanted[which], gtk_adjustment_get_upper(adjustment) - page);
    if (wanted > gtk_adjustment_get_value(adjustment)) /* as far as it still goes */
        gtk_adjustment_set_value(adjustment, wanted);
    self->padding = false;
}

/* ------------------------------------------------------------------------ */
/* Columns that make way for the names                                       */
/* ------------------------------------------------------------------------ */

#define NAME_CHARS_CAP 40 /* a longer name is not worth dropping columns for */
#define COLUMN_ARROW 20 /* a header's sort arrow and its gap */
#define COLUMN_PADDING 16 /* a cell's xpad on both sides, and GTK's own */

static int text_width(PangoLayout* layout, const char* text)
{
    int w = 0;
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_pixel_size(layout, &w, NULL);
    return w;
}

/* The widest name shown (capped at about NAME_CHARS_CAP characters), and
 * what Size, Modified and Type need: worked out from their texts, so every
 * view of a folder comes to the same widths whatever it has shown before. */
static void measure_columns(SfePane* self)
{
    GtkTreeModel* model = sfe_dir_model_get_model(self->model);
    GtkTreeIter iter;
    PangoLayout* layout = gtk_widget_create_pango_layout(self->list, NULL);
    g_autoptr(GHashTable) types = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    int cap = text_width(layout, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMN");
    int widest = 0;

    for (bool ok = model && gtk_tree_model_get_iter_first(model, &iter); ok; ok = gtk_tree_model_iter_next(model, &iter)) {
        g_autofree char* name = NULL;
        g_autoptr(GFileInfo) info = NULL;
        gtk_tree_model_get(model, &iter, SFE_COL_NAME, &name, SFE_COL_INFO, &info, -1);
        if (name != NULL && widest < cap)
            widest = MAX(widest, text_width(layout, name));
        if (info != NULL)
            g_hash_table_add(types, sfe_dir_model_text(info, SFE_SORT_TYPE)); /* takes it */
    }
    self->name_width = MIN(widest, cap);

    /* A cell's text, or the header with its sort arrow, plus the paddings. */
    static const char* const samples[][3] = {
        [SFE_SORT_SIZE] = { "888.8 MB", "999 bytes", NULL },
        [SFE_SORT_MODIFIED] = { "8888-88-88 88:88", NULL, NULL },
    };
    for (int key = SFE_SORT_SIZE; key <= SFE_SORT_TYPE; key++) {
        int w = text_width(layout, gtk_tree_view_column_get_title(self->columns[key])) + COLUMN_ARROW;
        for (int i = 0; key != SFE_SORT_TYPE && i < 3 && samples[key][i] != NULL; i++)
            w = MAX(w, text_width(layout, samples[key][i]));
        if (key == SFE_SORT_TYPE) {
            GHashTableIter it;
            gpointer type;
            g_hash_table_iter_init(&it, types);
            while (g_hash_table_iter_next(&it, &type, NULL))
                w = MAX(w, text_width(layout, type));
        }
        self->column_width[key] = w + COLUMN_PADDING;
        /* The fixed-width columns get what was measured, so
         * fit_columns() decides with the widths they really have. */
        gtk_tree_view_column_set_fixed_width(self->columns[key], self->column_width[key]);
    }
    g_object_unref(layout);
}

/* Shows Size, Modified and Type only while the names still fit beside
 * them; they go from the right: Type, then Modified, then Size. */
static void fit_columns(SfePane* self)
{
    static const SfeSortKey extras[] = { SFE_SORT_SIZE, SFE_SORT_MODIFIED, SFE_SORT_TYPE };
    int width = gtk_widget_get_allocated_width(self->list);
    if (self->view != SFE_VIEW_LIST || width <= 1)
        return;
    if (self->name_width < 0)
        measure_columns(self);
    const SfeConfig* cfg = sfe_config();
    int icon = (int)lround(LIST_ICON_SIZE * cfg->ui_scale);
    int name_needed = self->name_width + icon + 32; /* icon, paddings, a little air */
    int keep = (int)G_N_ELEMENTS(extras);
    for (;;) {
        int used = name_needed;
        for (int i = 0; i < keep; i++)
            used += self->column_width[extras[i]];
        if (keep == 0 || used <= width)
            break;
        keep--;
    }
    bool changed = false;
    for (int i = 0; i < (int)G_N_ELEMENTS(extras); i++) {
        GtkTreeViewColumn* c = self->columns[extras[i]];
        if (gtk_tree_view_column_get_visible(c) != (i < keep)) {
            gtk_tree_view_column_set_visible(c, i < keep);
            changed = true;
        }
    }
    if (changed) /* the list keeps the old widths otherwise: the name stays squeezed */
        gtk_tree_view_columns_autosize(GTK_TREE_VIEW(self->list));
    /* With fixed widths GTK may hand the spare room to the last
     * column instead of Name, so Name gets it here. */
    int rest = width;
    for (int i = 0; i < keep; i++)
        rest -= self->column_width[extras[i]];
    gtk_tree_view_column_set_fixed_width(self->columns[SFE_SORT_NAME], MAX(rest, name_needed / 2));
    self->columns_for_width = width;
}

static gboolean on_fit_columns(gpointer data)
{
    SfePane* self = data;
    self->columns_source = 0;
    fit_columns(self);
    return G_SOURCE_REMOVE;
}

/* Not during the allocation itself: hiding a column relays the list out.
 * Ahead of GtkTreeView's row measuring, which in a big folder keeps the
 * default idle priority waiting for half a minute after a resize. */
static void schedule_fit_columns(SfePane* self)
{
    if (self->columns_source == 0)
        self->columns_source = g_idle_add_full(G_PRIORITY_HIGH_IDLE, on_fit_columns, self, NULL);
}

static void on_list_allocated(GtkWidget* list, GdkRectangle* alloc, gpointer data)
{
    (void)list;
    SfePane* self = data;
    if (alloc->width != self->columns_for_width)
        schedule_fit_columns(self);
}

/* Other names: measure them again. */
static void names_changed(SfePane* self)
{
    self->name_width = -1;
    schedule_fit_columns(self);
}

/* Whether the event being handled is the user's (scrolling, a key, the
 * mouse) rather than the window being resized or focused. */
static bool user_input(void)
{
    GdkEvent* event = gtk_get_current_event();
    GdkEventType type = event ? event->type : GDK_NOTHING;
    if (event != NULL)
        gdk_event_free(event);
    switch (type) {
    case GDK_SCROLL:
    case GDK_KEY_PRESS:
    case GDK_KEY_RELEASE:
    case GDK_BUTTON_PRESS:
    case GDK_2BUTTON_PRESS:
    case GDK_3BUTTON_PRESS:
    case GDK_BUTTON_RELEASE:
    case GDK_MOTION_NOTIFY:
    case GDK_TOUCH_BEGIN:
    case GDK_TOUCH_UPDATE:
    case GDK_TOUCH_END:
    case GDK_TOUCHPAD_SWIPE:
        return true;
    default:
        return false;
    }
}

/* Remembers where the user scrolled to. A view pulls itself back from the
 * empty space after the files when it re-measures (the window resized,
 * lost or got the focus: it keeps the last row in view); with no input
 * from the user that is undone. Not while a keyboard cursor move scrolls:
 * its animation frames come without input, and undoing one stopped gg a
 * few lines up (cursor_scrolls()). */
static void on_scroll_value(GtkAdjustment* adjustment, gpointer data)
{
    SfePane* self = data;
    if (self->padding)
        return;
    int which = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(adjustment), "sfe-view-index"));
    double value = gtk_adjustment_get_value(adjustment);
    double wanted = self->scroll_wanted[which];
    double page = gtk_adjustment_get_page_size(adjustment);
    bool was_in_space = self->content_end[which] > 0 && wanted > self->content_end[which] - page;
    bool cursor_moving = g_get_monotonic_time() < self->cursor_scroll_until;
    if (was_in_space && value < wanted && !user_input() && !cursor_moving) {
        self->padding = true;
        gtk_adjustment_set_value(adjustment, MIN(wanted, gtk_adjustment_get_upper(adjustment) - page));
        self->padding = false;
        return;
    }
    self->scroll_wanted[which] = value;
}

/* ------------------------------------------------------------------------ */
/* Drag and drop of files (text/uri-list, as other file managers use)        */
/* ------------------------------------------------------------------------ */

static void set_anchor(SfePane* self, GtkTreePath* path);

static const GtkTargetEntry uri_targets[] = { { (char*)"text/uri-list", 0, 0 } };

/* The row at bin window coordinates (NULL on the list's headers). */
static GtkTreePath* path_at(GtkWidget* view, GdkWindow* window, double x, double y)
{
    if (GTK_IS_ICON_VIEW(view))
        return gtk_icon_view_get_path_at_pos(GTK_ICON_VIEW(view), (int)x, (int)y);
    GtkTreePath* path = NULL;
    if (window == gtk_tree_view_get_bin_window(GTK_TREE_VIEW(view)))
        gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(view), (int)x, (int)y, &path, NULL, NULL, NULL);
    return path;
}

static bool path_selected(GtkWidget* view, GtkTreePath* path)
{
    if (GTK_IS_ICON_VIEW(view))
        return gtk_icon_view_path_is_selected(GTK_ICON_VIEW(view), path);
    return gtk_tree_selection_path_is_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(view)), path);
}

/* A press on a selected file may start dragging the whole selection, so
 * GTK must not see it (it would select just that file). Without a drag the
 * release selects that file; a double click opens it. True when taken. */
static bool drag_press(SfePane* self, GtkWidget* view, GdkEventButton* event)
{
    if (event->button != GDK_BUTTON_PRIMARY || self->single || (event->state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK)))
        return false;
    GtkTreePath* path = path_at(view, event->window, event->x, event->y);
    if (path == NULL || !path_selected(view, path)) {
        gtk_tree_path_free(path);
        return false;
    }
    g_clear_pointer(&self->drag_path, gtk_tree_path_free);
    if (event->type == GDK_2BUTTON_PRESS)
        activate_path(self, path);
    if (event->type == GDK_BUTTON_PRESS) {
        self->drag_path = gtk_tree_path_copy(path);
        self->drag_x = event->x;
        self->drag_y = event->y;
        gtk_widget_grab_focus(view);
    }
    gtk_tree_path_free(path);
    return true;
}

static void drag_release(SfePane* self, GtkWidget* view)
{
    GtkTreePath* path = g_steal_pointer(&self->drag_path);
    if (path == NULL)
        return;
    if (GTK_IS_ICON_VIEW(view)) {
        gtk_icon_view_unselect_all(GTK_ICON_VIEW(view));
        gtk_icon_view_select_path(GTK_ICON_VIEW(view), path);
        gtk_icon_view_set_cursor(GTK_ICON_VIEW(view), path, NULL, FALSE);
        set_anchor(self, path);
    } else {
        gtk_tree_view_set_cursor(GTK_TREE_VIEW(view), path, NULL, FALSE); /* (selects only it) */
    }
    gtk_tree_path_free(path);
}

static bool drag_motion(SfePane* self, GtkWidget* view, GdkEventMotion* event)
{
    if (self->drag_path == NULL)
        return false;
    if (!gtk_drag_check_threshold(view, (int)self->drag_x, (int)self->drag_y, (int)event->x, (int)event->y))
        return true;
    g_autoptr(GFileInfo) info = sfe_dir_model_info_at(self->model, self->drag_path);
    g_clear_pointer(&self->drag_path, gtk_tree_path_free);
    GtkTargetList* targets = gtk_target_list_new(uri_targets, G_N_ELEMENTS(uri_targets));
    GdkDragContext* ctx = gtk_drag_begin_with_coordinates(view, targets, GDK_ACTION_COPY | GDK_ACTION_MOVE,
        GDK_BUTTON_PRIMARY, (GdkEvent*)event, -1, -1);
    gtk_target_list_unref(targets);
    if (ctx != NULL && info != NULL) {
        cairo_surface_t* icon = sfe_dir_model_icon(self->model, info, false);
        if (icon != NULL) {
            gtk_drag_set_icon_surface(ctx, icon);
            cairo_surface_destroy(icon);
        }
    }
    return true;
}

static void on_drag_data_get(GtkWidget* view, GdkDragContext* ctx, GtkSelectionData* sel, guint info,
    guint time, gpointer data)
{
    (void)view, (void)ctx, (void)info, (void)time;
    GList* files = sfe_pane_get_selected_files(SFE_PANE(data));
    g_autoptr(GPtrArray) uris = g_ptr_array_new_with_free_func(g_free);
    for (GList* l = files; l != NULL; l = l->next)
        g_ptr_array_add(uris, g_file_get_uri(l->data));
    g_ptr_array_add(uris, NULL);
    gtk_selection_data_set_uris(sel, (char**)uris->pdata);
    g_list_free_full(files, g_object_unref);
}

static void drop_reset(SfePane* self)
{
    g_clear_object(&self->drop_ctx);
    g_list_free_full(g_steal_pointer(&self->drop_files), g_object_unref);
    self->drop_ready = self->drop_now = false;
}

/* Marks the folder a drop would go into: the folder under the pointer,
 * else the whole view (its folder). */
static void drop_highlight(GtkWidget* view, GtkTreePath* folder)
{
    if (GTK_IS_ICON_VIEW(view))
        gtk_icon_view_set_drag_dest_item(GTK_ICON_VIEW(view), folder, GTK_ICON_VIEW_DROP_INTO);
    else
        gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW(view), folder, GTK_TREE_VIEW_DROP_INTO_OR_BEFORE);
    if (folder != NULL)
        gtk_drag_unhighlight(view);
    else
        gtk_drag_highlight(view);
}

static void drop_unhighlight(GtkWidget* view)
{
    if (GTK_IS_ICON_VIEW(view))
        gtk_icon_view_set_drag_dest_item(GTK_ICON_VIEW(view), NULL, GTK_ICON_VIEW_NO_DROP);
    else
        gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW(view), NULL, GTK_TREE_VIEW_DROP_BEFORE);
    gtk_drag_unhighlight(view);
}

/* Where a drop at widget coordinates goes (new reference). */
static GFile* drop_folder(SfePane* self, GtkWidget* view, int x, int y)
{
    int bx = 0, by = 0;
    GtkTreePath* path = NULL;
    if (GTK_IS_ICON_VIEW(view)) {
        gtk_icon_view_convert_widget_to_bin_window_coords(GTK_ICON_VIEW(view), x, y, &bx, &by);
        path = gtk_icon_view_get_path_at_pos(GTK_ICON_VIEW(view), bx, by);
    } else {
        gtk_tree_view_convert_widget_to_bin_window_coords(GTK_TREE_VIEW(view), x, y, &bx, &by);
        gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(view), bx, by, &path, NULL, NULL, NULL);
    }
    GFile* dest = NULL;
    GtkTreeModel* model = sfe_dir_model_get_model(self->model);
    GtkTreeIter iter;
    gboolean is_dir = FALSE;
    if (path != NULL && model != NULL && gtk_tree_model_get_iter(model, &iter, path))
        gtk_tree_model_get(model, &iter, SFE_COL_IS_DIR, &is_dir, -1);
    if (is_dir)
        dest = sfe_dir_model_file_at(self->model, path);
    if (dest == NULL)
        g_clear_pointer(&path, gtk_tree_path_free);
    drop_highlight(view, path);
    gtk_tree_path_free(path);
    return dest != NULL ? dest : g_object_ref(self->dir);
}

static bool same_filesystem(GFile* a, GFile* b)
{
    g_autoptr(GFileInfo) ia = g_file_query_info(a, G_FILE_ATTRIBUTE_ID_FILESYSTEM, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
    g_autoptr(GFileInfo) ib = g_file_query_info(b, G_FILE_ATTRIBUTE_ID_FILESYSTEM, 0, NULL, NULL);
    const char* fa = ia ? g_file_info_get_attribute_string(ia, G_FILE_ATTRIBUTE_ID_FILESYSTEM) : NULL;
    const char* fb = ib ? g_file_info_get_attribute_string(ib, G_FILE_ATTRIBUTE_ID_FILESYSTEM) : NULL;
    return fa != NULL && g_strcmp0(fa, fb) == 0;
}

/* Copy or move, as other file managers do: Ctrl copies, Shift moves, else a
 * move on the same file system and a copy to another. 0 refuses: a folder
 * into itself, a move to where the files already are, nothing usable. */
static GdkDragAction drop_action(SfePane* self, GtkWidget* view, GdkDragContext* ctx, GFile* dest)
{
    if (self->drop_files == NULL)
        return 0;
    bool all_here = true;
    for (GList* l = self->drop_files; l != NULL; l = l->next) {
        g_autoptr(GFile) parent = g_file_get_parent(l->data);
        if (g_file_equal(l->data, dest) || g_file_has_prefix(dest, l->data))
            return 0;
        all_here = all_here && parent != NULL && g_file_equal(parent, dest);
    }
    GdkModifierType mask = 0;
    gdk_window_get_device_position(gtk_widget_get_window(view), gdk_drag_context_get_device(ctx), NULL, NULL, &mask);
    GdkDragAction action;
    if (mask & GDK_CONTROL_MASK)
        action = GDK_ACTION_COPY;
    else if (mask & GDK_SHIFT_MASK)
        action = GDK_ACTION_MOVE;
    else
        action = same_filesystem(self->drop_files->data, dest) ? GDK_ACTION_MOVE : GDK_ACTION_COPY;
    if (!(gdk_drag_context_get_actions(ctx) & action))
        action = gdk_drag_context_get_suggested_action(ctx) & (GDK_ACTION_COPY | GDK_ACTION_MOVE);
    return action == GDK_ACTION_MOVE && all_here ? 0 : action;
}

static void drop_status(SfePane* self, GtkWidget* view, GdkDragContext* ctx, guint time)
{
    g_autoptr(GFile) dest = drop_folder(self, view, self->drop_x, self->drop_y);
    GdkDragAction action = self->drop_ready ? drop_action(self, view, ctx, dest) : 0;
    if (action == 0)
        drop_unhighlight(view);
    gdk_drag_status(ctx, action, time);
}

static void drop_finish(SfePane* self, GtkWidget* view, GdkDragContext* ctx, guint time)
{
    g_autoptr(GFile) dest = drop_folder(self, view, self->drop_x, self->drop_y);
    GdkDragAction action = drop_action(self, view, ctx, dest);
    GtkWidget* window = gtk_widget_get_toplevel(GTK_WIDGET(self));
    if (action != 0 && SFE_IS_WINDOW(window))
        sfe_actions_transfer(SFE_WINDOW(window), self->drop_files, dest, action == GDK_ACTION_MOVE);
    drop_unhighlight(view);
    gtk_drag_finish(ctx, action != 0 && SFE_IS_WINDOW(window), FALSE, time);
    drop_reset(self);
}

/* The files are fetched on the first motion, to choose copy or move. */
static gboolean on_drag_motion(GtkWidget* view, GdkDragContext* ctx, int x, int y, guint time, gpointer data)
{
    SfePane* self = data;
    if (gtk_drag_dest_find_target(view, ctx, NULL) == GDK_NONE)
        return FALSE;
    self->drop_x = x;
    self->drop_y = y;
    if (ctx != self->drop_ctx) {
        drop_reset(self);
        self->drop_ctx = g_object_ref(ctx);
        gtk_drag_get_data(view, ctx, gdk_atom_intern_static_string("text/uri-list"), time);
    }
    drop_status(self, view, ctx, time);
    return TRUE;
}

/* (Also before a drop.) */
static void on_drag_leave(GtkWidget* view, GdkDragContext* ctx, guint time, gpointer data)
{
    (void)ctx, (void)time, (void)data;
    drop_unhighlight(view);
}

static gboolean on_drag_drop(GtkWidget* view, GdkDragContext* ctx, int x, int y, guint time, gpointer data)
{
    SfePane* self = data;
    if (gtk_drag_dest_find_target(view, ctx, NULL) == GDK_NONE)
        return FALSE;
    if (ctx != self->drop_ctx) {
        drop_reset(self);
        self->drop_ctx = g_object_ref(ctx);
    }
    self->drop_x = x;
    self->drop_y = y;
    self->drop_now = true;
    if (self->drop_ready)
        drop_finish(self, view, ctx, time);
    else
        gtk_drag_get_data(view, ctx, gdk_atom_intern_static_string("text/uri-list"), time);
    return TRUE;
}

/* Only local files and the trash (to restore from it). */
static void on_drag_data_received(GtkWidget* view, GdkDragContext* ctx, int x, int y, GtkSelectionData* sel,
    guint info, guint time, gpointer data)
{
    (void)x, (void)y, (void)info;
    SfePane* self = data;
    if (ctx != self->drop_ctx)
        return;
    if (!self->drop_ready) {
        g_auto(GStrv) uris = gtk_selection_data_get_uris(sel);
        bool usable = uris != NULL && uris[0] != NULL;
        GList* files = NULL;
        for (char** u = uris; usable && *u != NULL; u++) {
            GFile* file = g_file_new_for_uri(*u);
            usable = g_file_is_native(file) || g_file_has_uri_scheme(file, "trash");
            files = g_list_prepend(files, file);
        }
        if (usable)
            self->drop_files = g_list_reverse(files);
        else
            g_list_free_full(files, g_object_unref);
        self->drop_ready = true;
    }
    if (self->drop_now)
        drop_finish(self, view, ctx, time);
    else
        drop_status(self, view, ctx, time);
}

static void setup_dnd(SfePane* self, GtkWidget* view)
{
    gtk_drag_dest_set(view, 0, uri_targets, G_N_ELEMENTS(uri_targets), GDK_ACTION_COPY | GDK_ACTION_MOVE);
    g_signal_connect(view, "drag-data-get", G_CALLBACK(on_drag_data_get), self);
    g_signal_connect(view, "drag-motion", G_CALLBACK(on_drag_motion), self);
    g_signal_connect(view, "drag-leave", G_CALLBACK(on_drag_leave), self);
    g_signal_connect(view, "drag-drop", G_CALLBACK(on_drag_drop), self);
    g_signal_connect(view, "drag-data-received", G_CALLBACK(on_drag_data_received), self);
}

/* ------------------------------------------------------------------------ */
/* The drag-select rectangle in the empty space                              */
/* ------------------------------------------------------------------------ */

/* Event coordinates (relative to the view's bin window, also while the
 * pointer is outside it) to content coordinates. */
/* An event on the grid's outer window (below the items) in the inner
 * window's coordinates, which the rest of the grid code uses. */
static void grid_event_to_bin(GtkWidget* view, GdkWindow* window, double* x, double* y)
{
    if (!GTK_IS_ICON_VIEW(view) || window != gtk_widget_get_window(view))
        return;
    *x += gtk_adjustment_get_value(gtk_scrollable_get_hadjustment(GTK_SCROLLABLE(view)));
    *y += gtk_adjustment_get_value(gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(view)));
}

static void event_to_content(GtkWidget* view, double x, double y, double* cx, double* cy)
{
    if (GTK_IS_ICON_VIEW(view)) { /* the bin window is the content */
        *cx = x;
        *cy = y;
        return;
    }
    int tx = 0, ty = 0;
    gtk_tree_view_convert_bin_window_to_tree_coords(GTK_TREE_VIEW(view), (int)x, (int)y, &tx, &ty);
    *cx = tx;
    *cy = ty;
}

static void band_press(SfePane* self, GtkWidget* view, GdkEventButton* event)
{
    if (event->button != GDK_BUTTON_PRIMARY || event->type != GDK_BUTTON_PRESS || self->single)
        return;
    self->band_pressed = true;
    self->band_active = false;
    self->band_own = false;
    g_clear_pointer(&self->band_base, g_hash_table_unref);
    event_to_content(view, event->x, event->y, &self->band_x1, &self->band_y1);
    self->band_x2 = self->band_x1;
    self->band_y2 = self->band_y1;
    if (!GTK_IS_ICON_VIEW(view) || (event->state & GDK_SHIFT_MASK))
        return; /* (Shift+click selects a run instead) */

    /* GTK bands only from empty space on its inner window; on a file (the
     * views do not drag files) or below the items, sfe does. */
    GtkIconView* grid = GTK_ICON_VIEW(view);
    GtkTreePath* path = gtk_icon_view_get_path_at_pos(grid, (int)event->x, (int)event->y);
    bool below = event->window == gtk_widget_get_window(view);
    self->band_own = path != NULL || below;
    if (self->band_own && (event->state & GDK_CONTROL_MASK)) {
        self->band_base = g_hash_table_new(NULL, NULL);
        GList* sel = gtk_icon_view_get_selected_items(grid);
        for (GList* l = sel; l != NULL; l = l->next)
            g_hash_table_add(self->band_base, GINT_TO_POINTER(gtk_tree_path_get_indices(l->data)[0] + 1));
        g_list_free_full(sel, (GDestroyNotify)gtk_tree_path_free);
    } else if (below) {
        gtk_icon_view_unselect_all(grid); /* a click in empty space, as GTK does above */
    }
    gtk_tree_path_free(path);
}

/* The band's items: the visible ones it touches (and the earlier selection
 * with Ctrl). Items scrolled away keep what they had. */
static void band_select(SfePane* self)
{
    GtkIconView* grid = GTK_ICON_VIEW(self->grid);
    GtkTreePath *start = NULL, *end = NULL;
    if (!gtk_icon_view_get_visible_range(grid, &start, &end))
        return;
    double vx = gtk_adjustment_get_value(gtk_scrollable_get_hadjustment(GTK_SCROLLABLE(grid)));
    double vy = gtk_adjustment_get_value(gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(grid)));
    double x1 = MIN(self->band_x1, self->band_x2), x2 = MAX(self->band_x1, self->band_x2);
    double y1 = MIN(self->band_y1, self->band_y2), y2 = MAX(self->band_y1, self->band_y2);
    int last = gtk_tree_path_get_indices(end)[0];
    for (int i = gtk_tree_path_get_indices(start)[0]; i <= last; i++) {
        GtkTreePath* path = gtk_tree_path_new_from_indices(i, -1);
        GdkRectangle r; /* in widget coordinates */
        if (gtk_icon_view_get_cell_rect(grid, path, NULL, &r)) {
            bool hit = r.x + vx < x2 && r.x + vx + r.width > x1 && r.y + vy < y2 && r.y + vy + r.height > y1;
            if (hit || (self->band_base && g_hash_table_contains(self->band_base, GINT_TO_POINTER(i + 1))))
                gtk_icon_view_select_path(grid, path);
            else
                gtk_icon_view_unselect_path(grid, path);
        }
        gtk_tree_path_free(path);
    }
    gtk_tree_path_free(start);
    gtk_tree_path_free(end);
}

static gboolean on_view_motion(GtkWidget* view, GdkEventMotion* event, gpointer data)
{
    SfePane* self = data;
    if (!(event->state & GDK_BUTTON1_MASK))
        return FALSE;
    grid_event_to_bin(view, event->window, &event->x, &event->y);
    if (drag_motion(self, view, event))
        return TRUE;
    if (!self->band_pressed)
        return FALSE;
    event_to_content(view, event->x, event->y, &self->band_x2, &self->band_y2);
    if (self->band_own) {
        if (!self->band_active && !gtk_drag_check_threshold(view, (int)self->band_x1, (int)self->band_y1,
                (int)self->band_x2, (int)self->band_y2))
            return FALSE;
        self->band_active = true;
        band_select(self);
        gtk_widget_queue_draw(view);
        return TRUE;
    }
    self->band_active = GTK_IS_ICON_VIEW(view) || gtk_tree_view_is_rubber_banding_active(GTK_TREE_VIEW(view));
    if (self->band_active)
        gtk_widget_queue_draw(view);
    return FALSE;
}

static gboolean on_view_release(GtkWidget* view, GdkEventButton* event, gpointer data)
{
    (void)event;
    SfePane* self = data;
    drag_release(self, view);
    if (self->band_active)
        gtk_widget_queue_draw(view);
    self->band_pressed = self->band_active = self->band_own = false;
    g_clear_pointer(&self->band_base, g_hash_table_unref);
    return FALSE;
}

/* After the view drew itself: the part of the band below the last row. */
static gboolean on_view_draw_after(GtkWidget* view, cairo_t* cr, gpointer data)
{
    SfePane* self = data;
    int which = view == self->grid ? 0 : 1;
    if (!self->band_active || view != active_view(self) || !gtk_cairo_should_draw_window(cr, gtk_widget_get_window(view)))
        return FALSE;
    double x1 = MIN(self->band_x1, self->band_x2), x2 = MAX(self->band_x1, self->band_x2);
    double y1 = MIN(self->band_y1, self->band_y2), y2 = MAX(self->band_y1, self->band_y2);
    double end = self->band_own ? -G_MAXDOUBLE : self->content_end[which];
    if (y2 <= end)
        return FALSE; /* all over the files: GTK drew it */

    /* content -> widget coordinates */
    double dx = 0, dy = 0;
    if (GTK_IS_ICON_VIEW(view)) {
        dx = -gtk_adjustment_get_value(gtk_scrollable_get_hadjustment(GTK_SCROLLABLE(view)));
        dy = -gtk_adjustment_get_value(gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(view)));
    } else {
        int wx = 0, wy = 0;
        gtk_tree_view_convert_tree_to_widget_coords(GTK_TREE_VIEW(view), 0, 0, &wx, &wy);
        dx = wx;
        dy = wy;
    }
    cairo_save(cr);
    if (!self->band_own) {
        cairo_rectangle(cr, 0, end + dy, gtk_widget_get_allocated_width(view), gtk_widget_get_allocated_height(view));
        cairo_clip(cr); /* only where GTK could not draw */
    }
    GtkStyleContext* style = gtk_widget_get_style_context(view);
    gtk_style_context_save(style);
    gtk_style_context_add_class(style, GTK_STYLE_CLASS_RUBBERBAND);
    gtk_render_background(style, cr, x1 + dx, y1 + dy, x2 - x1, y2 - y1);
    gtk_render_frame(style, cr, x1 + dx, y1 + dy, x2 - x1, y2 - y1);
    gtk_style_context_restore(style);
    cairo_restore(cr);
    return FALSE;
}

/* Another folder: it starts at the top, not where the last one was. */
static void forget_scroll(SfePane* self)
{
    self->scroll_wanted[0] = self->scroll_wanted[1] = 0;
    self->content_end[0] = self->content_end[1] = 0;
}

static void on_files_changed(SfeDirModel* model, gpointer data)
{
    (void)model;
    schedule_thumbnails(data);
    names_changed(data);
    update_filter_badge(data);
}

static void apply_config(SfePane* self)
{
    const SfeConfig* cfg = sfe_config();

    if ((int)cfg->thumbnails != self->cfg_thumbnails) {
        if (self->cfg_thumbnails >= 0 && !cfg->thumbnails)
            sfe_dir_model_clear_thumbnails(self->model); /* icons again */
        self->cfg_thumbnails = cfg->thumbnails;
        schedule_thumbnails(self);
    }
    if ((int)cfg->folder_covers != self->cfg_folder_covers) {
        if (self->cfg_folder_covers >= 0)
            sfe_dir_model_clear_thumbnails(self->model); /* folders asked again (files come from the cache) */
        self->cfg_folder_covers = cfg->folder_covers;
        schedule_thumbnails(self);
    }

    sfe_dir_model_set_show_hidden(self->model, cfg->show_hidden);
    if (cfg->sort_by != self->cfg_sort_by || cfg->sort_order != self->cfg_sort_order
        || cfg->dirs_first != self->cfg_dirs_first) {
        self->cfg_sort_by = cfg->sort_by;
        self->cfg_sort_order = cfg->sort_order;
        self->cfg_dirs_first = cfg->dirs_first;
        sfe_dir_model_set_sort(self->model, cfg->sort_by,
            cfg->sort_order == SFE_SORT_DESCENDING ? GTK_SORT_DESCENDING : GTK_SORT_ASCENDING, cfg->dirs_first);
        update_sort_indicators(self);
    }
    apply_icon_sizes(self);
}

static void on_config_changed(SfeApplication* app, gpointer data)
{
    (void)app;
    apply_config(SFE_PANE(data));
}

static void on_scale_factor_changed(GObject* object, GParamSpec* pspec, gpointer data)
{
    (void)object, (void)pspec;
    apply_icon_sizes(SFE_PANE(data));
}

/* ------------------------------------------------------------------------ */
/* Views                                                                     */
/* ------------------------------------------------------------------------ */

void sfe_pane_set_view(SfePane* self, SfeViewMode mode)
{
    GList* selected = sfe_pane_get_selected_files(self);
    GtkWidget* old = active_view(self);
    bool had_focus = gtk_widget_has_focus(old);

    sfe_pane_visual_end(self);
    self->view = mode;
    attach_model(self);
    if (gtk_stack_get_visible_child(GTK_STACK(self->stack)) != self->message_box)
        gtk_stack_set_visible_child(GTK_STACK(self->stack), gtk_widget_get_parent(active_view(self)));
    update_buttons(self);
    schedule_fit_columns(self);
    schedule_thumbnails(self);

    /* Keep the first selected file selected across the switch. */
    if (selected != NULL) {
        g_autofree char* name = g_file_get_basename(selected->data);
        GtkTreePath* path = sfe_dir_model_find(self->model, name);
        if (path != NULL) {
            select_path(self, path);
            gtk_tree_path_free(path);
        }
    }
    g_list_free_full(selected, g_object_unref);
    if (had_focus)
        gtk_widget_grab_focus(active_view(self));
}

GList* sfe_pane_get_all_files(SfePane* self)
{
    GtkTreeModel* model = sfe_dir_model_get_model(self->model);
    GtkTreeIter iter;
    GList* files = NULL;

    if (model == NULL || !gtk_tree_model_get_iter_first(model, &iter))
        return NULL;
    do {
        GFile* file = NULL;
        gtk_tree_model_get(model, &iter, SFE_COL_FILE, &file, -1);
        if (file != NULL)
            files = g_list_prepend(files, file);
    } while (gtk_tree_model_iter_next(model, &iter));
    return g_list_reverse(files);
}

GList* sfe_pane_get_selected_files(SfePane* self)
{
    GList* paths = NULL;
    GList* files = NULL;

    if (self->view == SFE_VIEW_GRID)
        paths = gtk_icon_view_get_selected_items(GTK_ICON_VIEW(self->grid));
    else
        paths = gtk_tree_selection_get_selected_rows(gtk_tree_view_get_selection(GTK_TREE_VIEW(self->list)), NULL);

    /* In the order shown, so numbering (mass rename) follows the view. */
    paths = g_list_sort(paths, (GCompareFunc)gtk_tree_path_compare);
    for (GList* l = paths; l != NULL; l = l->next) {
        GFile* file = sfe_dir_model_file_at(self->model, l->data);
        if (file != NULL)
            files = g_list_prepend(files, file);
    }
    g_list_free_full(paths, (GDestroyNotify)gtk_tree_path_free);
    return g_list_reverse(files);
}

void sfe_pane_set_file_filter(SfePane* self, const SfeFileFilter* filter)
{
    sfe_dir_model_set_filter(self->model, filter);
}

void sfe_pane_set_single_selection(SfePane* self, bool single)
{
    if (self->single == single)
        return;
    self->single = single;
    if (single) {
        sfe_pane_visual_end(self);
        forget_anchor(self);
    }
    /* SINGLE, not BROWSE: Ctrl+click can still clear the selection */
    GtkSelectionMode mode = single ? GTK_SELECTION_SINGLE : GTK_SELECTION_MULTIPLE;
    gtk_icon_view_set_selection_mode(GTK_ICON_VIEW(self->grid), mode);
    gtk_tree_view_set_rubber_banding(GTK_TREE_VIEW(self->list), !single);
    gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(self->list)), mode);
}

bool sfe_pane_get_single_selection(SfePane* self)
{
    return self->single;
}

void sfe_pane_select_all(SfePane* self)
{
    if (self->single)
        return;
    if (self->view == SFE_VIEW_GRID)
        gtk_icon_view_select_all(GTK_ICON_VIEW(self->grid));
    else
        gtk_tree_selection_select_all(gtk_tree_view_get_selection(GTK_TREE_VIEW(self->list)));
}

static void entry_free(gpointer p)
{
    SfePaneEntry* e = p;
    g_free(e->name);
    g_clear_object(&e->file);
    g_free(e);
}

GPtrArray* sfe_pane_get_entries(SfePane* self)
{
    GPtrArray* entries = g_ptr_array_new_with_free_func(entry_free);
    GtkTreeModel* model = sfe_dir_model_get_model(self->model);
    GtkTreeIter iter;

    if (model == NULL || !gtk_tree_model_get_iter_first(model, &iter))
        return entries;
    do {
        SfePaneEntry* e = g_new0(SfePaneEntry, 1);
        gboolean is_dir = FALSE;
        gtk_tree_model_get(model, &iter, SFE_COL_FILE, &e->file, SFE_COL_NAME, &e->name, SFE_COL_IS_DIR, &is_dir, -1);
        e->is_dir = is_dir;
        if (e->file != NULL && e->name != NULL)
            g_ptr_array_add(entries, e);
        else
            entry_free(e);
    } while (gtk_tree_model_iter_next(model, &iter));
    return entries;
}

static GList* selected_paths(SfePane* self)
{
    if (self->view == SFE_VIEW_GRID)
        return gtk_icon_view_get_selected_items(GTK_ICON_VIEW(self->grid));
    return gtk_tree_selection_get_selected_rows(gtk_tree_view_get_selection(GTK_TREE_VIEW(self->list)), NULL);
}

static void select_paths(SfePane* self, GList* paths)
{
    if (self->view == SFE_VIEW_GRID) {
        gtk_icon_view_unselect_all(GTK_ICON_VIEW(self->grid));
        for (GList* l = paths; l != NULL; l = l->next)
            gtk_icon_view_select_path(GTK_ICON_VIEW(self->grid), l->data);
    } else {
        GtkTreeSelection* selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(self->list));
        gtk_tree_selection_unselect_all(selection);
        for (GList* l = paths; l != NULL; l = l->next)
            gtk_tree_selection_select_path(selection, l->data);
    }
}

/* The views' own keyboard movement ("move-cursor"; it needs the focus).
 * `modify` moves the cursor without selecting, like Ctrl+arrow.
 *
 * GTK 3's signal is (step, count) -> handled; some versions add (extend,
 * modify). A move with the plain signal selects the row it lands on, so for
 * `modify` the selection is put back afterwards. */
static void move_cursor(SfePane* self, GtkMovementStep step, int count, bool modify)
{
    GtkWidget* view = active_view(self);
    gboolean handled = FALSE;
    GSignalQuery query;
    g_signal_query(g_signal_lookup("move-cursor", G_OBJECT_TYPE(view)), &query);
    GList* keep = modify ? selected_paths(self) : NULL;

    cursor_scrolls(self);
    gtk_widget_grab_focus(view);
    if (query.n_params >= 4)
        g_signal_emit_by_name(view, "move-cursor", step, count, FALSE, modify, &handled);
    else
        g_signal_emit_by_name(view, "move-cursor", step, count, &handled);
    if (modify)
        select_paths(self, keep);
    g_list_free_full(keep, (GDestroyNotify)gtk_tree_path_free);
}

/* The cursor to row `index` alone, selecting only it like j / k do (or, in
 * visual mode, keeping the selection). Not through "move-cursor": the views
 * read Shift from the current event, so "G" would select every row on the
 * way. */
static void jump_cursor(SfePane* self, int index, bool visual)
{
    if (index < 0)
        return;
    GtkTreePath* path = gtk_tree_path_new_from_indices(index, -1);
    GList* keep = visual ? selected_paths(self) : NULL;
    cursor_scrolls(self);
    gtk_widget_grab_focus(active_view(self));
    if (self->view == SFE_VIEW_GRID) {
        GtkIconView* grid = GTK_ICON_VIEW(self->grid);
        gtk_icon_view_set_cursor(grid, path, NULL, FALSE);
        if (!visual) {
            gtk_icon_view_unselect_all(grid);
            gtk_icon_view_select_path(grid, path);
        }
        gtk_icon_view_scroll_to_path(grid, path, FALSE, 0, 0);
    } else {
        gtk_tree_view_set_cursor(GTK_TREE_VIEW(self->list), path, NULL, FALSE); /* selects only this row */
    }
    if (visual)
        select_paths(self, keep);
    g_list_free_full(keep, (GDestroyNotify)gtk_tree_path_free);
    gtk_tree_path_free(path);
}

static void apply_visual(SfePane* self);

void sfe_pane_move_cursor(SfePane* self, SfeCursorMove move)
{
    /* In visual mode the cursor moves alone; the selection follows it. */
    bool visual = self->visual;
    if (!visual)
        forget_anchor(self); /* a Shift-selection starts from the new cursor */
    switch (move) {
    case SFE_CURSOR_DOWN: move_cursor(self, GTK_MOVEMENT_DISPLAY_LINES, 1, visual); break;
    case SFE_CURSOR_UP: move_cursor(self, GTK_MOVEMENT_DISPLAY_LINES, -1, visual); break;
    case SFE_CURSOR_FIRST: jump_cursor(self, n_rows(self) > 0 ? 0 : -1, visual); break;
    case SFE_CURSOR_LAST: jump_cursor(self, n_rows(self) - 1, visual); break;
    case SFE_CURSOR_PREVIOUS:
    case SFE_CURSOR_NEXT:
        move_cursor(self, self->view == SFE_VIEW_GRID ? GTK_MOVEMENT_VISUAL_POSITIONS : GTK_MOVEMENT_DISPLAY_LINES,
            move == SFE_CURSOR_NEXT ? 1 : -1, visual);
        break;
    }
    if (visual)
        apply_visual(self);
}

void sfe_pane_toggle_cursor_item(SfePane* self)
{
    GtkTreePath* path = highlighted(self);
    if (path == NULL)
        return;
    if (self->visual) { /* in or out of the drawn chunks */
        GFile* file = sfe_dir_model_file_at(self->model, path);
        if (file != NULL && !g_hash_table_remove(self->visual_base, file))
            g_hash_table_add(self->visual_base, g_object_ref(file));
        g_clear_object(&file);
        gtk_tree_path_free(path);
        move_cursor(self, self->view == SFE_VIEW_GRID ? GTK_MOVEMENT_VISUAL_POSITIONS : GTK_MOVEMENT_DISPLAY_LINES, 1,
            true);
        apply_visual(self);
        return;
    }
    if (self->view == SFE_VIEW_GRID) {
        GtkIconView* grid = GTK_ICON_VIEW(self->grid);
        if (gtk_icon_view_path_is_selected(grid, path))
            gtk_icon_view_unselect_path(grid, path);
        else
            gtk_icon_view_select_path(grid, path);
        gtk_icon_view_set_cursor(grid, path, NULL, FALSE);
        move_cursor(self, GTK_MOVEMENT_VISUAL_POSITIONS, 1, true); /* the next item, not the next row */
    } else {
        GtkTreeSelection* selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(self->list));
        GtkTreePath* cursor = NULL;
        gtk_tree_view_get_cursor(GTK_TREE_VIEW(self->list), &cursor, NULL);
        if (cursor == NULL) { /* no cursor yet: a move would only put it on row 0 */
            GList* keep = selected_paths(self);
            gtk_tree_view_set_cursor(GTK_TREE_VIEW(self->list), path, NULL, FALSE);
            select_paths(self, keep);
            g_list_free_full(keep, (GDestroyNotify)gtk_tree_path_free);
        }
        gtk_tree_path_free(cursor);
        if (gtk_tree_selection_path_is_selected(selection, path))
            gtk_tree_selection_unselect_path(selection, path);
        else
            gtk_tree_selection_select_path(selection, path);
        move_cursor(self, GTK_MOVEMENT_DISPLAY_LINES, 1, true);
    }
    gtk_tree_path_free(path);
}

/* ------------------------------------------------------------------------ */
/* Visual mode                                                               */
/* ------------------------------------------------------------------------ */

bool sfe_pane_in_visual(SfePane* self)
{
    return self->visual;
}

bool sfe_pane_visual_drawing(SfePane* self)
{
    return self->visual && self->visual_anchor != NULL;
}

/* The anchor's row index, or -1 when no chunk is being drawn. */
static int anchor_index(SfePane* self)
{
    if (self->visual_anchor == NULL)
        return -1;
    GtkTreePath* path = gtk_tree_row_reference_get_path(self->visual_anchor);
    if (path == NULL /* the anchor's file went away */
        || gtk_tree_row_reference_get_model(self->visual_anchor) != sfe_dir_model_get_model(self->model)) {
        gtk_tree_path_free(path);
        return -1;
    }
    int index = gtk_tree_path_get_indices(path)[0];
    gtk_tree_path_free(path);
    return index;
}

static void set_row_selected(SfePane* self, GtkTreePath* path, bool selected)
{
    if (self->view == SFE_VIEW_GRID) {
        GtkIconView* grid = GTK_ICON_VIEW(self->grid);
        if (gtk_icon_view_path_is_selected(grid, path) != selected)
            (selected ? gtk_icon_view_select_path : gtk_icon_view_unselect_path)(grid, path);
    } else {
        GtkTreeSelection* selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(self->list));
        if (gtk_tree_selection_path_is_selected(selection, path) != selected)
            (selected ? gtk_tree_selection_select_path : gtk_tree_selection_unselect_path)(selection, path);
    }
}

/* Selects exactly the drawn chunks plus the rows between the anchor and the
 * cursor. */
static void apply_visual(SfePane* self)
{
    GtkTreeModel* model = sfe_dir_model_get_model(self->model);
    GtkTreeIter iter;
    int lo = anchor_index(self), hi = lo;
    GtkTreePath* cursor = highlighted(self);

    if (lo < 0) {
        g_clear_pointer(&self->visual_anchor, gtk_tree_row_reference_free);
    forget_anchor(self);
    } else if (cursor != NULL) {
        int c = gtk_tree_path_get_indices(cursor)[0];
        lo = MIN(lo, c);
        hi = MAX(hi, c);
    }
    if (model != NULL && gtk_tree_model_get_iter_first(model, &iter)) {
        int i = 0;
        do {
            g_autoptr(GFile) file = NULL;
            gtk_tree_model_get(model, &iter, SFE_COL_FILE, &file, -1);
            bool selected = (i >= lo && i <= hi && lo >= 0)
                || (file != NULL && g_hash_table_contains(self->visual_base, file));
            GtkTreePath* path = gtk_tree_path_new_from_indices(i, -1);
            set_row_selected(self, path, selected);
            gtk_tree_path_free(path);
            i++;
        } while (gtk_tree_model_iter_next(model, &iter));
    }
    gtk_tree_path_free(cursor);
    g_signal_emit(self, signals[SIGNAL_VISUAL_CHANGED], 0);
}

/* A new chunk starts at the cursor. */
static void start_chunk(SfePane* self)
{
    GtkTreePath* cursor = highlighted(self);
    GtkTreeModel* model = sfe_dir_model_get_model(self->model);
    if (cursor == NULL || model == NULL) {
        gtk_tree_path_free(cursor);
        return;
    }
    g_clear_pointer(&self->visual_anchor, gtk_tree_row_reference_free);
    self->visual_anchor = gtk_tree_row_reference_new(model, cursor);
    /* make it the real cursor too (row 0 may only be the fallback) */
    if (self->view == SFE_VIEW_GRID)
        gtk_icon_view_set_cursor(GTK_ICON_VIEW(self->grid), cursor, NULL, FALSE);
    else
        gtk_tree_view_set_cursor(GTK_TREE_VIEW(self->list), cursor, NULL, FALSE);
    gtk_tree_path_free(cursor);
}

/* The chunk being drawn joins the finished ones. */
static void finish_chunk(SfePane* self)
{
    GList* files = sfe_pane_get_selected_files(self);
    for (GList* l = files; l != NULL; l = l->next)
        g_hash_table_add(self->visual_base, l->data); /* takes the reference */
    g_list_free(files);
    g_clear_pointer(&self->visual_anchor, gtk_tree_row_reference_free);
}

static void visual_start(SfePane* self)
{
    if (self->visual || self->single || n_rows(self) == 0)
        return;
    self->visual = true;
    g_hash_table_remove_all(self->visual_base);
    gtk_widget_grab_focus(active_view(self));
    start_chunk(self);
    apply_visual(self);
}

void sfe_pane_visual_chunk(SfePane* self)
{
    if (self->single)
        return;
    if (!self->visual) {
        visual_start(self);
        return;
    }
    if (self->visual_anchor != NULL && anchor_index(self) >= 0)
        finish_chunk(self);
    else
        start_chunk(self);
    apply_visual(self);
}

void sfe_pane_visual_swap(SfePane* self)
{
    int anchor = anchor_index(self);
    GtkTreePath* cursor = highlighted(self);
    if (anchor >= 0 && cursor != NULL) {
        GtkTreePath* path = gtk_tree_path_new_from_indices(anchor, -1);
        g_clear_pointer(&self->visual_anchor, gtk_tree_row_reference_free);
        self->visual_anchor = gtk_tree_row_reference_new(sfe_dir_model_get_model(self->model), cursor);
        if (self->view == SFE_VIEW_GRID) {
            gtk_icon_view_set_cursor(GTK_ICON_VIEW(self->grid), path, NULL, FALSE);
            gtk_icon_view_scroll_to_path(GTK_ICON_VIEW(self->grid), path, FALSE, 0, 0);
        } else {
            /* set_cursor would select only this row; the selection is redone below */
            gtk_tree_view_set_cursor(GTK_TREE_VIEW(self->list), path, NULL, FALSE);
        }
        gtk_tree_path_free(path);
        apply_visual(self);
    }
    gtk_tree_path_free(cursor);
}

void sfe_pane_visual_end(SfePane* self)
{
    if (!self->visual)
        return;
    self->visual = false;
    g_clear_pointer(&self->visual_anchor, gtk_tree_row_reference_free);
    g_hash_table_remove_all(self->visual_base);
    g_signal_emit(self, signals[SIGNAL_VISUAL_CHANGED], 0);
}

void sfe_pane_visual_finish(SfePane* self)
{
    if (!self->visual)
        return;
    sfe_pane_visual_end(self);
    if (!sfe_config()->visual_clear)
        return;
    if (self->view == SFE_VIEW_GRID)
        gtk_icon_view_unselect_all(GTK_ICON_VIEW(self->grid));
    else
        gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(GTK_TREE_VIEW(self->list)));
}

void sfe_pane_center_cursor(SfePane* self)
{
    GtkTreePath* path = NULL;
    if (self->visual) { /* the first selected item */
        GList* rows = self->view == SFE_VIEW_GRID
            ? gtk_icon_view_get_selected_items(GTK_ICON_VIEW(self->grid))
            : gtk_tree_selection_get_selected_rows(gtk_tree_view_get_selection(GTK_TREE_VIEW(self->list)), NULL);
        for (GList* l = rows; l != NULL; l = l->next) {
            if (path == NULL || gtk_tree_path_compare(l->data, path) < 0)
                path = l->data;
        }
        path = path ? gtk_tree_path_copy(path) : NULL;
        g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
    }
    if (path == NULL)
        path = highlighted(self);
    if (path == NULL)
        return;
    if (self->view == SFE_VIEW_GRID)
        gtk_icon_view_scroll_to_path(GTK_ICON_VIEW(self->grid), path, TRUE, 0.5, 0.5);
    else
        gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(self->list), path, NULL, TRUE, 0.5, 0);
    /* back to the left edge, where the names are */
    GtkAdjustment* h = gtk_scrollable_get_hadjustment(GTK_SCROLLABLE(active_view(self)));
    if (h != NULL)
        gtk_adjustment_set_value(h, gtk_adjustment_get_lower(h));
    gtk_tree_path_free(path);
}

/* Opens files in their applications; the first directory is opened here. */
void sfe_pane_open_files(SfePane* self, GList* files)
{
    for (GList* l = files; l != NULL; l = l->next) {
        g_autoptr(GFileInfo) info = g_file_query_info(l->data, G_FILE_ATTRIBUTE_STANDARD_TYPE,
            G_FILE_QUERY_INFO_NONE, NULL, NULL);
        if (info == NULL)
            continue;
        bool dir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
        activate_file(self, l->data, info);
        if (dir)
            return; /* the pane moved away; the rest no longer applies */
    }
}

/* Selects `name` if it is listed: the first of a batch replaces the
 * selection (and gets the cursor), the others are added to it. */
static bool reveal_now(SfePane* self, const char* name)
{
    GtkTreePath* path = sfe_dir_model_find(self->model, name);
    if (path == NULL)
        return false;
    if (self->reveal_replace) {
        select_path(self, path);
        self->reveal_replace = false;
    } else if (self->view == SFE_VIEW_GRID) {
        gtk_icon_view_select_path(GTK_ICON_VIEW(self->grid), path);
    } else {
        gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(self->list)), path);
    }
    gtk_tree_path_free(path);
    return true;
}

void sfe_pane_reveal_many(SfePane* self, char* const* names, guint n)
{
    if (self->model == NULL) /* pane already closed */
        return;
    g_clear_pointer(&self->reveal_names, g_hash_table_unref);
    self->reveal_replace = true;
    for (guint i = 0; i < n; i++) {
        if (reveal_now(self, names[i]))
            continue;
        if (self->reveal_names == NULL)
            self->reveal_names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        g_hash_table_add(self->reveal_names, g_strdup(names[i])); /* selected once the monitor sees it */
    }
}

void sfe_pane_reveal(SfePane* self, const char* name)
{
    char* names[] = { (char*)name };
    sfe_pane_reveal_many(self, names, 1);
}

static void on_file_added(SfeDirModel* model, const char* name, gpointer data)
{
    (void)model;
    SfePane* self = data;
    if (self->reveal_names == NULL || !g_hash_table_remove(self->reveal_names, name))
        return;
    reveal_now(self, name);
    if (g_hash_table_size(self->reveal_names) == 0)
        g_clear_pointer(&self->reveal_names, g_hash_table_unref);
}

/* ------------------------------------------------------------------------ */
/* Context menu                                                              */
/* ------------------------------------------------------------------------ */

/* Right-click selects like file managers do: an unselected item becomes the
 * only selection, empty space clears the selection. */
static void select_for_menu(SfePane* self, GtkWidget* view, GdkEventButton* event)
{
    GtkTreePath* path = NULL;

    if (view == self->grid) {
        GtkIconView* grid = GTK_ICON_VIEW(view);
        path = gtk_icon_view_get_path_at_pos(grid, (int)event->x, (int)event->y);
        if (path == NULL) {
            gtk_icon_view_unselect_all(grid);
        } else if (!gtk_icon_view_path_is_selected(grid, path)) {
            gtk_icon_view_unselect_all(grid);
            gtk_icon_view_select_path(grid, path);
            gtk_icon_view_set_cursor(grid, path, NULL, FALSE);
        }
    } else {
        GtkTreeView* list = GTK_TREE_VIEW(view);
        GtkTreeSelection* sel = gtk_tree_view_get_selection(list);
        if (!gtk_tree_view_get_path_at_pos(list, (int)event->x, (int)event->y, &path, NULL, NULL, NULL)) {
            gtk_tree_selection_unselect_all(sel);
        } else if (!gtk_tree_selection_path_is_selected(sel, path)) {
            gtk_tree_selection_unselect_all(sel);
            gtk_tree_selection_select_path(sel, path);
        }
    }
    if (path != NULL)
        gtk_tree_path_free(path);
}

static void show_menu(SfePane* self, GtkWidget* view, const GdkEvent* event)
{
    GtkWidget* window = gtk_widget_get_toplevel(GTK_WIDGET(self));
    gtk_widget_grab_focus(view); /* makes this pane the active one */
    if (SFE_IS_WINDOW(window))
        sfe_context_menu_popup(SFE_WINDOW(window), view, event);
}

/* ------------------------------------------------------------------------ */
/* Grid: Shift-selection in reading order                                    */
/* ------------------------------------------------------------------------ */

/* GtkIconView selects the rectangle between two items; a file list reads
 * like text, so Shift selects from the anchor to the item in reading order:
 * the rest of the first row, the rows between, the start of the last one. */

static GtkTreePath* anchor_path(SfePane* self)
{
    GtkTreePath* path = self->grid_anchor ? gtk_tree_row_reference_get_path(self->grid_anchor) : NULL;
    return path ? path : highlighted(self);
}

static void set_anchor(SfePane* self, GtkTreePath* path)
{
    forget_anchor(self);
    GtkTreeModel* model = sfe_dir_model_get_model(self->model);
    if (model != NULL && path != NULL)
        self->grid_anchor = gtk_tree_row_reference_new(model, path);
}

/* Selects every item from `a` to `b` (and keeps the rest with `keep`). */
static void select_run(SfePane* self, GtkTreePath* a, GtkTreePath* b, bool keep)
{
    GtkIconView* grid = GTK_ICON_VIEW(self->grid);
    int ia = gtk_tree_path_get_indices(a)[0], ib = gtk_tree_path_get_indices(b)[0];
    if (!keep)
        gtk_icon_view_unselect_all(grid);
    for (int i = MIN(ia, ib); i <= MAX(ia, ib); i++) {
        GtkTreePath* path = gtk_tree_path_new_from_indices(i, -1);
        gtk_icon_view_select_path(grid, path);
        gtk_tree_path_free(path);
    }
}

/* Shift+click selects the run from the anchor; a plain click (or Ctrl+click)
 * moves the anchor and is left to GTK. True when handled. */
static bool grid_click(SfePane* self, GdkEventButton* event)
{
    GtkIconView* grid = GTK_ICON_VIEW(self->grid);
    if (self->single)
        return false; /* GTK selects the one item */
    GtkTreePath* path = gtk_icon_view_get_path_at_pos(grid, (int)event->x, (int)event->y);
    if (path == NULL)
        return false;
    if (!(event->state & GDK_SHIFT_MASK)) {
        set_anchor(self, path);
        gtk_tree_path_free(path);
        return false;
    }
    GtkTreePath* anchor = anchor_path(self);
    if (anchor != NULL) {
        if (self->grid_anchor == NULL)
            set_anchor(self, anchor);
        select_run(self, anchor, path, event->state & GDK_CONTROL_MASK);
        gtk_icon_view_set_cursor(grid, path, NULL, FALSE); /* (does not touch the selection) */
        gtk_widget_grab_focus(self->grid);
        gtk_tree_path_free(anchor);
    }
    gtk_tree_path_free(path);
    return anchor != NULL;
}

/* Shift+arrows, Home, End, Page Up / Down extend the run from the anchor;
 * the same keys without Shift move the anchor. True when handled. */
static bool grid_key(SfePane* self, GdkEventKey* event)
{
    GtkMovementStep step;
    int count;
    switch (event->keyval) {
    case GDK_KEY_Left: case GDK_KEY_KP_Left: step = GTK_MOVEMENT_VISUAL_POSITIONS; count = -1; break;
    case GDK_KEY_Right: case GDK_KEY_KP_Right: step = GTK_MOVEMENT_VISUAL_POSITIONS; count = 1; break;
    case GDK_KEY_Up: case GDK_KEY_KP_Up: step = GTK_MOVEMENT_DISPLAY_LINES; count = -1; break;
    case GDK_KEY_Down: case GDK_KEY_KP_Down: step = GTK_MOVEMENT_DISPLAY_LINES; count = 1; break;
    case GDK_KEY_Home: case GDK_KEY_KP_Home: step = GTK_MOVEMENT_BUFFER_ENDS; count = -1; break;
    case GDK_KEY_End: case GDK_KEY_KP_End: step = GTK_MOVEMENT_BUFFER_ENDS; count = 1; break;
    case GDK_KEY_Page_Up: case GDK_KEY_KP_Page_Up: step = GTK_MOVEMENT_PAGES; count = -1; break;
    case GDK_KEY_Page_Down: case GDK_KEY_KP_Page_Down: step = GTK_MOVEMENT_PAGES; count = 1; break;
    default: return false;
    }
    if (!(event->state & GDK_SHIFT_MASK) || self->single) {
        forget_anchor(self); /* GTK moves; the new cursor is the next anchor */
        return false;
    }
    GtkTreePath* anchor = anchor_path(self);
    if (anchor == NULL)
        return false;
    if (self->grid_anchor == NULL)
        set_anchor(self, anchor);
    if (step == GTK_MOVEMENT_VISUAL_POSITIONS) { /* in reading order: across row ends too */
        GtkTreePath* at = highlighted(self);
        int index = at ? CLAMP(gtk_tree_path_get_indices(at)[0] + count, 0, n_rows(self) - 1) : 0;
        GtkTreePath* next = gtk_tree_path_new_from_indices(index, -1);
        gtk_icon_view_set_cursor(GTK_ICON_VIEW(self->grid), next, NULL, FALSE);
        gtk_icon_view_scroll_to_path(GTK_ICON_VIEW(self->grid), next, FALSE, 0, 0);
        gtk_tree_path_free(next);
        gtk_tree_path_free(at);
    } else {
        move_cursor(self, step, count, true);
    }
    GtkTreePath* cursor = highlighted(self);
    if (cursor != NULL)
        select_run(self, anchor, cursor, event->state & GDK_CONTROL_MASK);
    gtk_tree_path_free(cursor);
    gtk_tree_path_free(anchor);
    return true;
}

static gboolean on_view_button_press(GtkWidget* view, GdkEventButton* event, gpointer data)
{
    SfePane* self = data;

    sfe_pane_visual_end(self); /* the mouse takes over the selection */
    grid_event_to_bin(view, event->window, &event->x, &event->y);
    if (drag_press(self, view, event))
        return TRUE;
    band_press(self, view, event);
    if (GTK_IS_ICON_VIEW(view) && event->type == GDK_BUTTON_PRESS && event->button == GDK_BUTTON_PRIMARY
        && grid_click(self, event))
        return TRUE;
    if (event->type != GDK_BUTTON_PRESS || event->button != GDK_BUTTON_SECONDARY)
        return FALSE;
    if (GTK_IS_TREE_VIEW(view) && event->window != gtk_tree_view_get_bin_window(GTK_TREE_VIEW(view)))
        return FALSE; /* column headers */
    select_for_menu(self, view, event);
    show_menu(self, view, (GdkEvent*)event);
    return TRUE;
}

/* Menu key / Shift+F10. */
static gboolean on_view_popup_menu(GtkWidget* view, gpointer data)
{
    show_menu(SFE_PANE(data), view, NULL);
    return TRUE;
}

static void on_grid_activated(GtkIconView* view, GtkTreePath* path, gpointer data)
{
    (void)view;
    activate_path(SFE_PANE(data), path);
}

static void on_list_activated(GtkTreeView* view, GtkTreePath* path, GtkTreeViewColumn* column, gpointer data)
{
    (void)view, (void)column;
    activate_path(SFE_PANE(data), path);
}

/* Clicking a column header sorts by it; clicking again flips the order.
 * Saved in the config, so every view (and the next start) follows. */
static void on_column_clicked(GtkTreeViewColumn* column, gpointer data)
{
    (void)data;
    SfeSortKey clicked = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(column), "sfe-sort-key"));
    const SfeConfig* cfg = sfe_config();
    SfeSortOrder order = SFE_SORT_ASCENDING;
    if (cfg->sort_by == (int)clicked && cfg->sort_order == SFE_SORT_ASCENDING)
        order = SFE_SORT_DESCENDING;
    sfe_application_set_sort(sfe_app(), clicked, order);
}

static gboolean on_view_key_press(GtkWidget* view, GdkEventKey* event, gpointer data)
{
    guint mods = event->state & gtk_accelerator_get_default_mod_mask();

    if (GTK_IS_ICON_VIEW(view) && grid_key(SFE_PANE(data), event))
        return TRUE;

    if (event->keyval == GDK_KEY_Escape && mods == 0) {
        if (searching(SFE_PANE(data)))
            end_search(SFE_PANE(data), true);
        else if (SFE_PANE(data)->filter != NULL)
            end_filter(SFE_PANE(data), true);
        else if (GTK_IS_ICON_VIEW(view))
            gtk_icon_view_unselect_all(GTK_ICON_VIEW(view));
        else
            gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(GTK_TREE_VIEW(view)));
        return TRUE;
    }
    if (event->keyval == GDK_KEY_BackSpace && mods == 0) {
        sfe_pane_go_up(SFE_PANE(data));
        return TRUE;
    }
    return FALSE;
}

static gboolean on_button_press(GtkWidget* widget, GdkEventButton* event, gpointer data)
{
    (void)widget;
    SfePane* self = data;

    if (event->type != GDK_BUTTON_PRESS)
        return FALSE;
    if (event->button == MOUSE_BUTTON_BACK) {
        sfe_pane_go_back(self);
        return TRUE;
    }
    if (event->button == MOUSE_BUTTON_FORWARD) {
        sfe_pane_go_forward(self);
        return TRUE;
    }
    return FALSE;
}

static GtkWidget* scrolled(GtkWidget* child)
{
    GtkWidget* sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(sw), child);
    return sw;
}

/* Selections change a row at a time (Shift+click, select all, visual
 * mode): "selection-changed" goes out once, when they are done. */
static gboolean emit_selection_changed(gpointer data)
{
    SfePane* self = data;
    self->selection_source = 0;
    g_signal_emit(self, signals[SIGNAL_SELECTION_CHANGED], 0);
    return G_SOURCE_REMOVE;
}

static void on_selection_changed(gpointer view_or_selection, gpointer data)
{
    (void)view_or_selection;
    SfePane* self = data;
    if (self->selection_source == 0)
        self->selection_source = g_idle_add(emit_selection_changed, self);
}

/* While a query is set (searching or filtered), the letters of a name that
 * matched it: in the accent colour, bold and underlined in a selected row. */
static void name_cell(GtkCellLayout* layout, GtkCellRenderer* cell, GtkTreeModel* model, GtkTreeIter* iter,
    gpointer data)
{
    (void)layout;
    SfePane* self = data;
    PangoAttrList* base = g_object_get_data(G_OBJECT(cell), "sfe-base-attributes");
    const char* query = sfe_dir_model_get_query(self->model);
    g_autofree char* name = NULL;
    g_autoptr(GArray) at = g_array_new(FALSE, FALSE, sizeof(int));

    if (query != NULL)
        gtk_tree_model_get(model, iter, SFE_COL_NAME, &name, -1);
    if (name == NULL || sfe_fuzzy_match(query, name, at) == SFE_FUZZY_NO_MATCH || at->len == 0) {
        g_object_set(cell, "attributes", base, NULL);
        return;
    }
    bool grid = cell == self->grid_text;
    bool selected;
    if (grid) {
        g_autoptr(GtkTreePath) path = gtk_tree_model_get_path(model, iter);
        selected = gtk_icon_view_path_is_selected(GTK_ICON_VIEW(self->grid), path);
    } else {
        selected = gtk_tree_selection_iter_is_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(self->list)), iter);
    }
    GdkRGBA accent = { 0.3, 0.5, 0.9, 1 };
    gtk_style_context_lookup_color(gtk_widget_get_style_context(grid ? self->grid : self->list), "sfe_accent", &accent);

    PangoAttrList* attrs = base ? pango_attr_list_copy(base) : pango_attr_list_new();
    for (guint i = 0; i < at->len; i++) {
        int start = g_array_index(at, int, i);
        int end = g_utf8_next_char(name + start) - name;
        PangoAttribute* marks[2] = { pango_attr_weight_new(PANGO_WEIGHT_BOLD),
            selected ? pango_attr_underline_new(PANGO_UNDERLINE_SINGLE)
                     : pango_attr_foreground_new(accent.red * 65535, accent.green * 65535, accent.blue * 65535) };
        for (int k = 0; k < 2; k++) {
            marks[k]->start_index = start;
            marks[k]->end_index = end;
            pango_attr_list_insert(attrs, marks[k]);
        }
    }
    g_object_set(cell, "attributes", attrs, NULL);
    pango_attr_list_unref(attrs);
}

static GtkWidget* build_filter_badge(SfePane* self)
{
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget* icon = sfe_scaled_icon_new("system-search-symbolic", 16);
    GtkWidget* clear = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);

    gtk_style_context_add_class(gtk_widget_get_style_context(box), "sfe-filter-badge");
    gtk_button_set_relief(GTK_BUTTON(clear), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(clear, "Show all files (Esc)");
    gtk_widget_set_can_focus(clear, FALSE);
    g_signal_connect(clear, "clicked", G_CALLBACK(on_filter_clear), self);
    self->filter_label = gtk_label_new(NULL);
    gtk_label_set_ellipsize(GTK_LABEL(self->filter_label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_pack_start(GTK_BOX(box), icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), self->filter_label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), clear, FALSE, FALSE, 0);
    gtk_widget_show_all(box);
    gtk_widget_set_no_show_all(box, TRUE);
    gtk_widget_hide(box);
    self->filter_badge = box;
    return box;
}

/* GtkIconView takes the mouse only on its inner window, which ends at the
 * last row. Scrolled into the room after the files, the wheel there went
 * past the grid and its scrolled window never saw it; presses there (a
 * click, a menu, a band) were lost too. */
static void on_grid_realized(GtkWidget* grid, gpointer data)
{
    (void)data;
    GdkWindow* window = gtk_widget_get_window(grid);
    gdk_window_set_events(window, gdk_window_get_events(window) | GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK
        | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_BUTTON_MOTION_MASK);
}

/* Rows hold only the file; icons and texts are made when a row is drawn. */
static void icon_cell(GtkCellLayout* layout, GtkCellRenderer* cell, GtkTreeModel* model, GtkTreeIter* iter, gpointer data)
{
    (void)layout;
    SfePane* self = data;
    g_autoptr(GFileInfo) info = NULL;
    gtk_tree_model_get(model, iter, SFE_COL_INFO, &info, -1);
    bool grid = g_object_get_data(G_OBJECT(cell), "sfe-grid") != NULL;
    cairo_surface_t* surface = info ? sfe_dir_model_icon(self->model, info, grid) : NULL;
    g_object_set(cell, "surface", surface, NULL);
    if (surface)
        cairo_surface_destroy(surface);
}

static void text_cell(GtkTreeViewColumn* column, GtkCellRenderer* cell, GtkTreeModel* model, GtkTreeIter* iter, gpointer data)
{
    (void)data;
    g_autoptr(GFileInfo) info = NULL;
    gtk_tree_model_get(model, iter, SFE_COL_INFO, &info, -1);
    g_autofree char* text = info ? sfe_dir_model_text(info,
        GPOINTER_TO_INT(g_object_get_data(G_OBJECT(column), "sfe-sort-key"))) : NULL;
    g_object_set(cell, "text", text, NULL);
}

/* A grid name in its fixed two-line box. GtkCellRendererText
 * would cut a third line off; this draws at most two, with "…" in the
 * middle of the second, so the end of the name (its extension) stays. */
typedef GtkCellRendererText SfeNameRenderer;
typedef GtkCellRendererTextClass SfeNameRendererClass;
G_DEFINE_TYPE(SfeNameRenderer, sfe_name_renderer, GTK_TYPE_CELL_RENDERER_TEXT)

static void sfe_name_renderer_render(GtkCellRenderer* cell, cairo_t* cr, GtkWidget* widget,
    const GdkRectangle* background, const GdkRectangle* area, GtkCellRendererState flags)
{
    (void)background;
    g_autofree char* text = NULL;
    PangoAttrList* attrs = NULL;
    int xpad, ypad, wrap_width;
    g_object_get(cell, "text", &text, "attributes", &attrs, "wrap-width", &wrap_width, NULL);
    gtk_cell_renderer_get_padding(cell, &xpad, &ypad);

    PangoLayout* layout = gtk_widget_create_pango_layout(widget, text);
    pango_layout_set_attributes(layout, attrs);
    pango_layout_set_width(layout, MAX(1, MIN(wrap_width, area->width - 2 * xpad)) * PANGO_SCALE);
    pango_layout_set_height(layout, -2); /* two lines at most */
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_MIDDLE);
    pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);

    GtkStyleContext* context = gtk_widget_get_style_context(widget);
    gtk_style_context_save(context);
    gtk_style_context_set_state(context, gtk_cell_renderer_get_state(cell, widget, flags));
    gtk_render_layout(context, cr, area->x + xpad, area->y + ypad, layout);
    gtk_style_context_restore(context);
    g_object_unref(layout);
    if (attrs != NULL)
        pango_attr_list_unref(attrs);
}

static void sfe_name_renderer_class_init(SfeNameRendererClass* klass)
{
    GTK_CELL_RENDERER_CLASS(klass)->render = sfe_name_renderer_render;
}

static void sfe_name_renderer_init(SfeNameRenderer* self)
{
    (void)self;
}

static GtkWidget* build_grid(SfePane* self)
{
    GtkWidget* grid = gtk_icon_view_new();
    GtkCellLayout* layout = GTK_CELL_LAYOUT(grid);
    GtkCellRenderer* icon = gtk_cell_renderer_pixbuf_new();
    GtkCellRenderer* text = g_object_new(sfe_name_renderer_get_type(), NULL);

    gtk_icon_view_set_selection_mode(GTK_ICON_VIEW(grid), GTK_SELECTION_MULTIPLE);
    gtk_icon_view_set_item_padding(GTK_ICON_VIEW(grid), 4);
    gtk_icon_view_set_row_spacing(GTK_ICON_VIEW(grid), 4);
    gtk_icon_view_set_column_spacing(GTK_ICON_VIEW(grid), 4);

    gtk_cell_layout_pack_start(layout, icon, FALSE);
    g_object_set_data(G_OBJECT(icon), "sfe-grid", GINT_TO_POINTER(1));
    gtk_cell_layout_set_cell_data_func(layout, icon, icon_cell, self, NULL);

    /* Long names wrap, but without Pango's hyphens ("wallpaper.pn-g"). */
    PangoAttrList* attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_insert_hyphens_new(FALSE));
    /* yalign 0: in a row with a two-line name, short names sit right under
     * their icon instead of being centred with space above and below. */
    g_object_set(text, "xalign", 0.5, "yalign", 0.0, "alignment", PANGO_ALIGN_CENTER,
        "wrap-mode", PANGO_WRAP_WORD_CHAR, "attributes", attrs, NULL);
    gtk_cell_layout_pack_start(layout, text, FALSE);
    gtk_cell_layout_add_attribute(layout, text, "text", SFE_COL_NAME);
    self->grid_text = text;
    self->grid_icon = icon;
    g_object_set_data_full(G_OBJECT(text), "sfe-base-attributes", attrs, /* takes our reference */
        (GDestroyNotify)pango_attr_list_unref);
    gtk_cell_layout_set_cell_data_func(layout, text, name_cell, self, NULL);

    g_signal_connect(grid, "item-activated", G_CALLBACK(on_grid_activated), self);
    g_signal_connect_after(grid, "realize", G_CALLBACK(on_grid_realized), NULL);
    g_signal_connect_after(grid, "style-updated", G_CALLBACK(on_grid_style_updated), self);
    g_signal_connect(grid, "selection-changed", G_CALLBACK(on_selection_changed), self);
    g_signal_connect(grid, "key-press-event", G_CALLBACK(on_view_key_press), self);
    g_signal_connect(grid, "button-press-event", G_CALLBACK(on_view_button_press), self);
    g_signal_connect(grid, "motion-notify-event", G_CALLBACK(on_view_motion), self);
    g_signal_connect(grid, "button-release-event", G_CALLBACK(on_view_release), self);
    g_signal_connect_after(grid, "draw", G_CALLBACK(on_view_draw_after), self);
    g_signal_connect(grid, "popup-menu", G_CALLBACK(on_view_popup_menu), self);
    setup_dnd(self, grid);
    return grid;
}

static GtkTreeViewColumn* add_text_column(SfePane* self, GtkWidget* list, const char* title,
    SfeSortKey key, float xalign)
{
    GtkCellRenderer* text = gtk_cell_renderer_text_new();
    GtkTreeViewColumn* column = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(column, title);
    gtk_tree_view_column_pack_start(column, text, TRUE);
    if (key == SFE_SORT_NAME)
        gtk_tree_view_column_add_attribute(column, text, "text", SFE_COL_NAME);
    else
        gtk_tree_view_column_set_cell_data_func(column, text, text_cell, NULL, NULL);

    g_object_set(text, "xalign", xalign, "ypad", ROW_PADDING, "xpad", 6, NULL);
    /* Not resizable by hand: a dragged width sticks and breaks fit_columns(). */
    gtk_tree_view_column_set_resizable(column, FALSE);
    gtk_tree_view_column_set_clickable(column, TRUE);
    g_object_set_data(G_OBJECT(column), "sfe-sort-key", GINT_TO_POINTER(key));
    g_signal_connect(column, "clicked", G_CALLBACK(on_column_clicked), self);
    gtk_tree_view_append_column(GTK_TREE_VIEW(list), column);
    self->columns[key] = column;
    return column;
}

static GtkWidget* build_list(SfePane* self)
{
    GtkWidget* list = gtk_tree_view_new();
    GtkTreeView* tree = GTK_TREE_VIEW(list);

    gtk_tree_view_set_enable_search(tree, FALSE);
    gtk_tree_view_set_rubber_banding(tree, TRUE);
    gtk_tree_selection_set_mode(gtk_tree_view_get_selection(tree), GTK_SELECTION_MULTIPLE);

    /* Name: icon + text in one column. */
    GtkTreeViewColumn* name = add_text_column(self, list, "Name", SFE_SORT_NAME, 0.0f);
    GtkCellRenderer* icon = gtk_cell_renderer_pixbuf_new();
    g_object_set(icon, "ypad", ROW_PADDING, "xpad", 4, NULL);
    GList* cells = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(name));
    g_object_set(cells->data, "ellipsize", PANGO_ELLIPSIZE_MIDDLE, NULL);
    gtk_cell_layout_set_cell_data_func(GTK_CELL_LAYOUT(name), cells->data, name_cell, self, NULL);
    g_list_free(cells);
    gtk_tree_view_column_pack_start(name, icon, FALSE);
    gtk_cell_layout_set_cell_data_func(GTK_CELL_LAYOUT(name), icon, icon_cell, self, NULL);
    gtk_cell_layout_reorder(GTK_CELL_LAYOUT(name), icon, 0);
    gtk_tree_view_column_set_expand(name, TRUE);

    add_text_column(self, list, "Size", SFE_SORT_SIZE, 1.0f);
    add_text_column(self, list, "Modified", SFE_SORT_MODIFIED, 0.0f);
    add_text_column(self, list, "Type", SFE_SORT_TYPE, 0.0f);

    /* Fixed-height mode. GtkTreeView then measures one row instead
     * of all of them; every column needs a fixed width. measure_columns()
     * and fit_columns() set them (before the first paint). */
    for (size_t i = 0; i < G_N_ELEMENTS(self->columns); i++)
        gtk_tree_view_column_set_sizing(self->columns[i], GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_expand(name, FALSE); /* fit_columns() gives Name the rest; GTK's expand overshoots */
    gtk_tree_view_set_fixed_height_mode(tree, TRUE);

    g_signal_connect(list, "row-activated", G_CALLBACK(on_list_activated), self);
    g_signal_connect(gtk_tree_view_get_selection(tree), "changed", G_CALLBACK(on_selection_changed), self);
    g_signal_connect(list, "key-press-event", G_CALLBACK(on_view_key_press), self);
    g_signal_connect(list, "button-press-event", G_CALLBACK(on_view_button_press), self);
    g_signal_connect(list, "motion-notify-event", G_CALLBACK(on_view_motion), self);
    g_signal_connect(list, "button-release-event", G_CALLBACK(on_view_release), self);
    g_signal_connect_after(list, "draw", G_CALLBACK(on_view_draw_after), self);
    g_signal_connect(list, "popup-menu", G_CALLBACK(on_view_popup_menu), self);
    setup_dnd(self, list);
    g_signal_connect(list, "size-allocate", G_CALLBACK(on_list_allocated), self);
    return list;
}

/* ------------------------------------------------------------------------ */
/* Top bar                                                                   */
/* ------------------------------------------------------------------------ */

static void on_entry_activate(GtkEntry* entry, gpointer data)
{
    SfePane* self = data;
    g_autofree char* path = sfe_path_entry_get_path(GTK_WIDGET(entry));
    g_autoptr(GFile) file = g_file_new_for_commandline_arg(path);
    g_autoptr(GFileInfo) info = g_file_query_info(file, SFE_FILE_ATTRIBUTES, G_FILE_QUERY_INFO_NONE, NULL, NULL);

    if (info == NULL) {
        sfe_path_entry_set_error(GTK_WIDGET(entry), TRUE);
        return;
    }
    activate_file(self, file, info);
    sfe_pane_focus_view(self);
}

/* Escape restores the current path and returns to the files. */
static gboolean on_entry_key_press(GtkWidget* entry, GdkEventKey* event, gpointer data)
{
    SfePane* self = data;

    if (event->keyval != GDK_KEY_Escape)
        return FALSE;
    if (self->dir != NULL)
        sfe_path_entry_set_dir(entry, self->dir);
    sfe_pane_focus_view(self);
    return TRUE;
}

static void on_view_button_clicked(GtkButton* button, gpointer data)
{
    (void)button;
    SfePane* self = data;
    sfe_pane_set_view(self, self->view == SFE_VIEW_GRID ? SFE_VIEW_LIST : SFE_VIEW_GRID);
}

static GtkWidget* icon_button(const char* icon, const char* tooltip, GCallback callback, gpointer data)
{
    GtkWidget* button = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(button), sfe_scaled_icon_new(icon, 16));
    gtk_widget_set_tooltip_text(button, tooltip);
    gtk_widget_set_focus_on_click(button, FALSE);
    g_signal_connect_swapped(button, "clicked", callback, data);
    return button;
}

static void on_split_v_clicked(SfePane* self)
{
    g_signal_emit(self, signals[SIGNAL_SPLIT], 0, SFE_SPLIT_VERTICAL);
}

static void on_split_h_clicked(SfePane* self)
{
    g_signal_emit(self, signals[SIGNAL_SPLIT], 0, SFE_SPLIT_HORIZONTAL);
}

static void on_close_clicked(SfePane* self)
{
    g_signal_emit(self, signals[SIGNAL_CLOSE], 0);
}

static GtkWidget* build_top_bar(SfePane* self)
{
    GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* nav = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);

    gtk_style_context_add_class(gtk_widget_get_style_context(bar), "sfe-topbar");

    self->back_button = icon_button("go-previous-symbolic", "Back (Alt+Left)", G_CALLBACK(sfe_pane_go_back), self);
    self->forward_button = icon_button("go-next-symbolic", "Forward (Alt+Right)", G_CALLBACK(sfe_pane_go_forward), self);
    self->up_button = icon_button("go-up-symbolic", "Parent folder (Alt+Up)", G_CALLBACK(sfe_pane_go_up), self);
    gtk_container_add(GTK_CONTAINER(nav), self->back_button);
    gtk_container_add(GTK_CONTAINER(nav), self->forward_button);
    gtk_container_add(GTK_CONTAINER(nav), self->up_button);

    self->entry = sfe_path_entry_new();
    g_signal_connect(self->entry, "activate", G_CALLBACK(on_entry_activate), self);
    g_signal_connect(self->entry, "key-press-event", G_CALLBACK(on_entry_key_press), self);

    self->view_button = gtk_button_new();
    gtk_widget_set_focus_on_click(self->view_button, FALSE);
    g_signal_connect(self->view_button, "clicked", G_CALLBACK(on_view_button_clicked), self);

    gtk_box_pack_start(GTK_BOX(bar), nav, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), self->entry, TRUE, TRUE, 0);
    GtkWidget* split = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    self->split_v_button = icon_button("sfe-split-vertical-symbolic", "Split side by side (Ctrl+\\)",
        G_CALLBACK(on_split_v_clicked), self);
    self->split_h_button = icon_button("sfe-split-horizontal-symbolic", "Split top and bottom (Ctrl+-)",
        G_CALLBACK(on_split_h_clicked), self);
    self->close_button = icon_button("window-close-symbolic", "Close this view (Ctrl+W)",
        G_CALLBACK(on_close_clicked), self);
    gtk_container_add(GTK_CONTAINER(split), self->split_v_button);
    gtk_container_add(GTK_CONTAINER(split), self->split_h_button);
    gtk_container_add(GTK_CONTAINER(split), self->close_button);

    gtk_box_pack_start(GTK_BOX(bar), self->view_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), split, FALSE, FALSE, 0);
    return bar;
}

/* ------------------------------------------------------------------------ */
/* Public focus helpers                                                      */
/* ------------------------------------------------------------------------ */

void sfe_pane_focus_location(SfePane* self)
{
    gtk_widget_grab_focus(self->entry); /* selects the whole path */
}

void sfe_pane_focus_view(SfePane* self)
{
    bool message = gtk_stack_get_visible_child(GTK_STACK(self->stack)) == self->message_box;
    gtk_widget_grab_focus(message ? self->message_box : active_view(self));
}

SfeViewMode sfe_pane_get_view(SfePane* self)
{
    return self->view;
}

void sfe_pane_set_split_buttons(SfePane* self, bool vertical, bool horizontal, bool close)
{
    gtk_widget_set_sensitive(self->split_v_button, vertical);
    gtk_widget_set_sensitive(self->split_h_button, horizontal);
    gtk_widget_set_sensitive(self->close_button, close);
}

void sfe_pane_set_active(SfePane* self, bool active)
{
    GtkStyleContext* style = gtk_widget_get_style_context(GTK_WIDGET(self));
    if (active) {
        gtk_style_context_add_class(style, "sfe-active");
    } else {
        gtk_style_context_remove_class(style, "sfe-active");
        sfe_pane_visual_end(self);
    }
}

/* ------------------------------------------------------------------------ */
/* GObject                                                                   */
/* ------------------------------------------------------------------------ */

static void sfe_pane_dispose(GObject* object)
{
    SfePane* self = SFE_PANE(object);
    g_clear_handle_id(&self->search_timer, g_source_remove);

    if (self->config_handler != 0) {
        g_signal_handler_disconnect(sfe_app(), self->config_handler);
        self->config_handler = 0;
    }
    if (self->thumb_cancel != NULL) /* before `self` goes: results would land here */
        g_cancellable_cancel(self->thumb_cancel);
    g_clear_object(&self->thumb_cancel);
    g_clear_handle_id(&self->thumb_source, g_source_remove);
    g_clear_handle_id(&self->columns_source, g_source_remove);
    g_clear_handle_id(&self->selection_source, g_source_remove);
    if (self->model != NULL) {
        g_signal_handlers_disconnect_by_data(self->model, self);
        g_clear_object(&self->model);
    }
    g_clear_pointer(&self->history, sfe_history_free);
    g_clear_pointer(&self->select_after_load, g_free);
    g_clear_pointer(&self->reveal_names, g_hash_table_unref);
    g_clear_pointer(&self->visual_anchor, gtk_tree_row_reference_free);
    g_clear_pointer(&self->visual_base, g_hash_table_unref);
    g_clear_object(&self->search_base);
    g_clear_pointer(&self->filter, g_free);
    g_clear_pointer(&self->band_base, g_hash_table_unref);
    g_clear_pointer(&self->drag_path, gtk_tree_path_free);
    drop_reset(self);
    g_clear_object(&self->dir);
    G_OBJECT_CLASS(sfe_pane_parent_class)->dispose(object);
}

static void sfe_pane_class_init(SfePaneClass* klass)
{
    G_OBJECT_CLASS(klass)->dispose = sfe_pane_dispose;

    /* Emitted whenever the pane starts showing another directory. */
    signals[SIGNAL_DIRECTORY_CHANGED] = g_signal_new("directory-changed",
        G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    /* Visual mode started, ended or changed its selection. */
    signals[SIGNAL_VISUAL_CHANGED] = g_signal_new("visual-changed",
        G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    /* The split / close buttons were clicked (argument: SfeSplitKind). */
    signals[SIGNAL_SPLIT] = g_signal_new("split",
        G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
    signals[SIGNAL_CLOSE] = g_signal_new("close",
        G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    /* The selected files changed (once per change, after it is done). */
    signals[SIGNAL_SELECTION_CHANGED] = g_signal_new("selection-changed",
        G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void sfe_pane_init(SfePane* self)
{
    gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
    gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(self)), "sfe-pane");
    self->visual_base = g_hash_table_new_full(g_file_hash, (GEqualFunc)g_file_equal, g_object_unref, NULL);

    self->history = sfe_history_new();
    self->model = sfe_dir_model_new();
    self->cfg_sort_by = -1;
    self->cfg_sort_order = -1;
    g_signal_connect(self->model, "loaded", G_CALLBACK(on_loaded), self);
    g_signal_connect(self->model, "error", G_CALLBACK(on_load_error), self);
    g_signal_connect(self->model, "gone", G_CALLBACK(on_gone), self);
    g_signal_connect(self->model, "file-added", G_CALLBACK(on_file_added), self);
    g_signal_connect(self->model, "files-changed", G_CALLBACK(on_files_changed), self);
    g_signal_connect(self->model, "thumbnails-changed", G_CALLBACK(on_thumbnails_changed), self);
    self->cfg_thumbnails = -1;
    self->cfg_folder_covers = -1;
    self->thumb_size = -1;
    self->name_width = -1;

    self->grid = build_grid(self);
    self->list = build_list(self);
    self->message = gtk_label_new(NULL);
    gtk_label_set_justify(GTK_LABEL(self->message), GTK_JUSTIFY_CENTER);
    gtk_label_set_line_wrap(GTK_LABEL(self->message), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(self->message), "sfe-placeholder");
    self->message_box = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(self->message_box), FALSE);
    gtk_widget_set_can_focus(self->message_box, TRUE);
    gtk_widget_add_events(self->message_box, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(self->message_box, "button-press-event", G_CALLBACK(on_message_press), NULL);
    gtk_container_add(GTK_CONTAINER(self->message_box), self->message);

    self->stack = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(self->stack), scrolled(self->grid), "grid");
    gtk_stack_add_named(GTK_STACK(self->stack), scrolled(self->list), "list");
    GtkWidget* views[] = { self->grid, self->list };
    for (size_t i = 0; i < G_N_ELEMENTS(views); i++) { /* what is on screen changes: thumbnails */
        GtkScrolledWindow* sw = GTK_SCROLLED_WINDOW(gtk_widget_get_parent(views[i]));
        GtkAdjustment* v = gtk_scrolled_window_get_vadjustment(sw);
        g_signal_connect(v, "value-changed", G_CALLBACK(on_view_moved), self);
        g_signal_connect(v, "changed", G_CALLBACK(on_view_moved), self);
        g_object_set_data(G_OBJECT(v), "sfe-view-index", GINT_TO_POINTER((int)i));
        g_signal_connect(v, "changed", G_CALLBACK(on_scroll_range), self);
        g_signal_connect(v, "value-changed", G_CALLBACK(on_scroll_value), self);
    }
    gtk_stack_add_named(GTK_STACK(self->stack), self->message_box, "message");

    gtk_box_pack_start(GTK_BOX(self), build_top_bar(self), FALSE, FALSE, 0);
    self->search = SFE_SEARCH_BAR(sfe_search_bar_new());
    /* Positioned by on_overlay_position(); FILL keeps the width it gets. */
    g_signal_connect(self->search, "query-changed", G_CALLBACK(on_search_changed), self);
    g_signal_connect_swapped(self->search, "activate", G_CALLBACK(search_activate), self);
    g_signal_connect(self->search, "move", G_CALLBACK(on_search_move), self);
    g_signal_connect(self->search, "close", G_CALLBACK(on_search_close), self);

    self->overlay = gtk_overlay_new();
    gtk_container_add(GTK_CONTAINER(self->overlay), self->stack);
    gtk_overlay_add_overlay(GTK_OVERLAY(self->overlay), GTK_WIDGET(self->search));
    gtk_overlay_add_overlay(GTK_OVERLAY(self->overlay), build_filter_badge(self));
    g_signal_connect(self->overlay, "get-child-position", G_CALLBACK(on_overlay_position), self);
    gtk_box_pack_start(GTK_BOX(self), self->overlay, TRUE, TRUE, 0);

    g_signal_connect(self, "button-press-event", G_CALLBACK(on_button_press), self);
    g_signal_connect(self, "notify::scale-factor", G_CALLBACK(on_scale_factor_changed), self);
    self->config_handler = g_signal_connect(sfe_app(), "config-changed", G_CALLBACK(on_config_changed), self);

    self->view = sfe_config()->view;
    apply_config(self);
    gtk_widget_show_all(GTK_WIDGET(self));
    update_buttons(self);
}

SfePane* sfe_pane_new(GFile* dir)
{
    SfePane* self = g_object_new(SFE_TYPE_PANE, NULL);
    go_to(self, dir);
    return self;
}

