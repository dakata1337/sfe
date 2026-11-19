#include "theme.h"

#include "app.h"
#include "icons.h"

#include <gtk/gtk.h>
#include <math.h>
#include <stdbool.h>

/* GTK3 reads ~/.config/gtk-3.0/gtk.css exactly once, at startup. To pick up
 * later edits we load the same file into our own provider, one priority step
 * above GTK's copy, and reload it whenever something in that directory
 * changes. */
#define USER_CSS_PRIORITY (GTK_STYLE_PROVIDER_PRIORITY_USER + 1)
#define RELOAD_DELAY_MS 150

static GtkCssProvider* user_provider; /* NULL until the first change */
static GtkCssProvider* palette_provider;
static guint palette_source;
static GPtrArray* monitors; /* GFileMonitor* */
static guint reload_source;
static double ui_scale = 1.0;

static char* user_css_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "gtk-3.0", "gtk.css", NULL);
}

static void reload_user_css(void)
{
    g_autofree char* path = user_css_path();
    g_autoptr(GError) error = NULL;
    GdkScreen* screen = gdk_screen_get_default();

    if (user_provider == NULL) {
        user_provider = gtk_css_provider_new();
        gtk_style_context_add_provider_for_screen(screen,
            GTK_STYLE_PROVIDER(user_provider), USER_CSS_PRIORITY);
    }

    if (!g_file_test(path, G_FILE_TEST_EXISTS))
        gtk_css_provider_load_from_data(user_provider, "", -1, NULL);
    else if (!gtk_css_provider_load_from_path(user_provider, path, &error))
        g_warning("%s: %s", path, error->message);
    else
        g_debug("reloaded %s", path);
}

static void schedule_palette_rebuild(void);

static gboolean reload_timeout(gpointer data)
{
    (void)data;
    reload_source = 0;
    reload_user_css();
    schedule_palette_rebuild();
    return G_SOURCE_REMOVE;
}

/* Theme generators usually rewrite several files in a row; coalesce them. */
static void on_css_dir_changed(GFileMonitor* monitor, GFile* file, GFile* other,
    GFileMonitorEvent event, gpointer data)
{
    (void)monitor, (void)file, (void)other, (void)data;

    if (event == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT
        || event == G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED)
        return;
    if (reload_source != 0)
        g_source_remove(reload_source);
    reload_source = g_timeout_add(RELOAD_DELAY_MS, reload_timeout, NULL);
}

static void watch_directory(const char* dir)
{
    g_autoptr(GFile) file = g_file_new_for_path(dir);
    g_autoptr(GError) error = NULL;
    GFileMonitor* monitor = g_file_monitor_directory(file, G_FILE_MONITOR_WATCH_MOVES, NULL, &error);

    if (monitor == NULL) {
        g_warning("cannot watch %s: %s", dir, error->message);
        return;
    }
    g_signal_connect(monitor, "changed", G_CALLBACK(on_css_dir_changed), NULL);
    g_ptr_array_add(monitors, monitor);
}

static void watch_user_css(void)
{
    g_autofree char* path = user_css_path();
    g_autofree char* dir = g_path_get_dirname(path);

    monitors = g_ptr_array_new_with_free_func(g_object_unref);
    watch_directory(dir);

    /* Generators like pywal symlink gtk.css into their own cache directory. */
    g_autofree char* target = g_file_read_link(path, NULL);
    if (target != NULL) {
        g_autofree char* abs = g_path_is_absolute(target)
            ? g_strdup(target)
            : g_build_filename(dir, target, NULL);
        g_autofree char* target_dir = g_path_get_dirname(abs);
        if (g_strcmp0(target_dir, dir) != 0)
            watch_directory(target_dir);
    }
}

/* ------------------------------------------------------------------------ */
/* Palette                                                                   */
/* ------------------------------------------------------------------------ */

/* style.css uses only these @sfe_* colors. Each is taken from the first name
 * the theme or the user's gtk.css defines: libadwaita-style names first
 * (Noctalia, matugen, adw-gtk3), then classic GTK3 names. The resolved
 * values are written as plain colors, which also rules out reference cycles
 * between themes that alias the two naming schemes to each other. */
typedef struct {
    const char* name;
    const char* sources[3]; /* NULL-terminated */
    const char* fallback; /* CSS color expression */
} PaletteEntry;

