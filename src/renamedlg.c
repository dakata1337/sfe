#include "renamedlg.h"

#include "dialogs.h"
#include "rename.h"
#include "undo.h"
#include <stdbool.h>

#define TEMPLATE_HINT "{name} name   {ext} extension   {n} counter   {n:03} counter padded to 3 digits"

/* What every rename dialog carries. */
typedef struct {
    GFile** files;
    gboolean* is_dir;
    int n;
    SfeRenamedFunc done;
    gpointer data;
    GDestroyNotify destroy;
} Request;

static Request* request_new(GList* files, SfeRenamedFunc done, gpointer data, GDestroyNotify destroy)
{
    Request* req = g_new0(Request, 1);
    req->n = (int)g_list_length(files);
    req->files = g_new0(GFile*, req->n);
    req->is_dir = g_new0(gboolean, req->n);
    int i = 0;
    for (GList* l = files; l != NULL; l = l->next, i++) {
        req->files[i] = g_object_ref(l->data);
        req->is_dir[i] = g_file_query_file_type(l->data, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL) == G_FILE_TYPE_DIRECTORY;
    }
    req->done = done;
    req->data = data;
    req->destroy = destroy;
    return req;
}

static void request_free(gpointer p)
{
    Request* req = p;
    for (int i = 0; i < req->n; i++)
        g_object_unref(req->files[i]);
    g_free(req->files);
    g_free(req->is_dir);
    if (req->destroy != NULL)
        req->destroy(req->data);
    g_free(req);
}

/* Renames `files` to `names` and reports; returns whether it worked. */
static bool perform(GtkWidget* parent, Request* req, GFile** files, char** names, int n)
{
    g_autoptr(GError) error = NULL;

    if (!sfe_rename_files(files, names, n, &error)) {
        sfe_show_error(parent, "Renaming failed", error->message);
        return false;
    }
    sfe_undo_record_renames(files, names, n);
    if (req->done != NULL) {
        g_autoptr(GPtrArray) list = g_ptr_array_new();
        for (int i = 0; i < n; i++)
            g_ptr_array_add(list, names[i]);
        req->done(list, req->data);
    }
    return true;
}

/* ------------------------------------------------------------------------ */
/* Many files                                                                */
/* ------------------------------------------------------------------------ */

enum {
    COL_OLD,
    COL_NEW, /* markup */
    N_COLS,
};

typedef struct {
    Request* req;
    SfeRenameInclude include;
    GFile** files; /* the included subset of req->files (borrowed) */
    gboolean* is_dir;
    int n;
    GtkWidget* dialog;
    GtkWidget* template_;
    GtkWidget* find;
    GtkWidget* replace;
    GtkWidget* regex;
    GtkWidget* match_case;
    GtkWidget* case_mode;
    GtkWidget* start;
    GtkWidget* step;
    GtkWidget* status;
    GtkListStore* store;
    char** names; /* current preview, NULL-terminated */
    bool valid;
} Many;

static void many_free(gpointer p)
{
    Many* m = p;
    request_free(m->req);
    g_free(m->files);
    g_free(m->is_dir);
    g_object_unref(m->store);
    g_strfreev(m->names);
    g_free(m);
}

/* The theme's error colour as "#rrggbb", for Pango markup. */
static char* error_color(GtkWidget* widget)
{
    GdkRGBA c = { 0.85, 0.1, 0.1, 1 };
    gtk_style_context_lookup_color(gtk_widget_get_style_context(widget), "sfe_error", &c);
    return g_strdup_printf("#%02x%02x%02x", (int)(c.red * 255), (int)(c.green * 255), (int)(c.blue * 255));
}

static void set_status(Many* m, const char* text, bool error)
{
    if (error) {
        g_autofree char* color = error_color(m->dialog);
        g_autofree char* escaped = g_markup_escape_text(text, -1);
        g_autofree char* markup = g_strdup_printf("<span foreground=\"%s\">%s</span>", color, escaped);
        gtk_label_set_markup(GTK_LABEL(m->status), markup);
    } else {
        gtk_label_set_text(GTK_LABEL(m->status), text);
    }
}

