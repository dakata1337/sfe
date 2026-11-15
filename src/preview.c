#include "preview.h"

#include "dirmodel.h"
#include "icons.h"
#include "theme.h"
#include "thumbs.h"
#include <stdbool.h>

#define PREVIEW_WIDTH 240 /* logical pixels the picture may take */
#define ICON_SIZE 128

struct _SfePreview {
    GtkBox parent;

    GtkWidget* image;
    GtkWidget* name;
    GtkWidget* details;
    GFile* file; /* shown now, or NULL */
    GCancellable* cancel; /* the lookups for `file` */
};

G_DEFINE_TYPE(SfePreview, sfe_preview, GTK_TYPE_BOX)

/* Fits `pixbuf` into the preview's square and shows it sharp on HiDPI. */
static void show_pixbuf(SfePreview* self, GdkPixbuf* pixbuf)
{
    int scale = gtk_widget_get_scale_factor(GTK_WIDGET(self));
    int box = sfe_theme_scaled(PREVIEW_WIDTH) * scale;
    int w = gdk_pixbuf_get_width(pixbuf), h = gdk_pixbuf_get_height(pixbuf);
    g_autoptr(GdkPixbuf) fitted = NULL;
    if (w > box || h > box) {
        double f = MIN((double)box / w, (double)box / h);
        fitted = gdk_pixbuf_scale_simple(pixbuf, MAX(1, (int)(w * f)), MAX(1, (int)(h * f)), GDK_INTERP_BILINEAR);
    } else {
        fitted = g_object_ref(pixbuf);
    }
    cairo_surface_t* surface = gdk_cairo_surface_create_from_pixbuf(fitted, scale, NULL);
    gtk_image_set_from_surface(GTK_IMAGE(self->image), surface);
    cairo_surface_destroy(surface);
}

static void show_text(SfePreview* self, const char* name, const char* details)
{
    gtk_label_set_text(GTK_LABEL(self->name), name ? name : "");
    gtk_widget_set_visible(self->name, name != NULL);
    gtk_label_set_text(GTK_LABEL(self->details), details ? details : "");
    gtk_widget_set_visible(self->details, details != NULL);
}

static void forget(SfePreview* self)
{
    if (self->cancel != NULL)
        g_cancellable_cancel(self->cancel);
    g_clear_object(&self->cancel);
    g_clear_object(&self->file);
}

/* A hint in place of a file: nothing or several selected. */
static void show_hint(SfePreview* self, const char* icon, const char* text)
{
    forget(self);
    gtk_image_set_from_icon_name(GTK_IMAGE(self->image), icon, GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(self->image), 64);
    show_text(self, NULL, text);
}

static void on_thumbnail(GFile* file, GdkPixbuf* pixbuf, gpointer data)
{
    SfePreview* self = data;
    if (pixbuf != NULL && self->file != NULL && g_file_equal(file, self->file))
        show_pixbuf(self, pixbuf);
}

/* Adds "1920 × 1080" to the details once the image's header is read. */
static void on_image_size(GObject* source, GAsyncResult* result, gpointer data)
{
    (void)source;
    int width = 0, height = 0;
    g_autoptr(GError) error = NULL;
    if (gdk_pixbuf_get_file_info_finish(result, &width, &height, &error) == NULL)
        return; /* (also when cancelled: `data` may be gone) */
    SfePreview* self = data;
    g_autofree char* text = g_strdup_printf("%s\n%d × %d", gtk_label_get_text(GTK_LABEL(self->details)), width, height);
    gtk_label_set_text(GTK_LABEL(self->details), text);
}

static char* format_time(GFileInfo* info)
{
    g_autoptr(GDateTime) mtime = g_file_info_get_modification_date_time(info);
    if (mtime == NULL)
        return NULL;
    g_autoptr(GDateTime) local = g_date_time_to_local(mtime);
    return g_date_time_format(local, "%Y-%m-%d %H:%M");
}

