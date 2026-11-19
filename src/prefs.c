#include "prefs.h"

#include "app.h"
#include "defaultfm.h"
#include "dialogs.h"
#include "icons.h"
#include "theme.h"
#include "opener.h"
#include "portalconf.h"
#include <string.h>

#define DIALOG_KEY "sfe-preferences"

#include "keymap.h"

/* Our own saves also emit "config-changed"; the list pages ignore those. */
static bool applying;

/* ------------------------------------------------------------------------ */
/* Applying a change                                                         */
/* ------------------------------------------------------------------------ */

static void save_config(SfeConfig* cfg)
{
    applying = true;
    sfe_application_set_config(sfe_app(), cfg); /* takes cfg */
    applying = false;
}

/* Copies the current config, lets `set` change one field, saves it. */
static void apply(const SfeOption* opt, void (*set)(void* field, gpointer value), gpointer value)
{
    SfeConfig* cfg = sfe_application_dup_config(sfe_app());
    set(sfe_config_field(cfg, opt), value);
    save_config(cfg);
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
        sfe_style_combo_box(w);
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

static void on_open_config(GtkButton* button, gpointer data)
{
    (void)data;
    g_autofree char* path = sfe_config_default_path();
    g_autoptr(GFile) file = g_file_new_for_path(path);
    g_autoptr(GError) error = NULL;
    if (!sfe_open_file(sfe_config(), file, &error))
        sfe_show_error(GTK_WIDGET(button), "Cannot open the config file", error->message);
}

static void show_default_state(GtkWidget* button)
{
    bool set = sfe_default_fm_is_set();
    gtk_button_set_label(GTK_BUTTON(button), set ? "sfe is the default" : "Make Default");
    gtk_widget_set_sensitive(button, !set && sfe_default_fm_available());
    if (!sfe_default_fm_available())
        gtk_widget_set_tooltip_text(button, "Install sfe (meson install or the package) to use this.");
}

static void on_make_default(GtkButton* button, gpointer data)
{
    (void)data;
    g_autoptr(GError) error = NULL;
    if (!sfe_default_fm_set(&error))
        sfe_show_error(GTK_WIDGET(button), "Cannot make sfe the default file manager", error->message);
    show_default_state(GTK_WIDGET(button));
}

/* ------------------------------------------------------------------------ */
/* Pages                                                                     */
/* ------------------------------------------------------------------------ */

static GtkWidget* page_grid(void)
{
    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 24);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 18);
    return grid;
}

static void add_row(GtkGrid* grid, int* row, GtkWidget* label, GtkWidget* widget)
{
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_widget_set_valign(widget, GTK_ALIGN_CENTER);
    gtk_grid_attach(grid, label, 0, *row, 1, 1);
    gtk_grid_attach(grid, widget, 1, (*row)++, 1, 1);
}

static GtkWidget* note(const char* text)
{
    GtkWidget* label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 60);
    gtk_style_context_add_class(gtk_widget_get_style_context(label), "sfe-placeholder");
    return label;
}

/* ---- File icons: one card per style, previewing it ---------------------- */

static const char* const preview_types[] = { "inode/directory", "text/plain", "image/png", "audio/mpeg", "application/pdf" };
static const char* const style_titles[] = { "System", "Tinted", "Symbolic" };
static const char* const style_notes[] = {
    "The icon theme as it is",
    "Folders in the theme's accent colour",
    "One colour, from the theme",
};

/* Draws the preview icons of every card again (the theme's colours changed). */
static void fill_previews(GtkWidget* chooser)
{
    int size = sfe_theme_scaled(32);
    int scale = gtk_widget_get_scale_factor(chooser);
    g_autoptr(GList) cards = gtk_container_get_children(GTK_CONTAINER(chooser));
    int style = 0;
    for (GList* l = cards; l != NULL; l = l->next, style++) {
        GtkWidget* strip = g_object_get_data(G_OBJECT(l->data), "sfe-strip");
        g_autoptr(GList) old = gtk_container_get_children(GTK_CONTAINER(strip));
        for (GList* o = old; o != NULL; o = o->next)
            gtk_widget_destroy(o->data);
        for (size_t i = 0; i < G_N_ELEMENTS(preview_types); i++) {
            g_autoptr(GdkPixbuf) pixbuf = sfe_icon_preview(preview_types[i], style, size, scale);
            cairo_surface_t* surface = gdk_cairo_surface_create_from_pixbuf(pixbuf, scale, NULL);
            GtkWidget* image = gtk_image_new_from_surface(surface);
            cairo_surface_destroy(surface);
            gtk_box_pack_start(GTK_BOX(strip), image, FALSE, FALSE, 0);
            gtk_widget_show(image);
        }
    }
}

static void on_icons_changed(GtkIconTheme* theme, gpointer chooser)
{
    (void)theme;
    fill_previews(chooser);
}

static void on_style_toggled(GtkToggleButton* card, gpointer data)
{
    (void)data;
    if (!gtk_toggle_button_get_active(card))
        return;
    const SfeOption* opt = option_of(G_OBJECT(card));
    int style = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(card), "sfe-style"));
    if (*(const int*)sfe_config_field(sfe_config(), opt) != style)
        apply(opt, set_int, GINT_TO_POINTER(style));
}