static void update_preview(Many* m)
{
    g_autoptr(GError) error = NULL;
    SfeRenameRule rule = {
        .template_ = gtk_entry_get_text(GTK_ENTRY(m->template_)),
        .find = gtk_entry_get_text(GTK_ENTRY(m->find)),
        .replace = gtk_entry_get_text(GTK_ENTRY(m->replace)),
        .regex = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(m->regex)),
        .match_case = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(m->match_case)),
        .case_mode = gtk_combo_box_get_active(GTK_COMBO_BOX(m->case_mode)),
        .counter_start = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(m->start)),
        .counter_step = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(m->step)),
    };
    g_autoptr(SfeRenamer) renamer = sfe_renamer_new(&rule, &error);

    m->valid = false;
    if (renamer == NULL) {
        set_status(m, error->message, true);
        gtk_dialog_set_response_sensitive(GTK_DIALOG(m->dialog), GTK_RESPONSE_ACCEPT, FALSE);
        return;
    }

    g_strfreev(m->names);
    m->names = g_new0(char*, m->n + 1);
    for (int i = 0; i < m->n; i++) { /* numbered among the included items only */
        g_autofree char* old = g_file_get_basename(m->files[i]);
        g_autofree char* display = g_filename_display_name(old);
        m->names[i] = sfe_renamer_apply(renamer, display, m->is_dir[i], i);
    }

    SfeRenameStatus* status = g_new0(SfeRenameStatus, MAX(m->n, 1));
    int problems = sfe_rename_check(m->files, m->names, m->n, status);
    int changes = 0;
    g_autofree char* color = error_color(m->dialog);
    GtkTreeIter iter;
    bool valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(m->store), &iter);

    for (int i = 0; i < m->n && valid; i++) {
        g_autofree char* escaped = g_markup_escape_text(m->names[i], -1);
        g_autofree char* markup = NULL;
        switch (status[i]) {
        case SFE_RENAME_OK:
            markup = g_strdup(escaped);
            changes++;
            break;
        case SFE_RENAME_UNCHANGED:
            markup = g_strdup_printf("<span alpha=\"50%%\">%s</span>", escaped);
            break;
        default:
            markup = g_strdup_printf("<span foreground=\"%s\">%s  — %s</span>", color, escaped,
                sfe_rename_status_text(status[i]));
        }
        gtk_list_store_set(m->store, &iter, COL_NEW, markup, -1);
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(m->store), &iter);
    }
    g_free(status);

    if (problems > 0) {
        g_autofree char* text = g_strdup_printf("%d name%s cannot be used", problems, problems == 1 ? "" : "s");
        set_status(m, text, true);
    } else {
        g_autofree char* text = g_strdup_printf("%d of %d will be renamed", changes, m->n);
        set_status(m, text, false);
    }
    m->valid = problems == 0 && changes > 0;
    gtk_dialog_set_response_sensitive(GTK_DIALOG(m->dialog), GTK_RESPONSE_ACCEPT, m->valid);
}

/* Picks the included items and refills the preview list with them. */
static void apply_include(Many* m)
{
    m->n = 0;
    gtk_list_store_clear(m->store);
    for (int i = 0; i < m->req->n; i++) {
        bool dir = m->req->is_dir[i];
        if ((m->include == SFE_RENAME_FILES && dir) || (m->include == SFE_RENAME_FOLDERS && !dir))
            continue;
        m->files[m->n] = m->req->files[i];
        m->is_dir[m->n] = dir;
        m->n++;
        g_autofree char* name = g_file_get_basename(m->req->files[i]);
        g_autofree char* display = g_filename_display_name(name);
        gtk_list_store_insert_with_values(m->store, NULL, -1, COL_OLD, display, COL_NEW, display, -1);
    }
    update_preview(m);
}

static void on_include_toggled(GtkToggleButton* button, gpointer data)
{
    if (!gtk_toggle_button_get_active(button))
        return; /* the button that was switched off */
    Many* m = data;
    m->include = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "sfe-include"));
    apply_include(m);
}