static void on_info(GObject* source, GAsyncResult* result, gpointer data)
{
    GFile* file = G_FILE(source);
    g_autoptr(GError) error = NULL;
    g_autoptr(GFileInfo) info = g_file_query_info_finish(file, result, &error);
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return; /* `data` may be gone */
    SfePreview* self = data;
    if (info == NULL) {
        show_text(self, NULL, error->message);
        return;
    }

    int scale = gtk_widget_get_scale_factor(GTK_WIDGET(self));
    cairo_surface_t* icon = sfe_icon_for_info(info, sfe_theme_scaled(ICON_SIZE), scale);
    gtk_image_set_from_surface(GTK_IMAGE(self->image), icon);
    cairo_surface_destroy(icon);

    const char* type = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE);
    bool is_dir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
    g_autofree char* description = type ? g_content_type_get_description(type) : NULL;
    g_autofree char* size = is_dir ? NULL : g_format_size(g_file_info_get_size(info));
    g_autofree char* when = format_time(info);
    g_autoptr(GString) details = g_string_new(description ? description : is_dir ? "Folder" : "File");
    if (size != NULL)
        g_string_append_printf(details, "\n%s", size);
    if (when != NULL)
        g_string_append_printf(details, "\n%s", when);
    show_text(self, g_file_info_get_display_name(info), details->str);

    if (is_dir || type == NULL)
        return;
    SfeThumbQueue* queue = sfe_thumb_queue_get_default();
    if (sfe_thumb_queue_supports(queue, type)) {
        g_autoptr(GDateTime) mtime = g_file_info_get_modification_date_time(info);
        SfeThumbSize thumb = sfe_thumb_size_for(sfe_theme_scaled(PREVIEW_WIDTH) * scale);
        sfe_thumb_queue_request(queue, file, type, mtime ? g_date_time_to_unix(mtime) : 0, thumb, self->cancel,
            on_thumbnail, self);
    }
    g_autofree char* path = g_file_get_path(file);
    if (path != NULL && g_content_type_is_mime_type(type, "image/*"))
        gdk_pixbuf_get_file_info_async(path, self->cancel, on_image_size, self);
}

void sfe_preview_set_files(SfePreview* self, GList* files)
{
    if (files == NULL) {
        show_hint(self, "image-x-generic-symbolic", "Select a file to preview it");
        return;
    }
    if (files->next != NULL) {
        g_autofree char* text = g_strdup_printf("%u items selected", g_list_length(files));
        show_hint(self, "edit-select-all-symbolic", text);
        return;
    }
    GFile* file = files->data;
    if (self->file != NULL && g_file_equal(file, self->file))
        return; /* (the selection was redone around it) */
    forget(self);
    self->file = g_object_ref(file);
    self->cancel = g_cancellable_new();
    g_file_query_info_async(file, SFE_FILE_ATTRIBUTES, G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT, self->cancel,
        on_info, self);
}

static void sfe_preview_dispose(GObject* object)
{
    forget(SFE_PREVIEW(object)); /* before `self` goes: results would land here */
    G_OBJECT_CLASS(sfe_preview_parent_class)->dispose(object);
}

static void sfe_preview_class_init(SfePreviewClass* klass)
{
    G_OBJECT_CLASS(klass)->dispose = sfe_preview_dispose;
}

static void sfe_preview_init(SfePreview* self)
{
    gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
    gtk_box_set_spacing(GTK_BOX(self), 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(self)), "sfe-preview");
    gtk_widget_set_size_request(GTK_WIDGET(self), sfe_theme_scaled(PREVIEW_WIDTH) + 24, -1);

    self->image = gtk_image_new();
    gtk_widget_set_size_request(self->image, -1, sfe_theme_scaled(PREVIEW_WIDTH));
    self->name = gtk_label_new(NULL);
    gtk_style_context_add_class(gtk_widget_get_style_context(self->name), "sfe-preview-name");
    self->details = gtk_label_new(NULL);
    gtk_style_context_add_class(gtk_widget_get_style_context(self->details), "sfe-placeholder");
    GtkWidget* labels[] = { self->name, self->details };
    for (int i = 0; i < 2; i++) {
        gtk_label_set_line_wrap(GTK_LABEL(labels[i]), TRUE);
        gtk_label_set_line_wrap_mode(GTK_LABEL(labels[i]), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_justify(GTK_LABEL(labels[i]), GTK_JUSTIFY_CENTER);
        gtk_label_set_max_width_chars(GTK_LABEL(labels[i]), 1); /* wrap to the preview's width */
        gtk_label_set_selectable(GTK_LABEL(labels[i]), FALSE);
        gtk_widget_set_no_show_all(labels[i], TRUE);
    }
    gtk_box_pack_start(GTK_BOX(self), self->image, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(self), self->name, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(self), self->details, FALSE, FALSE, 0);
    gtk_widget_show(self->image);
}

GtkWidget* sfe_preview_new(void)
{
    GtkWidget* self = g_object_new(SFE_TYPE_PREVIEW, NULL);
    sfe_preview_set_files(SFE_PREVIEW(self), NULL);
    return self;
}