static GtkWidget* file_icons_chooser(const SfeOption* opt)
{
    GtkWidget* chooser = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_set_homogeneous(GTK_BOX(chooser), TRUE);
    int current = *(const int*)sfe_config_field(sfe_config(), opt);
    GtkWidget* first = NULL;
    for (int style = 0; opt->choices[style] != NULL; style++) {
        GtkWidget* card = first ? gtk_radio_button_new_from_widget(GTK_RADIO_BUTTON(first)) : gtk_radio_button_new(NULL);
        first = first ? first : card;
        gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(card), FALSE); /* a card, not a dot */
        gtk_style_context_add_class(gtk_widget_get_style_context(card), "sfe-icon-choice");
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        GtkWidget* strip = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        gtk_widget_set_halign(strip, GTK_ALIGN_CENTER);
        GtkWidget* title = gtk_label_new(style_titles[style]);
        GtkWidget* note = gtk_label_new(style_notes[style]);
        gtk_style_context_add_class(gtk_widget_get_style_context(title), "sfe-icon-choice-title");
        gtk_style_context_add_class(gtk_widget_get_style_context(note), "sfe-placeholder");
        gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
        gtk_label_set_justify(GTK_LABEL(note), GTK_JUSTIFY_CENTER);
        gtk_label_set_max_width_chars(GTK_LABEL(note), 18);
        gtk_box_pack_start(GTK_BOX(box), strip, FALSE, FALSE, 4);
        gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), note, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(card), box);
        g_object_set_data(G_OBJECT(card), "sfe-strip", strip);
        g_object_set_data(G_OBJECT(card), "sfe-option", (gpointer)opt);
        g_object_set_data(G_OBJECT(card), "sfe-style", GINT_TO_POINTER(style));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(card), style == current);
        g_signal_connect(card, "toggled", G_CALLBACK(on_style_toggled), NULL);
        gtk_box_pack_start(GTK_BOX(chooser), card, TRUE, TRUE, 0);
    }
    fill_previews(chooser);
    /* Colours regenerated (Noctalia) while Settings is open: previews follow. */
    g_signal_connect_object(gtk_icon_theme_get_default(), "changed", G_CALLBACK(on_icons_changed), chooser, 0);
    return chooser;
}

/* "Looks ───────": a group of options. */
static GtkWidget* group_heading(const char* title, bool first)
{
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget* label = gtk_label_new(title);
    GtkWidget* line = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_style_context_add_class(gtk_widget_get_style_context(box), "sfe-settings-group");
    gtk_widget_set_valign(line, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), line, TRUE, TRUE, 0);
    gtk_widget_set_margin_top(box, first ? 0 : 14);
    return box;
}

/* The options of one config section, built from the option table. */
static GtkWidget* options_page(const char* section, const char* text)
{
    GtkWidget* grid = page_grid();
    size_t n = 0;
    const SfeOption* opts = sfe_config_options(&n);
    int row = 0;

    if (text != NULL)
        gtk_grid_attach(GTK_GRID(grid), note(text), 0, row++, 2, 1);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(opts[i].section, section) != 0)
            continue;
        if (opts[i].group != NULL) {
            GtkWidget* heading = group_heading(opts[i].group, row == 0);
            gtk_grid_attach(GTK_GRID(grid), heading, 0, row++, 2, 1);
        }
        GtkWidget* label = gtk_label_new(opts[i].label ? opts[i].label : opts[i].key);
        g_autofree char* tip = g_strdup_printf("%s\n(%s.%s in config.toml)", opts[i].help, opts[i].section, opts[i].key);
        gtk_widget_set_tooltip_text(label, tip);
        if (strcmp(opts[i].key, "file_icons") == 0) { /* previews need the whole width */
            gtk_label_set_xalign(GTK_LABEL(label), 0);
            gtk_grid_attach(GTK_GRID(grid), label, 0, row++, 2, 1);
            gtk_grid_attach(GTK_GRID(grid), file_icons_chooser(&opts[i]), 0, row++, 2, 1);
            continue;
        }
        add_row(GTK_GRID(grid), &row, label, widget_for(&opts[i]));
    }
    return grid;
}

/* "Use sfe for Open / Save dialogs": the portal's FileChooser line. */
static bool reverting; /* our own correction of the switch */

static void on_file_dialogs(GObject* sw, GParamSpec* pspec, gpointer data)
{
    (void)pspec, (void)data;
    if (reverting)
        return;
    bool use = gtk_switch_get_active(GTK_SWITCH(sw));
    g_autofree char* desktop = sfe_portal_desktop();
    g_autofree char* path = sfe_portal_conf_path(desktop);
    g_autofree char* seed = sfe_portal_conf_seed(desktop);
    g_autoptr(GError) error = NULL;
    if (!sfe_portal_conf_set(path, seed, use, &error)) {
        sfe_show_error(GTK_WIDGET(sw), "Cannot change the file dialogs", error->message);
        reverting = true;
        gtk_switch_set_active(GTK_SWITCH(sw), !use);
        reverting = false;
        return;
    }
    sfe_portal_restart(); /* it reads the file again on the next dialog */
}

