#include "icons.h"

#include "app.h"
#include <math.h>
#include <stdbool.h>
#include <string.h>

static GHashTable* cache; /* "style@content-type-or-special-icon@size@scale" -> cairo_surface_t* */

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

/* A colour of sfe's palette (theme.c defines @sfe_accent, @sfe_fg ...). */
static GdkRGBA palette_color(const char* name, const char* fallback)
{
    GdkRGBA color;
    GtkStyleContext* ctx = gtk_style_context_new();
    gtk_style_context_set_screen(ctx, gdk_screen_get_default());
    if (!gtk_style_context_lookup_color(ctx, name, &color))
        gdk_rgba_parse(&color, fallback);
    g_object_unref(ctx);
    return color;
}

static bool is_folder(GIcon* icon)
{
    if (!G_IS_THEMED_ICON(icon))
        return false;
    const char* const* names = g_themed_icon_get_names(G_THEMED_ICON(icon));
    return names != NULL && names[0] != NULL
        && (strstr(names[0], "folder") != NULL || g_str_has_prefix(names[0], "inode-directory"));
}

static void rgb_to_hsl(double r, double g, double b, double* h, double* s, double* l)
{
    double max = MAX(r, MAX(g, b)), min = MIN(r, MIN(g, b));
    *l = (max + min) / 2;
    if (max == min) {
        *h = *s = 0;
        return;
    }
    double d = max - min;
    *s = *l > 0.5 ? d / (2 - max - min) : d / (max + min);
    if (max == r)
        *h = (g - b) / d + (g < b ? 6 : 0);
    else if (max == g)
        *h = (b - r) / d + 2;
    else
        *h = (r - g) / d + 4;
    *h /= 6;
}

static double hue_to_rgb(double p, double q, double t)
{
    t = t < 0 ? t + 1 : t > 1 ? t - 1 : t;
    if (t < 1.0 / 6)
        return p + (q - p) * 6 * t;
    if (t < 0.5)
        return q;
    if (t < 2.0 / 3)
        return p + (q - p) * (2.0 / 3 - t) * 6;
    return p;
}

static void hsl_to_rgb(double h, double s, double l, double* r, double* g, double* b)
{
    if (s == 0) {
        *r = *g = *b = l;
        return;
    }
    double q = l < 0.5 ? l * (1 + s) : l + s - l * s;
    double p = 2 * l - q;
    *r = hue_to_rgb(p, q, h + 1.0 / 3);
    *g = hue_to_rgb(p, q, h);
    *b = hue_to_rgb(p, q, h - 1.0 / 3);
}

/* Recolours the coloured parts of a full-colour icon (a folder's body) to
 * `accent`, keeping their shading; greys and whites stay. */
static void tint(GdkPixbuf* pixbuf, const GdkRGBA* accent)
{
    double ah, as, al;
    rgb_to_hsl(accent->red, accent->green, accent->blue, &ah, &as, &al);
    /* Shades of the icon are moved around the accent's lightness. Folder
     * icons are drawn around mid lightness; a very light or dark accent is
     * kept in a band where the folder still reads as a folder. */
    double shift = CLAMP(al, 0.35, 0.60) - 0.55;
    int n = gdk_pixbuf_get_n_channels(pixbuf), stride = gdk_pixbuf_get_rowstride(pixbuf);
    int w = gdk_pixbuf_get_width(pixbuf), h = gdk_pixbuf_get_height(pixbuf);
    guchar* pixels = gdk_pixbuf_get_pixels(pixbuf);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            guchar* p = pixels + y * stride + x * n;
            double ph, ps, pl;
            rgb_to_hsl(p[0] / 255.0, p[1] / 255.0, p[2] / 255.0, &ph, &ps, &pl);
            if (ps < 0.15) /* grey, white, black: the paper, outlines */
                continue;
            double r, g, b;
            hsl_to_rgb(ah, MIN(1.0, as * (0.6 + 0.4 * ps)), CLAMP(pl + shift, 0.0, 1.0), &r, &g, &b);
            p[0] = (guchar)lround(r * 255);
            p[1] = (guchar)lround(g * 255);
            p[2] = (guchar)lround(b * 255);
        }
    }
}

/* A one-colour (symbolic) version: folders in the accent, the rest in the
 * text colour. NULL when the theme has no symbolic icon for it. */
static GdkPixbuf* load_symbolic(GIcon* icon, int size, int scale)
{
    GtkIconTheme* theme = gtk_icon_theme_get_default();
    GtkIconLookupFlags flags = GTK_ICON_LOOKUP_FORCE_SIZE | GTK_ICON_LOOKUP_FORCE_SYMBOLIC;
    GtkIconInfo* info = icon ? gtk_icon_theme_lookup_by_gicon_for_scale(theme, icon, size, scale, flags) : NULL;
    if (info != NULL && !gtk_icon_info_is_symbolic(info)) /* only a colour one: the generic page */
        g_clear_object(&info);
    if (info == NULL)
        info = gtk_icon_theme_lookup_icon_for_scale(theme, "text-x-generic-symbolic", size, scale, flags);
    if (info == NULL)
        return NULL;
    GdkRGBA fg = palette_color("sfe_view_fg", "#2e3436");
    GdkRGBA accent = palette_color("sfe_accent", "#3584e4");
    fg.alpha = 0.75;
    GdkPixbuf* pixbuf = gtk_icon_info_load_symbolic(info, is_folder(icon) ? &accent : &fg, NULL, NULL, NULL, NULL, NULL);
    g_object_unref(info);
    return pixbuf;
}

