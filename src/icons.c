#include "icons.h"

static GHashTable* cache; /* "icon-string@size@scale" -> cairo_surface_t* */

static void on_theme_changed(GtkIconTheme* theme, gpointer data)
{
    (void)theme, (void)data;
    g_hash_table_remove_all(cache);
}

static void ensure_cache(void)
{
    if (cache != NULL)
        return;
    cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
        (GDestroyNotify)cairo_surface_destroy);
    g_signal_connect(gtk_icon_theme_get_default(), "changed", G_CALLBACK(on_theme_changed), NULL);
}

static GdkPixbuf* load_pixbuf(GIcon* icon, int size, int scale)
{
    GtkIconTheme* theme = gtk_icon_theme_get_default();
    /* No GENERIC_FALLBACK: themed GIcons already list their fallback names. */
    GtkIconLookupFlags flags = GTK_ICON_LOOKUP_FORCE_SIZE;
    GtkIconInfo* info = NULL;
    GdkPixbuf* pixbuf = NULL;

    if (icon != NULL)
        info = gtk_icon_theme_lookup_by_gicon_for_scale(theme, icon, size, scale, flags);
    if (info == NULL)
        info = gtk_icon_theme_lookup_icon_for_scale(theme, "text-x-generic", size, scale, flags);
    if (info != NULL) {
        pixbuf = gtk_icon_info_load_icon(info, NULL);
        g_object_unref(info);
    }
    if (pixbuf == NULL) { /* no usable icon theme at all: transparent square */
        pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, size * scale, size * scale);
        gdk_pixbuf_fill(pixbuf, 0);
    }
    return pixbuf;
}

cairo_surface_t* sfe_icon_for_info(GFileInfo* info, int size, int scale)
{
    GIcon* icon = g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_ICON)
        ? g_file_info_get_icon(info)
        : NULL;
    g_autofree char* icon_str = icon ? g_icon_to_string(icon) : g_strdup("");
    g_autofree char* key = g_strdup_printf("%s@%d@%d", icon_str, size, scale);

    ensure_cache();
    cairo_surface_t* surface = g_hash_table_lookup(cache, key);
    if (surface == NULL) {
        g_autoptr(GdkPixbuf) pixbuf = load_pixbuf(icon, size, scale);
        surface = gdk_cairo_surface_create_from_pixbuf(pixbuf, scale, NULL);
        g_hash_table_insert(cache, g_steal_pointer(&key), surface);
    }
    return cairo_surface_reference(surface);
}