static GtkWidget* file_dialogs_switch(void)
{
    GtkWidget* sw = gtk_switch_new();
    g_autofree char* desktop = sfe_portal_desktop();
    g_autofree char* path = sfe_portal_conf_path(desktop);
    gtk_switch_set_active(GTK_SWITCH(sw), sfe_portal_conf_uses_sfe(path));
    gtk_widget_set_halign(sw, GTK_ALIGN_END);
    if (!sfe_portal_installed()) {
        gtk_widget_set_sensitive(sw, FALSE);
        gtk_widget_set_tooltip_text(sw, "Install sfe (meson install or the package) to use this.");
    } else {
        g_autofree char* tip = g_strdup_printf("Sets org.freedesktop.impl.portal.FileChooser=sfe in %s", path);
        gtk_widget_set_tooltip_text(sw, tip);
    }
    g_signal_connect(sw, "notify::active", G_CALLBACK(on_file_dialogs), NULL);
    return sw;
}

static GtkWidget* system_page(void)
{
    GtkWidget* grid = page_grid();
    int row = 0;

    GtkWidget* fm_button = gtk_button_new();
    GtkWidget* fm_label = gtk_label_new("Default file manager");
    gtk_widget_set_tooltip_text(fm_label, "Open folders from other applications (and xdg-open) in sfe");
    show_default_state(fm_button);
    g_signal_connect(fm_button, "clicked", G_CALLBACK(on_make_default), NULL);
    add_row(GTK_GRID(grid), &row, fm_label, fm_button);

    GtkWidget* dialogs_label = gtk_label_new("Use sfe for Open / Save dialogs");
    gtk_widget_set_tooltip_text(dialogs_label, "Other programs' file dialogs (browsers, Flatpak apps) open in sfe");
    add_row(GTK_GRID(grid), &row, dialogs_label, file_dialogs_switch());
    gtk_grid_attach(GTK_GRID(grid),
        note("Programs that ask the desktop portal for a file dialog get sfe instead. Firefox and Zen ask "
             "it only with about:config → widget.use-xdg-desktop-portal.file-picker set to 1."),
        0, row++, 2, 1);

    GtkWidget* open = gtk_button_new_with_label("Open config.toml");
    g_signal_connect(open, "clicked", G_CALLBACK(on_open_config), NULL);
    add_row(GTK_GRID(grid), &row, gtk_label_new("Settings file"), open);
    gtk_grid_attach(GTK_GRID(grid),
        note("Everything here is saved in ~/.config/sfe/config.toml, which can also be edited "
             "by hand; sfe reloads it when it changes."),
        0, row++, 2, 1);
    return grid;
}

/* A list page: a tree view over `store` with buttons under it. */
static GtkWidget* list_page(const char* text, GtkWidget* view, GtkWidget** buttons, int n_buttons)
{
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 18);
    gtk_box_pack_start(GTK_BOX(box), note(text), FALSE, FALSE, 0);
    GtkWidget* sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
    gtk_style_context_add_class(gtk_widget_get_style_context(sw), "frame");
    gtk_container_add(GTK_CONTAINER(sw), view);
    gtk_box_pack_start(GTK_BOX(box), sw, TRUE, TRUE, 0);
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    for (int i = 0; i < n_buttons; i++)
        gtk_box_pack_start(GTK_BOX(row), buttons[i], FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    return box;
}

static GtkTreeViewColumn* text_column(GtkWidget* view, const char* title, int col, bool editable, GCallback edited,
    gpointer data)
{
    GtkCellRenderer* cell = gtk_cell_renderer_text_new();
    g_object_set(cell, "editable", editable, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    if (edited != NULL)
        g_signal_connect(cell, "edited", edited, data);
    GtkTreeViewColumn* column = gtk_tree_view_column_new_with_attributes(title, cell, "text", col, NULL);
    gtk_tree_view_column_set_resizable(column, TRUE);
    gtk_tree_view_column_set_expand(column, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), column);
    return column;
}

static bool selected_iter(GtkWidget* view, GtkTreeIter* iter)
{
    return gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(view)), NULL, iter);
}

/* ---- Keys ---------------------------------------------------------------- */

enum {
    KEY_MODE, /* "normal" / "visual" */
    KEY_SEQUENCE,
    KEY_COMMAND, /* "" when unbound */
    KEY_HELP,
    KEY_VISUAL, /* gboolean */
    KEY_CHANGED, /* gboolean: differs from the default */
    KEY_WEIGHT, /* bold when changed */
    KEY_DEFAULT, /* the built-in command, or NULL */
    KEY_N,
};

/* "<c-w>v" and "<C-w>v" are the same keys. */
static char* canonical(const char* sequence)
{
    g_auto(GStrv) tokens = sfe_keymap_parse(sequence, NULL);
    return tokens ? g_strjoinv(" ", tokens) : g_strdup(sequence);
}

static const SfeKeyOverride* find_override(const SfeConfig* cfg, const char* sequence, bool visual)
{
    g_autofree char* want = canonical(sequence);
    const SfeKeyOverride* found = NULL;
    for (guint i = 0; i < cfg->keys->len; i++) { /* the last one wins */
        const SfeKeyOverride* k = g_ptr_array_index(cfg->keys, i);
        g_autofree char* have = canonical(k->sequence);
        if (k->visual == visual && strcmp(have, want) == 0)
            found = k;
    }
    return found;
}