static GdkPixbuf* load_pixbuf(GIcon* icon, int size, int scale, int style)
{
    if (style == SFE_FILE_ICONS_SYMBOLIC) {
        GdkPixbuf* symbolic = load_symbolic(icon, size, scale);
        if (symbolic != NULL)
            return symbolic;
    }

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
    if (pixbuf != NULL && style == SFE_FILE_ICONS_TINTED && is_folder(icon)) {
        GdkPixbuf* own = gdk_pixbuf_copy(pixbuf); /* the theme's copy may be shared */
        g_object_unref(pixbuf);
        pixbuf = own;
        GdkRGBA accent = palette_color("sfe_accent", "#3584e4");
        tint(pixbuf, &accent);
    }
    if (pixbuf == NULL) { /* no usable icon theme at all: transparent square */
        pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, size * scale, size * scale);
        gdk_pixbuf_fill(pixbuf, 0);
    }
    return pixbuf;
}

/* The home folder and the XDG folders (Downloads, Pictures, ...) have icons
 * of their own, as GIO gives them; NULL for any other path. */
static const char* special_icon_name(const char* path)
{
    static const struct {
        GUserDirectory dir;
        const char* icon;
    } specials[] = {
        { G_USER_DIRECTORY_DESKTOP, "user-desktop" },
        { G_USER_DIRECTORY_DOCUMENTS, "folder-documents" },
        { G_USER_DIRECTORY_DOWNLOAD, "folder-download" },
        { G_USER_DIRECTORY_MUSIC, "folder-music" },
        { G_USER_DIRECTORY_PICTURES, "folder-pictures" },
        { G_USER_DIRECTORY_PUBLIC_SHARE, "folder-publicshare" },
        { G_USER_DIRECTORY_TEMPLATES, "folder-templates" },
        { G_USER_DIRECTORY_VIDEOS, "folder-videos" },
    };
    if (g_strcmp0(path, g_get_home_dir()) == 0)
        return "user-home";
    for (size_t i = 0; i < G_N_ELEMENTS(specials); i++) {
        if (g_strcmp0(path, g_get_user_special_dir(specials[i].dir)) == 0)
            return specials[i].icon;
    }
    return NULL;
}

/* The icon GIO would give the file: its type's icon, or a special folder's.
 * Not read from the file info: those GIcons cost about 850 bytes per file. */
static GIcon* icon_of(const char* type, const char* special, bool symbolic)
{
    if (special != NULL) {
        g_autofree char* name = symbolic ? g_strconcat(special, "-symbolic", NULL) : g_strdup(special);
        const char* names[] = { name, symbolic ? "folder-symbolic" : "folder", NULL };
        return g_themed_icon_new_from_names((char**)names, -1);
    }
    return symbolic ? g_content_type_get_symbolic_icon(type) : g_content_type_get_icon(type);
}

cairo_surface_t* sfe_icon_for_info(GFileInfo* info, GFile* parent, int size, int scale)
{
    int style = sfe_config()->file_icons;
    const char* type = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE);
    if (type == NULL)
        type = g_file_info_get_content_type(info);
    if (type == NULL)
        type = "application/octet-stream";
    const char* special = NULL;
    if (parent != NULL && g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
        g_autofree char* dir = g_file_get_path(parent);
        g_autofree char* path = dir ? g_build_filename(dir, g_file_info_get_name(info), NULL) : NULL;
        special = special_icon_name(path);
    }
    /* One entry per type (or special folder), size and scale. */
    char key[256];
    g_snprintf(key, sizeof key, "%d@%s@%d@%d", style, special ? special : type, size, scale);

    ensure_cache();
    cairo_surface_t* surface = g_hash_table_lookup(cache, key);
    if (surface == NULL) {
        g_autoptr(GIcon) icon = icon_of(type, special, style == SFE_FILE_ICONS_SYMBOLIC);
        g_autoptr(GdkPixbuf) pixbuf = load_pixbuf(icon, size, scale, style);
        surface = gdk_cairo_surface_create_from_pixbuf(pixbuf, scale, NULL);
        g_hash_table_insert(cache, g_strdup(key), surface);
    }
    return cairo_surface_reference(surface);
}

void sfe_icons_reset(void)
{
    if (cache != NULL)
        g_hash_table_remove_all(cache);
    /* The views re-fetch their icons on "changed" (connected after us). */
    g_signal_emit_by_name(gtk_icon_theme_get_default(), "changed");
}

GdkPixbuf* sfe_icon_preview(const char* content_type, int style, int size, int scale)
{
    g_autoptr(GIcon) icon = style == SFE_FILE_ICONS_SYMBOLIC ? g_content_type_get_symbolic_icon(content_type)
                                                            : g_content_type_get_icon(content_type);
    return load_pixbuf(icon, size, scale, style);
}
