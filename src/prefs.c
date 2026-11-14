#include "prefs.h"

#include "app.h"
#include "dialogs.h"
#include "opener.h"
#include <string.h>

#define DIALOG_KEY "sfe-preferences"

/* Sections shown, in order, with their titles. ("preview" waits for the
 * thumbnails milestone: its options do nothing yet.) */
static const struct {
    const char* section;
    const char* title;
} sections[] = {
    { "general", "General" },
    { "sidebar", "Sidebar" },
};

/* ------------------------------------------------------------------------ */
/* Applying a change                                                         */
/* ------------------------------------------------------------------------ */

/* Copies the current config, lets `set` change one field, saves it. */
static void apply(const SfeOption* opt, void (*set)(void* field, gpointer value), gpointer value)
{
    SfeConfig* cfg = sfe_application_dup_config(sfe_app());
    set(sfe_config_field(cfg, opt), value);
    sfe_application_set_config(sfe_app(), cfg);
}

static void set_bool(void* field, gpointer value) { *(bool*)field = GPOINTER_TO_INT(value); }
static void set_int(void* field, gpointer value) { *(int*)field = GPOINTER_TO_INT(value); }
static void set_double(void* field, gpointer value) { *(double*)field = *(double*)value; }

static void set_string(void* field, gpointer value)
{
    g_free(*(char**)field);
    *(char**)field = g_strdup(value);
}

static const SfeOption* option_of(GObject* widget)
{
    return g_object_get_data(widget, "sfe-option");
}

static void on_switch(GObject* sw, GParamSpec* pspec, gpointer data)
{
    (void)pspec, (void)data;
    apply(option_of(sw), set_bool, GINT_TO_POINTER(gtk_switch_get_active(GTK_SWITCH(sw))));
}

static void on_spin(GtkSpinButton* spin, gpointer data)
{
    (void)data;
    const SfeOption* opt = option_of(G_OBJECT(spin));
    if (opt->type == SFE_OPT_INT) {
        apply(opt, set_int, GINT_TO_POINTER(gtk_spin_button_get_value_as_int(spin)));
    } else {
        double value = gtk_spin_button_get_value(spin);
        apply(opt, set_double, &value);
    }
}

static void on_combo(GtkComboBox* combo, gpointer data)
{
    (void)data;
    apply(option_of(G_OBJECT(combo)), set_int, GINT_TO_POINTER(gtk_combo_box_get_active(combo)));
}

/* Text is saved on Enter or when leaving the field, not on every key. */
static void commit_entry(GtkEntry* entry)
{
    const SfeOption* opt = option_of(G_OBJECT(entry));
    const char* text = gtk_entry_get_text(entry);
    if (g_strcmp0(*(char* const*)sfe_config_field(sfe_config(), opt), text) != 0)
        apply(opt, set_string, (gpointer)text);
}

static void on_entry_activate(GtkEntry* entry, gpointer data)
{
    (void)data;
    commit_entry(entry);
}

static gboolean on_entry_focus_out(GtkWidget* entry, GdkEvent* event, gpointer data)
{
    (void)event, (void)data;
    commit_entry(GTK_ENTRY(entry));
    return FALSE;
}

/* ------------------------------------------------------------------------ */
/* Building the form                                                         */
/* ------------------------------------------------------------------------ */

static GtkWidget* widget_for(const SfeOption* opt)
{
    const void* field = sfe_config_field(sfe_config(), opt);
    GtkWidget* w = NULL;

    switch (opt->type) {
    case SFE_OPT_BOOL:
        w = gtk_switch_new();
        gtk_switch_set_active(GTK_SWITCH(w), *(const bool*)field);
        gtk_widget_set_halign(w, GTK_ALIGN_END);
        g_signal_connect(w, "notify::active", G_CALLBACK(on_switch), NULL);
        break;
    case SFE_OPT_INT:
    case SFE_OPT_DOUBLE: {
        bool is_int = opt->type == SFE_OPT_INT;
        w = gtk_spin_button_new_with_range(opt->min, opt->max, is_int ? 1 : 0.05);
        gtk_spin_button_set_digits(GTK_SPIN_BUTTON(w), is_int ? 0 : 2);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(w), is_int ? *(const int*)field : *(const double*)field);
        gtk_widget_set_halign(w, GTK_ALIGN_END);
        g_signal_connect(w, "value-changed", G_CALLBACK(on_spin), NULL);
        break;
    }
    case SFE_OPT_ENUM:
        w = gtk_combo_box_text_new();
        for (int i = 0; opt->choices[i] != NULL; i++)
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(w), opt->choices[i]);
        gtk_combo_box_set_active(GTK_COMBO_BOX(w), *(const int*)field);
        gtk_widget_set_halign(w, GTK_ALIGN_END);
        g_signal_connect(w, "changed", G_CALLBACK(on_combo), NULL);
        break;
    case SFE_OPT_STRING:
        w = gtk_entry_new();
        gtk_entry_set_text(GTK_ENTRY(w), *(char* const*)field ? *(char* const*)field : "");
        gtk_entry_set_width_chars(GTK_ENTRY(w), 24);
        g_signal_connect(w, "activate", G_CALLBACK(on_entry_activate), NULL);
        g_signal_connect(w, "focus-out-event", G_CALLBACK(on_entry_focus_out), NULL);
        break;
    }
    g_object_set_data(G_OBJECT(w), "sfe-option", (gpointer)opt);
    return w;
}