static void add_key_row(GtkListStore* store, const char* sequence, const char* command, const char* help,
    bool visual, const char* default_command)
{
    bool changed = g_strcmp0(command, default_command) != 0;
    gtk_list_store_insert_with_values(store, NULL, -1, KEY_MODE, visual ? "visual" : "normal", KEY_SEQUENCE, sequence,
        KEY_COMMAND, command, KEY_HELP, changed ? (*command ? "your binding" : "unbound") : help, KEY_VISUAL, visual,
        KEY_CHANGED, changed, KEY_WEIGHT, changed ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL, KEY_DEFAULT,
        default_command, -1);
}

/* The built-in keys with the config's changes, then the user's own keys. */
static void fill_keys(GtkListStore* store)
{
    const SfeConfig* cfg = sfe_config();
    gtk_list_store_clear(store);
    for (int visual = 0; visual <= 1; visual++) {
        size_t n = 0;
        const SfeKeyBinding* d = visual ? sfe_keymap_visual_defaults(&n) : sfe_keymap_defaults(&n);
        for (size_t i = 0; i < n; i++) {
            const SfeKeyOverride* k = find_override(cfg, d[i].sequence, visual);
            add_key_row(store, d[i].sequence, k ? k->command : d[i].command, d[i].help, visual, d[i].command);
        }
    }
    for (guint i = 0; i < cfg->keys->len; i++) {
        const SfeKeyOverride* k = g_ptr_array_index(cfg->keys, i);
        size_t n = 0;
        const SfeKeyBinding* d = k->visual ? sfe_keymap_visual_defaults(&n) : sfe_keymap_defaults(&n);
        bool builtin = false;
        g_autofree char* have = canonical(k->sequence);
        for (size_t j = 0; j < n && !builtin; j++) {
            g_autofree char* def = canonical(d[j].sequence);
            builtin = strcmp(have, def) == 0;
        }
        if (!builtin && find_override(cfg, k->sequence, k->visual) == k)
            add_key_row(store, k->sequence, k->command, NULL, k->visual, NULL);
    }
}

/* Sets (or with NULL drops) the config's entry for these keys. */
static void save_key(const char* sequence, bool visual, const char* command)
{
    SfeConfig* cfg = sfe_application_dup_config(sfe_app());
    g_autofree char* want = canonical(sequence);
    for (guint i = cfg->keys->len; i-- > 0;) {
        SfeKeyOverride* k = g_ptr_array_index(cfg->keys, i);
        g_autofree char* have = canonical(k->sequence);
        if (k->visual == visual && strcmp(have, want) == 0)
            g_ptr_array_remove_index(cfg->keys, i);
    }
    if (command != NULL)
        sfe_config_add_key(cfg, sequence, command, visual);
    save_config(cfg);
}

static void on_key_command_edited(GtkCellRendererText* cell, char* path, char* text, gpointer data)
{
    (void)cell;
    GtkListStore* store = data;
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(store), &iter, path))
        return;
    g_autofree char* sequence = NULL;
    g_autofree char* fallback = NULL;
    gboolean visual = FALSE;
    gtk_tree_model_get(GTK_TREE_MODEL(store), &iter, KEY_SEQUENCE, &sequence, KEY_VISUAL, &visual, KEY_DEFAULT,
        &fallback, -1);
    g_strstrip(text);
    /* Back to the built-in command: no entry needed. */
    save_key(sequence, visual, fallback != NULL && strcmp(text, fallback) == 0 ? NULL : text);
    fill_keys(store);
}

static void on_key_reset(GtkButton* button, gpointer view)
{
    (void)button;
    GtkTreeIter iter;
    if (!selected_iter(view, &iter))
        return;
    GtkTreeModel* model = gtk_tree_view_get_model(view);
    g_autofree char* sequence = NULL;
    gboolean visual = FALSE;
    gtk_tree_model_get(model, &iter, KEY_SEQUENCE, &sequence, KEY_VISUAL, &visual, -1);
    save_key(sequence, visual, NULL); /* the default again, or gone if it was the user's */
    fill_keys(GTK_LIST_STORE(model));
}

static void on_key_unbind(GtkButton* button, gpointer view)
{
    (void)button;
    GtkTreeIter iter;
    if (!selected_iter(view, &iter))
        return;
    GtkTreeModel* model = gtk_tree_view_get_model(view);
    g_autofree char* sequence = NULL;
    g_autofree char* fallback = NULL;
    gboolean visual = FALSE;
    gtk_tree_model_get(model, &iter, KEY_SEQUENCE, &sequence, KEY_VISUAL, &visual, KEY_DEFAULT, &fallback, -1);
    save_key(sequence, visual, fallback != NULL ? "" : NULL); /* "" switches a built-in key off */
    fill_keys(GTK_LIST_STORE(model));
}

typedef struct {
    GtkWidget* view;
    GtkWidget* keys;
    GtkWidget* command;
    GtkWidget* visual;
    GtkWidget* status;
} AddKey;

