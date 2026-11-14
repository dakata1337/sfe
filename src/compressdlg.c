#include "compressdlg.h"

#include "archive.h"
#include "dialogs.h"
#include "fileops.h"
#include "util.h"
#include <string.h>

/* The format picked last time, offered first next time. */
static char* last_ext;

typedef struct {
    GFile* dir;
    SfeCompressFunc done;
    gpointer data;
    GDestroyNotify destroy;
    GtkWidget* dialog;
    GtkWidget* entry;
    GtkWidget* format;
    GtkWidget* status;
} Compress;

static void compress_free(gpointer p)
{
    Compress* c = p;
    g_object_unref(c->dir);
    if (c->destroy != NULL)
        c->destroy(c->data);
    g_free(c);
}

bool sfe_compress_available(void)
{
    size_t n = 0;
    const SfeArchiveFormat* formats = sfe_archive_formats(&n);
    for (size_t i = 0; i < n; i++) {
        if (sfe_archive_can_compress(formats[i].ext))
            return true;
    }
    return false;
}

/* "notes.txt" -> "notes", a folder keeps its name; several items are named
 * after the folder they are in. */
static char* default_name(GList* files, GFile* dir)
{
    if (files != NULL && files->next == NULL) {
        g_autofree char* base = g_file_get_basename(files->data);
        bool is_dir = g_file_query_file_type(files->data, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL) == G_FILE_TYPE_DIRECTORY;
        g_autofree char* stem = NULL;
        g_autofree char* ext = NULL;
        sfe_split_extension(base, is_dir, &stem, &ext);
        return g_filename_display_name(*stem == '.' && stem[1] != '\0' ? stem + 1 : stem);
    }
    g_autofree char* base = g_file_get_basename(dir);
    if (base == NULL || strcmp(base, "/") == 0)
        return g_strdup("Archive");
    return g_filename_display_name(base);
}

/* The full file name, or NULL if the name cannot be used. */
static char* file_name(Compress* c)
{
    const char* name = gtk_entry_get_text(GTK_ENTRY(c->entry));
    const char* ext = gtk_combo_box_get_active_id(GTK_COMBO_BOX(c->format));

    if (*name == '\0' || strchr(name, '/') != NULL || ext == NULL)
        return NULL;
    return g_strconcat(name, ext, NULL);
}

static void update(Compress* c)
{
    g_autofree char* name = file_name(c);
    g_autoptr(GFile) file = name ? g_file_get_child_for_display_name(c->dir, name, NULL) : NULL;
    bool exists = file != NULL && g_file_query_exists(file, NULL);

    if (exists) {
        g_autofree char* text = g_strdup_printf("“%s” already exists.", name);
        gtk_label_set_text(GTK_LABEL(c->status), text);
    } else {
        gtk_label_set_text(GTK_LABEL(c->status), "");
    }
    gtk_widget_set_visible(c->status, exists);
    if (exists)
        gtk_style_context_add_class(gtk_widget_get_style_context(c->entry), "error");
    else
        gtk_style_context_remove_class(gtk_widget_get_style_context(c->entry), "error");
    gtk_dialog_set_response_sensitive(GTK_DIALOG(c->dialog), GTK_RESPONSE_ACCEPT, file != NULL && !exists);
}

static void on_changed(GtkWidget* widget, gpointer data)
{
    (void)widget;
    update(data);
}

static void on_response(GtkDialog* dialog, int response, gpointer data)
{
    Compress* c = data;
    g_autofree char* name = file_name(c);
    g_autoptr(GFile) file = name ? g_file_get_child_for_display_name(c->dir, name, NULL) : NULL;

    if (response == GTK_RESPONSE_ACCEPT && file != NULL && !g_file_query_exists(file, NULL)) {
        g_free(last_ext);
        last_ext = g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(c->format)));
        c->done(file, c->data);
    }
    gtk_widget_destroy(GTK_WIDGET(dialog)); /* frees c */
}

void sfe_compress_ask(GtkWidget* parent, GList* files, GFile* dir, SfeCompressFunc done, gpointer data,
    GDestroyNotify destroy)
{
    Compress* c = g_new0(Compress, 1);
    c->dir = g_object_ref(dir);
    c->done = done;
    c->data = data;
    c->destroy = destroy;
    c->dialog = sfe_dialog_new(parent, "Compress");
    g_object_set_data_full(G_OBJECT(c->dialog), "sfe-compress", c, compress_free);

    gtk_window_set_modal(GTK_WINDOW(c->dialog), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(c->dialog), 420, -1);
    gtk_dialog_add_button(GTK_DIALOG(c->dialog), "Cancel", GTK_RESPONSE_CANCEL);
    GtkWidget* ok = gtk_dialog_add_button(GTK_DIALOG(c->dialog), "Create", GTK_RESPONSE_ACCEPT);
    gtk_style_context_add_class(gtk_widget_get_style_context(ok), "suggested-action");
    gtk_dialog_set_default_response(GTK_DIALOG(c->dialog), GTK_RESPONSE_ACCEPT);

    /* [ name                ][ .zip ▾ ] */
    c->entry = gtk_entry_new();
    c->format = gtk_combo_box_text_new();
    size_t n = 0;
    const SfeArchiveFormat* formats = sfe_archive_formats(&n);
    for (size_t i = 0; i < n; i++) {
        if (sfe_archive_can_compress(formats[i].ext))
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(c->format), formats[i].ext, formats[i].ext);
    }
    if (last_ext == NULL || !gtk_combo_box_set_active_id(GTK_COMBO_BOX(c->format), last_ext))
        gtk_combo_box_set_active(GTK_COMBO_BOX(c->format), 0);

    g_autofree char* initial = default_name(files, dir);
    gtk_entry_set_text(GTK_ENTRY(c->entry), initial);
    for (int i = 2; i < 100; i++) { /* start with a free name: "photos (2)" */
        g_autofree char* name = file_name(c);
        g_autoptr(GFile) file = name ? g_file_get_child_for_display_name(dir, name, NULL) : NULL;
        if (file == NULL || !g_file_query_exists(file, NULL))
            break;
        g_autofree char* numbered = sfe_numbered_name(initial, true, i);
        gtk_entry_set_text(GTK_ENTRY(c->entry), numbered);
    }
    gtk_entry_set_activates_default(GTK_ENTRY(c->entry), TRUE);
    gtk_widget_set_hexpand(c->entry, TRUE);

    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(row), "linked");
    gtk_box_pack_start(GTK_BOX(row), c->entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), c->format, FALSE, FALSE, 0);

    c->status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(c->status), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(c->status), "sfe-error-text");

    GtkWidget* area = gtk_dialog_get_content_area(GTK_DIALOG(c->dialog));
    gtk_container_set_border_width(GTK_CONTAINER(area), 12);
    gtk_box_set_spacing(GTK_BOX(area), 8);
    gtk_box_pack_start(GTK_BOX(area), row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(area), c->status, FALSE, FALSE, 0);

    g_signal_connect(c->entry, "changed", G_CALLBACK(on_changed), c);
    g_signal_connect(c->format, "changed", G_CALLBACK(on_changed), c);
    g_signal_connect(c->dialog, "response", G_CALLBACK(on_response), c);
    gtk_widget_show_all(c->dialog);
    update(c);
    gtk_editable_select_region(GTK_EDITABLE(c->entry), 0, -1);
}