static const PaletteEntry palette[] = {
    { "sfe_bg", { "window_bg_color", "theme_bg_color" }, "#f6f5f4" },
    { "sfe_fg", { "window_fg_color", "theme_fg_color" }, "#2e3436" },
    { "sfe_view_bg", { "view_bg_color", "theme_base_color" }, "@sfe_bg" },
    { "sfe_view_fg", { "view_fg_color", "theme_text_color" }, "@sfe_fg" },
    { "sfe_sidebar_bg", { "sidebar_bg_color" }, "@sfe_bg" },
    { "sfe_accent", { "accent_bg_color", "theme_selected_bg_color" }, "#3584e4" },
    { "sfe_accent_fg", { "accent_fg_color", "theme_selected_fg_color" }, "#ffffff" },
    /* error_bg_color first: base themes (Adwaita) define error_color, which
     * would win over a generated theme that only sets error_bg_color. */
    { "sfe_error", { "error_bg_color", "error_color" }, "#e01b24" },
    { "sfe_error_fg", { "error_fg_color", "destructive_fg_color" }, "#ffffff" },
};

/* What the palette was built from, for Settings → System. */
static const char* const color_labels[] = { "Window", "Text", "View", "View text", "Sidebar", "Accent",
    "Accent text", "Error", "Error text" };
static SfeThemeColor colors[G_N_ELEMENTS(palette)];
typedef struct {
    GtkWidget* widget;
    void (*changed)(GtkWidget* widget);
} Watcher;
static GPtrArray* watchers; /* Watcher*, told about palette changes */

static gboolean rebuild_palette(gpointer data)
{
    (void)data;
    GtkStyleContext* ctx = gtk_style_context_new();
    GString* css = g_string_new(NULL);

    palette_source = 0;
    gtk_style_context_set_screen(ctx, gdk_screen_get_default());
    for (size_t i = 0; i < G_N_ELEMENTS(palette); i++) {
        const PaletteEntry* e = &palette[i];
        GdkRGBA color;
        bool found = false;
        int j = 0;

        for (; e->sources[j] != NULL && !found; j++)
            found = gtk_style_context_lookup_color(ctx, e->sources[j], &color);

        g_autofree char* value = found ? gdk_rgba_to_string(&color) : g_strdup(e->fallback);
        g_string_append_printf(css, "@define-color %s %s;\n", e->name, value);
        colors[i].name = e->name;
        colors[i].label = color_labels[i];
        colors[i].source = found ? e->sources[j - 1] : NULL;
    }
    gtk_css_provider_load_from_data(palette_provider, css->str, -1, NULL);
    /* The values as resolved (fallbacks like "@sfe_bg" included). */
    GtkStyleContext* resolved = gtk_style_context_new();
    gtk_style_context_set_screen(resolved, gdk_screen_get_default());
    for (size_t i = 0; i < G_N_ELEMENTS(palette); i++) {
        if (!gtk_style_context_lookup_color(resolved, palette[i].name, &colors[i].rgba))
            gdk_rgba_parse(&colors[i].rgba, "#000");
    }
    g_object_unref(resolved);
    for (guint i = 0; watchers != NULL && i < watchers->len; i++) {
        const Watcher* w = g_ptr_array_index(watchers, i);
        w->changed(w->widget);
    }
    if (sfe_config() != NULL && sfe_config()->file_icons != SFE_FILE_ICONS_SYSTEM) /* (none yet at startup) */
        sfe_icons_reset(); /* tinted / symbolic icons use these colours */
    g_debug("palette:\n%s", css->str);

    g_string_free(css, TRUE);
    g_object_unref(ctx);
    return G_SOURCE_REMOVE;
}

const SfeThemeColor* sfe_theme_colors(size_t* n)
{
    *n = G_N_ELEMENTS(colors);
    return colors;
}

static void on_watcher_destroy(GtkWidget* widget, gpointer data)
{
    (void)widget;
    g_ptr_array_remove(watchers, data); /* (frees it) */
}

void sfe_theme_watch_colors(GtkWidget* widget, void (*changed)(GtkWidget* widget))
{
    if (watchers == NULL)
        watchers = g_ptr_array_new_with_free_func(g_free);
    Watcher* w = g_new(Watcher, 1);
    w->widget = widget;
    w->changed = changed;
    g_ptr_array_add(watchers, w);
    g_signal_connect(widget, "destroy", G_CALLBACK(on_watcher_destroy), w);
}

/* Theme switches can arrive as several setting changes; rebuild once. */
static void schedule_palette_rebuild(void)
{
    if (palette_source == 0)
        palette_source = g_idle_add(rebuild_palette, NULL);
}

static void on_theme_setting_changed(GObject* settings, GParamSpec* pspec, gpointer data)
{
    (void)settings, (void)pspec, (void)data;
    schedule_palette_rebuild();
}