static void on_add_key_response(GtkDialog* dialog, int response, gpointer data)
{
    AddKey* a = data;
    if (response == GTK_RESPONSE_ACCEPT) {
        const char* keys = gtk_entry_get_text(GTK_ENTRY(a->keys));
        const char* command = gtk_entry_get_text(GTK_ENTRY(a->command));
        g_autoptr(GError) error = NULL;
        g_auto(GStrv) tokens = sfe_keymap_parse(keys, &error);
        if (tokens == NULL || *command == '\0') {
            gtk_label_set_text(GTK_LABEL(a->status), tokens == NULL ? error->message : "A command is needed.");
            gtk_widget_show(a->status);
            return; /* the dialog stays */
        }
        save_key(keys, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->visual)), command);
        fill_keys(GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(a->view))));
    }
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void free_closure_data(gpointer data, GClosure* closure)
{
    (void)closure;
    g_free(data);
}

static void on_key_add(GtkButton* button, gpointer view)
{
    AddKey* a = g_new0(AddKey, 1);
    a->view = view;
    GtkWidget* dialog = sfe_dialog_new(GTK_WIDGET(button), "Add Key");
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    GtkWidget* grid = page_grid();
    int row = 0;
    a->keys = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(a->keys), "gd   <C-x>   <Space>");
    a->command = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(a->command), "cd ~/Downloads");
    gtk_entry_set_activates_default(GTK_ENTRY(a->command), TRUE);
    a->visual = gtk_check_button_new_with_label("Only in visual mode");
    a->status = gtk_label_new(NULL);
    gtk_style_context_add_class(gtk_widget_get_style_context(a->status), "sfe-error-text");
    gtk_widget_set_no_show_all(a->status, TRUE);
    add_row(GTK_GRID(grid), &row, gtk_label_new("Keys"), a->keys);
    add_row(GTK_GRID(grid), &row, gtk_label_new("Command"), a->command);
    gtk_grid_attach(GTK_GRID(grid), a->visual, 1, row++, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), a->status, 0, row++, 2, 1);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), grid, TRUE, TRUE, 0);
    gtk_dialog_add_button(GTK_DIALOG(dialog), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_add_button(GTK_DIALOG(dialog), "Add", GTK_RESPONSE_ACCEPT);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    g_signal_connect_data(dialog, "response", G_CALLBACK(on_add_key_response), a, free_closure_data, 0);
    gtk_widget_show_all(dialog);
}

static GtkWidget* keys_page(GtkListStore** store_out)
{
    GtkListStore* store = gtk_list_store_new(KEY_N, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_BOOLEAN, G_TYPE_BOOLEAN, G_TYPE_INT, G_TYPE_STRING);
    fill_keys(store);
    GtkWidget* view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);
    GtkTreeViewColumn* columns[] = {
        text_column(view, "Mode", KEY_MODE, false, NULL, NULL),
        text_column(view, "Keys", KEY_SEQUENCE, false, NULL, NULL),
        text_column(view, "Command", KEY_COMMAND, true, G_CALLBACK(on_key_command_edited), store),
        text_column(view, "Does", KEY_HELP, false, NULL, NULL),
    };
    for (int i = 0; i < 2; i++) { /* "normal", "<C-w>v": short, never shortened */
        GList* cells = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(columns[i]));
        g_object_set(cells->data, "ellipsize", PANGO_ELLIPSIZE_NONE, NULL);
        g_list_free(cells);
        gtk_tree_view_column_set_expand(columns[i], FALSE);
    }
    for (size_t i = 0; i < G_N_ELEMENTS(columns); i++) {
        GList* cells = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(columns[i]));
        gtk_tree_view_column_add_attribute(columns[i], cells->data, "weight", KEY_WEIGHT);
        g_list_free(cells);
    }
    gtk_tree_view_set_search_column(GTK_TREE_VIEW(view), KEY_SEQUENCE);

    GtkWidget* add = gtk_button_new_with_label("Add…");
    GtkWidget* unbind = gtk_button_new_with_label("Unbind");
    GtkWidget* reset = gtk_button_new_with_label("Reset");
    gtk_widget_set_tooltip_text(unbind, "Switch the selected key off");
    gtk_widget_set_tooltip_text(reset, "Give the selected key its built-in command back (or remove your own)");
    g_signal_connect(add, "clicked", G_CALLBACK(on_key_add), view);
    g_signal_connect(unbind, "clicked", G_CALLBACK(on_key_unbind), view);
    g_signal_connect(reset, "clicked", G_CALLBACK(on_key_reset), view);
    GtkWidget* buttons[] = { add, unbind, reset };
    *store_out = store;
    return list_page("Normal-mode keys run commands (any command line works, with arguments). Double-click a "
                     "command to change it; bold rows differ from the built-in keys. Visual-mode keys come first "
                     "while selecting with v.",
        view, buttons, G_N_ELEMENTS(buttons));
}

/* ---- Custom commands ------------------------------------------------------ */

enum {
    ACT_NAME,
    ACT_COMMAND,
    ACT_FOR, /* "files" / "folders" / "both" */
    ACT_N,
};

static const char* const kind_names[] = { "files", "folders", "both" };

static const char* kind_name(unsigned applies_to)
{
    return applies_to == SFE_ACTION_FILES ? kind_names[0] : applies_to == SFE_ACTION_DIRECTORIES ? kind_names[1] : kind_names[2];
}