static GtkWidget* build_include_switch(Many* m)
{
    static const char* const labels[] = { "All", "Files", "Folders" };
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget* first = NULL;

    gtk_style_context_add_class(gtk_widget_get_style_context(box), GTK_STYLE_CLASS_LINKED);
    gtk_style_context_add_class(gtk_widget_get_style_context(box), "sfe-include");
    gtk_widget_set_halign(box, GTK_ALIGN_START);
    for (int i = 0; i < 3; i++) {
        GtkWidget* b = gtk_radio_button_new_with_label_from_widget(first ? GTK_RADIO_BUTTON(first) : NULL, labels[i]);
        gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(b), FALSE); /* a segmented button, no dot */
        g_object_set_data(G_OBJECT(b), "sfe-include", GINT_TO_POINTER(i));
        if ((int)m->include == i) /* (before the handler: the preview does not exist yet) */
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b), TRUE);
        g_signal_connect(b, "toggled", G_CALLBACK(on_include_toggled), m);
        gtk_box_pack_start(GTK_BOX(box), b, FALSE, FALSE, 0);
        first = first ? first : b;
    }
    return box;
}

static void on_input_changed(GtkWidget* widget, gpointer data)
{
    (void)widget;
    update_preview(data);
}

static void on_many_response(GtkDialog* dialog, int response, gpointer data)
{
    Many* m = data;

    if (response == GTK_RESPONSE_ACCEPT) {
        update_preview(m); /* the disk may have changed since the last keystroke */
        if (!m->valid || !perform(GTK_WIDGET(dialog), m->req, m->files, m->names, m->n))
            return; /* keep the dialog open */
    }
    gtk_widget_destroy(GTK_WIDGET(dialog)); /* frees m */
}

static GtkWidget* labeled(GtkGrid* grid, int row, const char* text, GtkWidget* widget)
{
    GtkWidget* label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 1);
    gtk_style_context_add_class(gtk_widget_get_style_context(label), "sfe-placeholder");
    gtk_widget_set_hexpand(widget, TRUE);
    gtk_grid_attach(grid, label, 0, row, 1, 1);
    gtk_grid_attach(grid, widget, 1, row, 1, 1);
    return widget;
}

static GtkWidget* build_preview(Many* m)
{
    GtkWidget* view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(m->store));
    GtkCellRenderer* old = gtk_cell_renderer_text_new();
    GtkCellRenderer* new = gtk_cell_renderer_text_new();
    g_object_set(old, "ellipsize", PANGO_ELLIPSIZE_MIDDLE, "ypad", 3, NULL);
    g_object_set(new, "ellipsize", PANGO_ELLIPSIZE_MIDDLE, "ypad", 3, NULL);

    GtkTreeViewColumn* c1 = gtk_tree_view_column_new_with_attributes("Name", old, "text", COL_OLD, NULL);
    GtkTreeViewColumn* c2 = gtk_tree_view_column_new_with_attributes("New name", new, "markup", COL_NEW, NULL);
    gtk_tree_view_column_set_expand(c1, TRUE);
    gtk_tree_view_column_set_expand(c2, TRUE);
    gtk_tree_view_column_set_resizable(c1, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), c1);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), c2);
    gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(view)), GTK_SELECTION_NONE);

    GtkWidget* sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(sw), 220);
    gtk_container_add(GTK_CONTAINER(sw), view);
    gtk_widget_set_vexpand(sw, TRUE);
    return sw;
}