static void load_app_css(void)
{
    GtkCssProvider* provider = gtk_css_provider_new();
    GdkScreen* screen = gdk_screen_get_default();

    palette_provider = gtk_css_provider_new();
    gtk_style_context_add_provider_for_screen(screen,
        GTK_STYLE_PROVIDER(palette_provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    rebuild_palette(NULL);

    gtk_css_provider_load_from_resource(provider, "/org/sfe/style.css");
    gtk_style_context_add_provider_for_screen(screen,
        GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

/* The DPI the desktop asked for, before our scaling. */
static double base_dpi(void)
{
    int xft_dpi = 0;
    g_object_get(gtk_settings_get_default(), "gtk-xft-dpi", &xft_dpi, NULL);
    return xft_dpi > 0 ? xft_dpi / 1024.0 : 96.0;
}

/* ------------------------------------------------------------------------ */
/* Scale                                                                     */
/* ------------------------------------------------------------------------ */

static GtkCssProvider* scale_provider; /* pixel sizes that fonts do not move */
static GPtrArray* scaled_icons; /* GtkImage*, weak */

static int scaled(int base)
{
    return (int)lround(base * ui_scale);
}

int sfe_theme_scaled(int base_px)
{
    return scaled(base_px);
}

GtkIconSize sfe_theme_icon_size_near(int base_px)
{
    static const struct {
        int px;
        GtkIconSize size;
    } sizes[] = { { 16, GTK_ICON_SIZE_MENU }, { 24, GTK_ICON_SIZE_LARGE_TOOLBAR }, { 32, GTK_ICON_SIZE_DND },
        { 48, GTK_ICON_SIZE_DIALOG } };
    int want = scaled(base_px);
    GtkIconSize best = GTK_ICON_SIZE_MENU;
    int best_gap = G_MAXINT;
    for (size_t i = 0; i < G_N_ELEMENTS(sizes); i++) {
        int gap = ABS(sizes[i].px - want);
        if (gap < best_gap) {
            best = sizes[i].size;
            best_gap = gap;
        }
    }
    return best;
}

static void forget_icon(gpointer data, GObject* gone)
{
    (void)data;
    g_ptr_array_remove_fast(scaled_icons, gone);
}

GtkWidget* sfe_scaled_icon_new(const char* icon_name, int base_px)
{
    GtkWidget* image = gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_MENU);
    g_object_set_data(G_OBJECT(image), "sfe-base-px", GINT_TO_POINTER(base_px));
    gtk_image_set_pixel_size(GTK_IMAGE(image), scaled(base_px));
    if (scaled_icons == NULL)
        scaled_icons = g_ptr_array_new();
    g_ptr_array_add(scaled_icons, image);
    g_object_weak_ref(G_OBJECT(image), forget_icon, NULL);
    return image;
}

/* Buttons and tree expanders have pixel sizes in style.css. */
static void load_scale_css(void)
{
    g_autofree char* css = g_strdup_printf(
        ".sfe-window button { min-width: %dpx; min-height: %dpx; }\n"
        ".sfe-sidebar button.sfe-eject, .sfe-window .sfe-tree-bar button { min-width: %dpx; min-height: %dpx; }\n"
        ".sfe-window .sfe-sidebar treeview.view { -GtkTreeView-expander-size: %d; }\n",
        scaled(26), scaled(26), scaled(20), scaled(20), scaled(12));
    if (scale_provider == NULL) {
        scale_provider = gtk_css_provider_new();
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(scale_provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    }
    gtk_css_provider_load_from_data(scale_provider, css, -1, NULL);
}

static void apply_scale(void)
{
    GdkScreen* screen = gdk_screen_get_default();
    for (guint i = 0; scaled_icons != NULL && i < scaled_icons->len; i++) {
        GtkImage* image = g_ptr_array_index(scaled_icons, i);
        gtk_image_set_pixel_size(image, scaled(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(image), "sfe-base-px"))));
    }
    load_scale_css();
    gdk_screen_set_resolution(screen, base_dpi() * ui_scale); /* (notify::resolution: the tree's icons) */
    gtk_style_context_reset_widgets(screen);
}

static void on_dpi_changed(GObject* settings, GParamSpec* pspec, gpointer data)
{
    (void)settings, (void)pspec, (void)data;
    apply_scale();
}

void sfe_theme_set_scale(double scale)
{
    if (scale == ui_scale)
        return;
    ui_scale = scale;
    apply_scale();
}

void sfe_theme_init(void)
{
    GtkSettings* settings = gtk_settings_get_default();

    /* sfe's own icons (e.g. the split buttons), looked up like theme icons. */
    gtk_icon_theme_add_resource_path(gtk_icon_theme_get_default(), "/org/sfe/icons");
    load_app_css();
    load_scale_css();
    watch_user_css();
    g_signal_connect(settings, "notify::gtk-xft-dpi", G_CALLBACK(on_dpi_changed), NULL);
    g_signal_connect(settings, "notify::gtk-theme-name", G_CALLBACK(on_theme_setting_changed), NULL);
    g_signal_connect(settings, "notify::gtk-application-prefer-dark-theme",
        G_CALLBACK(on_theme_setting_changed), NULL);
}