static unsigned kind_value(const char* name)
{
    return g_strcmp0(name, "files") == 0 ? SFE_ACTION_FILES
        : g_strcmp0(name, "folders") == 0 ? SFE_ACTION_DIRECTORIES
                                          : SFE_ACTION_ANY;
}

static void fill_actions(GtkListStore* store)
{
    const SfeConfig* cfg = sfe_config();
    gtk_list_store_clear(store);
    for (guint i = 0; i < cfg->actions->len; i++) {
        const SfeUserAction* a = g_ptr_array_index(cfg->actions, i);
        gtk_list_store_insert_with_values(store, NULL, -1, ACT_NAME, a->name, ACT_COMMAND, a->command, ACT_FOR,
            kind_name(a->applies_to), -1);
    }
}

/* The whole list goes back to the config; rows without a name or a
 * command are not saved yet. */
static void save_actions(GtkTreeModel* model)
{
    SfeConfig* cfg = sfe_application_dup_config(sfe_app());
    g_ptr_array_set_size(cfg->actions, 0);
    GtkTreeIter iter;
    for (bool ok = gtk_tree_model_get_iter_first(model, &iter); ok; ok = gtk_tree_model_iter_next(model, &iter)) {
        g_autofree char* name = NULL;
        g_autofree char* command = NULL;
        g_autofree char* kind = NULL;
        gtk_tree_model_get(model, &iter, ACT_NAME, &name, ACT_COMMAND, &command, ACT_FOR, &kind, -1);
        if (name && *name && command && *command)
            sfe_config_add_user_action(cfg, name, command, kind_value(kind));
    }
    save_config(cfg);
}

static void edit_cell(GtkListStore* store, const char* path, int column, const char* text)
{
    GtkTreeIter iter;
    if (gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(store), &iter, path))
        gtk_list_store_set(store, &iter, column, text, -1);
}

static void on_action_name(GtkCellRendererText* cell, char* path, char* text, gpointer store)
{
    (void)cell;
    edit_cell(store, path, ACT_NAME, g_strstrip(text));
    save_actions(store);
}

static void on_action_command(GtkCellRendererText* cell, char* path, char* text, gpointer store)
{
    (void)cell;
    edit_cell(store, path, ACT_COMMAND, g_strstrip(text));
    save_actions(store);
}

static void on_action_for(GtkCellRendererText* cell, char* path, char* text, gpointer store)
{
    (void)cell;
    edit_cell(store, path, ACT_FOR, text);
    save_actions(store);
}

/* A new row, with its name being edited. */
static void add_and_edit(GtkWidget* view, GtkListStore* store, GtkTreeIter* iter)
{
    GtkTreePath* path = gtk_tree_model_get_path(GTK_TREE_MODEL(store), iter);
    gtk_widget_grab_focus(view);
    gtk_tree_view_set_cursor(GTK_TREE_VIEW(view), path, gtk_tree_view_get_column(GTK_TREE_VIEW(view), 0), TRUE);
    gtk_tree_path_free(path);
}

static void on_action_add(GtkButton* button, gpointer view)
{
    (void)button;
    GtkListStore* store = GTK_LIST_STORE(gtk_tree_view_get_model(view));
    GtkTreeIter iter;
    gtk_list_store_insert_with_values(store, &iter, -1, ACT_NAME, "", ACT_COMMAND, "", ACT_FOR, "both", -1);
    add_and_edit(view, store, &iter);
}

/* Removes the selected row; returns the model to save, or NULL. */
static GtkTreeModel* remove_selected(GtkWidget* view)
{
    GtkTreeIter iter;
    if (!selected_iter(view, &iter))
        return NULL;
    GtkTreeModel* model = gtk_tree_view_get_model(GTK_TREE_VIEW(view));
    gtk_list_store_remove(GTK_LIST_STORE(model), &iter);
    return model;
}

static void on_action_remove(GtkButton* button, gpointer view)
{
    (void)button;
    GtkTreeModel* model = remove_selected(view);
    if (model != NULL)
        save_actions(model);
}

static GtkWidget* actions_page(GtkListStore** store_out)
{
    GtkListStore* store = gtk_list_store_new(ACT_N, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    fill_actions(store);
    GtkWidget* view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);
    text_column(view, "Name", ACT_NAME, true, G_CALLBACK(on_action_name), store);
    text_column(view, "Command", ACT_COMMAND, true, G_CALLBACK(on_action_command), store);

    GtkListStore* kinds = gtk_list_store_new(1, G_TYPE_STRING);
    for (size_t i = 0; i < G_N_ELEMENTS(kind_names); i++)
        gtk_list_store_insert_with_values(kinds, NULL, -1, 0, kind_names[i], -1);
    GtkCellRenderer* combo = gtk_cell_renderer_combo_new();
    g_object_set(combo, "model", kinds, "text-column", 0, "has-entry", FALSE, "editable", TRUE, NULL);
    g_object_unref(kinds);
    g_signal_connect(combo, "edited", G_CALLBACK(on_action_for), store);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), gtk_tree_view_column_new_with_attributes("For", combo, "text", ACT_FOR, NULL));

    GtkWidget* add = gtk_button_new_with_label("Add");
    GtkWidget* remove = gtk_button_new_with_label("Remove");
    g_signal_connect(add, "clicked", G_CALLBACK(on_action_add), view);
    g_signal_connect(remove, "clicked", G_CALLBACK(on_action_remove), view);
    GtkWidget* buttons[] = { add, remove };
    *store_out = store;
    return list_page("Commands of your own in the right-click menu. In the command, %f stands for the selected "
                     "items and %d for the current folder. The same name can have one command for files and "
                     "another for folders.",
        view, buttons, G_N_ELEMENTS(buttons));
}

