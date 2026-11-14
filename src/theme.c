#include "theme.h"

#include <gtk/gtk.h>

/* GTK3 reads ~/.config/gtk-3.0/gtk.css exactly once, at startup. To pick up
 * later edits we load the same file into our own provider, one priority step
 * above GTK's copy, and reload it whenever something in that directory
 * changes. */
#define USER_CSS_PRIORITY (GTK_STYLE_PROVIDER_PRIORITY_USER + 1)
#define RELOAD_DELAY_MS 150

static GtkCssProvider* user_provider; /* NULL until the first change */
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

static gboolean reload_timeout(gpointer data)
{
    (void)data;
    reload_source = 0;
    reload_user_css();
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

static void load_app_css(void)
{
    GtkCssProvider* provider = gtk_css_provider_new();
    gtk_css_provider_load_from_resource(provider, "/org/sfe/style.css");
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
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

static void apply_scale(void)
{
    GdkScreen* screen = gdk_screen_get_default();
    gdk_screen_set_resolution(screen, base_dpi() * ui_scale);
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
    load_app_css();
    watch_user_css();
    g_signal_connect(gtk_settings_get_default(), "notify::gtk-xft-dpi",
        G_CALLBACK(on_dpi_changed), NULL);
}