void sfe_rename_many(GtkWidget* parent, GList* files, SfeRenameInclude include, SfeRenamedFunc done,
    gpointer data, GDestroyNotify destroy)
{
    Many* m = g_new0(Many, 1);
    m->req = request_new(files, done, data, destroy);
    m->files = g_new0(GFile*, MAX(m->req->n, 1));
    m->is_dir = g_new0(gboolean, MAX(m->req->n, 1));
    m->dialog = sfe_dialog_new(parent, "Rename Files");
    m->store = gtk_list_store_new(N_COLS, G_TYPE_STRING, G_TYPE_STRING);
    g_object_set_data_full(G_OBJECT(m->dialog), "sfe-rename", m, many_free);

    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    int row = 0;

    m->include = include == SFE_RENAME_NO_SWITCH ? SFE_RENAME_ALL : include;
    if (include != SFE_RENAME_NO_SWITCH)
        labeled(GTK_GRID(grid), row++, "Include", build_include_switch(m));

    m->template_ = labeled(GTK_GRID(grid), row++, "New name", gtk_entry_new());
    gtk_entry_set_text(GTK_ENTRY(m->template_), "{name}{ext}");
    GtkWidget* hint = gtk_label_new(TEMPLATE_HINT);
    gtk_label_set_xalign(GTK_LABEL(hint), 0);
    gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(hint), "sfe-placeholder");
    gtk_style_context_add_class(gtk_widget_get_style_context(hint), "sfe-small");
    gtk_grid_attach(GTK_GRID(grid), hint, 1, row++, 1, 1);

    m->find = labeled(GTK_GRID(grid), row++, "Find", gtk_entry_new());
    m->replace = labeled(GTK_GRID(grid), row++, "Replace with", gtk_entry_new());

    GtkWidget* options = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    m->regex = gtk_check_button_new_with_label("Regular expression");
    m->match_case = gtk_check_button_new_with_label("Match case");
    gtk_box_pack_start(GTK_BOX(options), m->regex, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(options), m->match_case, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(grid), options, 1, row++, 1, 1);

    m->case_mode = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(m->case_mode), "Keep");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(m->case_mode), "lowercase");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(m->case_mode), "UPPERCASE");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(m->case_mode), "Title Case");
    gtk_combo_box_set_active(GTK_COMBO_BOX(m->case_mode), SFE_CASE_KEEP);
    sfe_style_combo_box(m->case_mode);
    labeled(GTK_GRID(grid), row++, "Case", m->case_mode);

    GtkWidget* counter = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    m->start = gtk_spin_button_new_with_range(-1000000, 1000000, 1);
    m->step = gtk_spin_button_new_with_range(-1000, 1000, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(m->start), 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(m->step), 1);
    gtk_box_pack_start(GTK_BOX(counter), gtk_label_new("starts at"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(counter), m->start, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(counter), gtk_label_new("step"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(counter), m->step, FALSE, FALSE, 0);
    labeled(GTK_GRID(grid), row++, "Counter", counter);

    m->status = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(m->status), 0);

    GtkWidget* area = gtk_dialog_get_content_area(GTK_DIALOG(m->dialog));
    gtk_container_set_border_width(GTK_CONTAINER(area), 16);
    gtk_box_set_spacing(GTK_BOX(area), 12);
    gtk_box_pack_start(GTK_BOX(area), grid, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(area), build_preview(m), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(area), m->status, FALSE, FALSE, 0);

    gtk_dialog_add_button(GTK_DIALOG(m->dialog), "Cancel", GTK_RESPONSE_CANCEL);
    GtkWidget* ok = gtk_dialog_add_button(GTK_DIALOG(m->dialog), "Rename", GTK_RESPONSE_ACCEPT);
    gtk_style_context_add_class(gtk_widget_get_style_context(ok), "suggested-action");
    gtk_window_set_default_size(GTK_WINDOW(m->dialog), 640, 560);
    gtk_window_set_modal(GTK_WINDOW(m->dialog), TRUE);

    GtkWidget* inputs[] = { m->template_, m->find, m->replace };
    for (size_t i = 0; i < G_N_ELEMENTS(inputs); i++) {
        g_signal_connect(inputs[i], "changed", G_CALLBACK(on_input_changed), m);
        gtk_entry_set_activates_default(GTK_ENTRY(inputs[i]), TRUE);
    }
    g_signal_connect(m->regex, "toggled", G_CALLBACK(on_input_changed), m);
    g_signal_connect(m->match_case, "toggled", G_CALLBACK(on_input_changed), m);
    g_signal_connect(m->case_mode, "changed", G_CALLBACK(on_input_changed), m);
    g_signal_connect(m->start, "value-changed", G_CALLBACK(on_input_changed), m);
    g_signal_connect(m->step, "value-changed", G_CALLBACK(on_input_changed), m);
    g_signal_connect(m->dialog, "response", G_CALLBACK(on_many_response), m);
    gtk_dialog_set_default_response(GTK_DIALOG(m->dialog), GTK_RESPONSE_ACCEPT);

    apply_include(m); /* fills the preview */
    gtk_widget_show_all(m->dialog);
    gtk_widget_grab_focus(m->find);
}