/* ---- Programs per file type ----------------------------------------------- */

enum {
    OPEN_EXT,
    OPEN_COMMAND,
    OPEN_N,
};

static int compare_strings(gconstpointer a, gconstpointer b)
{
    return g_strcmp0(*(char* const*)a, *(char* const*)b);
}

static void fill_programs(GtkListStore* store)
{
    const SfeConfig* cfg = sfe_config();
    gtk_list_store_clear(store);
    guint n = 0;
    g_autofree gpointer* keys = g_hash_table_get_keys_as_array(cfg->open_with, &n);
    qsort(keys, n, sizeof(gpointer), compare_strings);
    for (guint i = 0; i < n; i++)
        gtk_list_store_insert_with_values(store, NULL, -1, OPEN_EXT, keys[i], OPEN_COMMAND,
            g_hash_table_lookup(cfg->open_with, keys[i]), -1);
}

static void save_programs(GtkTreeModel* model)
{
    SfeConfig* cfg = sfe_application_dup_config(sfe_app());
    g_hash_table_remove_all(cfg->open_with);
    GtkTreeIter iter;
    for (bool ok = gtk_tree_model_get_iter_first(model, &iter); ok; ok = gtk_tree_model_iter_next(model, &iter)) {
        g_autofree char* ext = NULL;
        g_autofree char* command = NULL;
        gtk_tree_model_get(model, &iter, OPEN_EXT, &ext, OPEN_COMMAND, &command, -1);
        if (ext && *ext && command && *command)
            g_hash_table_replace(cfg->open_with, g_ascii_strdown(ext, -1), g_strdup(command));
    }
    save_config(cfg);
}

static void on_program_ext(GtkCellRendererText* cell, char* path, char* text, gpointer store)
{
    (void)cell;
    g_strstrip(text);
    while (*text == '.') /* ".mp4" -> "mp4" */
        text++;
    g_autofree char* lower = g_ascii_strdown(text, -1);
    edit_cell(store, path, OPEN_EXT, lower);
    save_programs(store);
}

static void on_program_command(GtkCellRendererText* cell, char* path, char* text, gpointer store)
{
    (void)cell;
    edit_cell(store, path, OPEN_COMMAND, g_strstrip(text));
    save_programs(store);
}

static void on_program_remove(GtkButton* button, gpointer view)
{
    (void)button;
    GtkTreeModel* model = remove_selected(view);
    if (model != NULL)
        save_programs(model);
}

static void on_program_add(GtkButton* button, gpointer view)
{
    (void)button;
    GtkListStore* store = GTK_LIST_STORE(gtk_tree_view_get_model(view));
    GtkTreeIter iter;
    gtk_list_store_insert_with_values(store, &iter, -1, OPEN_EXT, "", OPEN_COMMAND, "", -1);
    add_and_edit(view, store, &iter);
}

static GtkWidget* programs_page(GtkListStore** store_out)
{
    GtkListStore* store = gtk_list_store_new(OPEN_N, G_TYPE_STRING, G_TYPE_STRING);
    fill_programs(store);
    GtkWidget* view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);
    GtkTreeViewColumn* ext = text_column(view, "Extension", OPEN_EXT, true, G_CALLBACK(on_program_ext), store);
    gtk_tree_view_column_set_expand(ext, FALSE);
    gtk_tree_view_column_set_min_width(ext, 110);
    text_column(view, "Command", OPEN_COMMAND, true, G_CALLBACK(on_program_command), store);

    GtkWidget* add = gtk_button_new_with_label("Add");
    GtkWidget* remove = gtk_button_new_with_label("Remove");
    g_signal_connect(add, "clicked", G_CALLBACK(on_program_add), view);
    g_signal_connect(remove, "clicked", G_CALLBACK(on_program_remove), view);
    GtkWidget* buttons[] = { add, remove };
    *store_out = store;
    return list_page("Which program opens a file, by extension; %f stands for the file. Other files open with "
                     "the system's default application (xdg-open).",
        view, buttons, G_N_ELEMENTS(buttons));
}

/* ------------------------------------------------------------------------ */
/* The window                                                                */
/* ------------------------------------------------------------------------ */

typedef struct {
    GtkListStore* keys;
    GtkListStore* actions;
    GtkListStore* programs;
    gulong handler;
} Lists;

/* config.toml edited by hand while the window is open: the lists follow
 * (the option pages are read when the window opens). */
static void on_config_changed(SfeApplication* app, gpointer data)
{
    (void)app;
    Lists* l = data;
    if (applying)
        return;
    fill_keys(l->keys);
    fill_actions(l->actions);
    fill_programs(l->programs);
}