static void add_title(GtkGrid* grid, int* row, const char* text)
{
    GtkWidget* title = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(title), "sfe-sidebar-title");
    gtk_widget_set_margin_top(title, *row == 0 ? 0 : 12);
    gtk_grid_attach(grid, title, 0, (*row)++, 2, 1);
}

static void on_open_config(GtkButton* button, gpointer data)
{
    (void)data;
    g_autofree char* path = sfe_config_default_path();
    g_autoptr(GFile) file = g_file_new_for_path(path);
    g_autoptr(GError) error = NULL;
    if (!sfe_open_file(sfe_config(), file, &error))
        sfe_show_error(GTK_WIDGET(button), "Cannot open the config file", error->message);
}

static GtkWidget* build_form(void)
{
    GtkWidget* grid = gtk_grid_new();
    size_t n = 0;
    const SfeOption* opts = sfe_config_options(&n);
    int row = 0;

    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 24);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 18);

    for (size_t s = 0; s < G_N_ELEMENTS(sections); s++) {
        add_title(GTK_GRID(grid), &row, sections[s].title);
        for (size_t i = 0; i < n; i++) {
            if (strcmp(opts[i].section, sections[s].section) != 0)
                continue;
            GtkWidget* label = gtk_label_new(opts[i].label ? opts[i].label : opts[i].key);
            GtkWidget* widget = widget_for(&opts[i]);
            g_autofree char* tip = g_strdup_printf("%s\n(%s.%s in config.toml)", opts[i].help, opts[i].section, opts[i].key);
            gtk_label_set_xalign(GTK_LABEL(label), 0);
            gtk_widget_set_hexpand(label, TRUE);
            gtk_widget_set_tooltip_text(label, tip);
            gtk_widget_set_valign(widget, GTK_ALIGN_CENTER);
            gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
            gtk_grid_attach(GTK_GRID(grid), widget, 1, row++, 1, 1);
        }
    }

    add_title(GTK_GRID(grid), &row, "More");
    GtkWidget* note = gtk_label_new("Bookmarks, programs per file type ([open]) and custom "
                                    "commands ([[actions]]) are edited in config.toml.");
    gtk_label_set_xalign(GTK_LABEL(note), 0);
    gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(note), 40);
    gtk_style_context_add_class(gtk_widget_get_style_context(note), "sfe-placeholder");
    GtkWidget* open = gtk_button_new_with_label("Open config.toml");
    gtk_widget_set_valign(open, GTK_ALIGN_CENTER);
    g_signal_connect(open, "clicked", G_CALLBACK(on_open_config), NULL);
    gtk_grid_attach(GTK_GRID(grid), note, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), open, 1, row++, 1, 1);
    return grid;
}

/* ------------------------------------------------------------------------ */

static void on_dialog_destroy(GtkWidget* dialog, gpointer window)
{
    (void)dialog;
    g_object_set_data(G_OBJECT(window), DIALOG_KEY, NULL);
}

void sfe_show_preferences(GtkWidget* parent)
{
    GtkWidget* window = gtk_widget_get_toplevel(parent);
    GtkWidget* dialog = g_object_get_data(G_OBJECT(window), DIALOG_KEY);

    if (dialog != NULL) {
        gtk_window_present(GTK_WINDOW(dialog));
        return;
    }
    dialog = sfe_dialog_new(parent, "Preferences");
    GtkWidget* scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 640);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 520, -1);
    gtk_container_add(GTK_CONTAINER(scroll), build_form());
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), scroll, TRUE, TRUE, 0);
    gtk_dialog_add_button(GTK_DIALOG(dialog), "Close", GTK_RESPONSE_CLOSE);
    g_signal_connect(dialog, "response", G_CALLBACK(gtk_widget_destroy), NULL);
    g_signal_connect(dialog, "destroy", G_CALLBACK(on_dialog_destroy), window);
    g_object_set_data(G_OBJECT(window), DIALOG_KEY, dialog);
    gtk_widget_show_all(dialog);
}