static void on_dialog_destroy(GtkWidget* dialog, gpointer data)
{
    Lists* l = data;
    GtkWidget* window = g_object_get_data(G_OBJECT(dialog), "sfe-window");
    g_signal_handler_disconnect(sfe_app(), l->handler);
    g_object_set_data(G_OBJECT(window), DIALOG_KEY, NULL);
    g_free(l);
}

static void add_page(GtkStack* stack, GtkWidget* page, const char* name, const char* title, bool scroll)
{
    GtkWidget* child = page;
    if (scroll) {
        child = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(child), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_container_add(GTK_CONTAINER(child), page);
    }
    gtk_stack_add_titled(stack, child, name, title);
}

static void on_close_clicked(GtkButton* button, gpointer dialog)
{
    (void)button;
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CLOSE);
}

/* Under the pages, across the whole window: a hint on the left, Close on the
 * right (the dialog's own button row sat in the corner, over the sidebar). */
static GtkWidget* footer(GtkWidget* dialog)
{
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_style_context_add_class(gtk_widget_get_style_context(box), "sfe-settings-footer");
    GtkWidget* hint = note("");
    gtk_label_set_max_width_chars(GTK_LABEL(hint), -1);
    gtk_label_set_ellipsize(GTK_LABEL(hint), PANGO_ELLIPSIZE_END);
    gtk_label_set_line_wrap(GTK_LABEL(hint), FALSE);
    gtk_widget_set_valign(hint, GTK_ALIGN_CENTER);
    g_object_set_data(G_OBJECT(dialog), "sfe-hint", hint);
    GtkWidget* close = gtk_button_new_with_label("Close");
    g_signal_connect(close, "clicked", G_CALLBACK(on_close_clicked), dialog);
    gtk_box_pack_start(GTK_BOX(box), hint, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(box), close, FALSE, FALSE, 0);
    return box;
}

static const char* const page_names[] = { "general", "thumbnails", "sidebar", "keys", "commands", "programs", "system", NULL };

const char* const* sfe_preferences_pages(void)
{
    return page_names;
}

void sfe_show_preferences(GtkWidget* parent)
{
    sfe_show_preferences_page(parent, NULL);
}

static void show_page(GtkWidget* dialog, const char* page)
{
    GtkWidget* stack = g_object_get_data(G_OBJECT(dialog), "sfe-stack");
    if (page != NULL && gtk_stack_get_child_by_name(GTK_STACK(stack), page) != NULL)
        gtk_stack_set_visible_child_name(GTK_STACK(stack), page);
}

void sfe_show_preferences_page(GtkWidget* parent, const char* page)
{
    GtkWidget* window = gtk_widget_get_toplevel(parent);
    GtkWidget* dialog = g_object_get_data(G_OBJECT(window), DIALOG_KEY);

    if (dialog != NULL) {
        show_page(dialog, page);
        gtk_window_present(GTK_WINDOW(dialog));
        return;
    }
    dialog = sfe_dialog_new(parent, "Settings");
    gtk_window_set_default_size(GTK_WINDOW(dialog), 820, 560);
    Lists* lists = g_new0(Lists, 1);

    GtkWidget* stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    add_page(GTK_STACK(stack), options_page("general", NULL), "general", "General", true);
    add_page(GTK_STACK(stack),
        options_page("preview", "Pictures, videos and documents show a small preview instead of their icon. "
                                "Thumbnails are shared with other file managers in ~/.cache/thumbnails."),
        "thumbnails", "Thumbnails", true);
    add_page(GTK_STACK(stack),
        options_page("sidebar", "Bookmarks are added with Ctrl+D or the menus, reordered by dragging and removed "
                                "from their right-click menu."),
        "sidebar", "Sidebar", true);
    add_page(GTK_STACK(stack), keys_page(&lists->keys), "keys", "Keys", false);
    add_page(GTK_STACK(stack), actions_page(&lists->actions), "commands", "Custom Commands", false);
    add_page(GTK_STACK(stack), programs_page(&lists->programs), "programs", "Programs", false);
    add_page(GTK_STACK(stack), system_page(), "system", "System", true);

    GtkWidget* sidebar = gtk_stack_sidebar_new();
    gtk_stack_sidebar_set_stack(GTK_STACK_SIDEBAR(sidebar), GTK_STACK(stack));
    gtk_widget_set_size_request(sidebar, 170, -1);
    GtkWidget* body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(body), sidebar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), stack, TRUE, TRUE, 0);
    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), 0);
    gtk_box_set_spacing(GTK_BOX(content), 0);
    gtk_box_pack_start(GTK_BOX(content), body, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(content), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), footer(dialog), FALSE, FALSE, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(dialog), "sfe-settings");

    g_signal_connect(dialog, "response", G_CALLBACK(gtk_widget_destroy), NULL);
    lists->handler = g_signal_connect(sfe_app(), "config-changed", G_CALLBACK(on_config_changed), lists);
    g_object_set_data(G_OBJECT(dialog), "sfe-window", window);
    g_signal_connect(dialog, "destroy", G_CALLBACK(on_dialog_destroy), lists);
    g_object_set_data(G_OBJECT(window), DIALOG_KEY, dialog);
    g_object_set_data(G_OBJECT(dialog), "sfe-stack", stack);
    gtk_widget_show_all(dialog);
    show_page(dialog, page);
}
