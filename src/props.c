#include "props.h"

#include "dialogs.h"
#include <stdbool.h>
#include <sys/stat.h>
#include <unistd.h>

#define SIZE_REFRESH_MS 200
#define INFO_ATTRIBUTES                                                        \
    "standard::*,time::modified,time::access,time::created,unix::mode,"        \
    "owner::user,owner::group,unix::uid"

/* ------------------------------------------------------------------------ */
/* Background size count                                                     */
/* ------------------------------------------------------------------------ */

/* Shared between the counting thread and the label refresh timer. */
typedef struct {
    gint ref;
    GCancellable* cancel;
    GMutex lock; /* guards bytes, items, done */
    guint64 bytes;
    int items;
    bool done;
    GList* files;
    GtkLabel* label; /* main thread only; NULL once the dialog is gone */
    guint timer;
} SizeCount;

static gpointer ref_copy(gconstpointer object, gpointer data)
{
    (void)data;
    return g_object_ref((gpointer)object);
}

static void size_count_unref(SizeCount* sc)
{
    if (!g_atomic_int_dec_and_test(&sc->ref))
        return;
    g_object_unref(sc->cancel);
    g_mutex_clear(&sc->lock);
    g_list_free_full(sc->files, g_object_unref);
    g_free(sc);
}

static void count(SizeCount* sc, GFile* file)
{
    g_autoptr(GFileInfo) info = g_file_query_info(file, "standard::type,standard::size,standard::name",
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, sc->cancel, NULL);
    if (info == NULL)
        return;

    bool is_dir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
    g_mutex_lock(&sc->lock);
    sc->items++;
    if (!is_dir)
        sc->bytes += g_file_info_get_size(info);
    g_mutex_unlock(&sc->lock);
    if (!is_dir)
        return;
    g_autoptr(GFileEnumerator) en = g_file_enumerate_children(file, "standard::name",
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, sc->cancel, NULL);
    GFileInfo* child;
    while (en != NULL && (child = g_file_enumerator_next_file(en, sc->cancel, NULL)) != NULL) {
        g_autoptr(GFile) f = g_file_get_child(file, g_file_info_get_name(child));
        count(sc, f);
        g_object_unref(child);
    }
}

static gpointer count_thread(gpointer data)
{
    SizeCount* sc = data;
    for (GList* l = sc->files; l != NULL; l = l->next)
        count(sc, l->data);
    g_mutex_lock(&sc->lock);
    sc->done = true;
    g_mutex_unlock(&sc->lock);
    size_count_unref(sc);
    return NULL;
}

static gboolean refresh_size(gpointer data)
{
    SizeCount* sc = data;
    g_mutex_lock(&sc->lock);
    bool done = sc->done;
    guint64 bytes = sc->bytes;
    int items = sc->items;
    g_mutex_unlock(&sc->lock);
    g_autofree char* size = g_format_size(bytes);
    g_autofree char* text = g_strdup_printf("%s (%d item%s)%s", size, items, items == 1 ? "" : "s",
        done ? "" : " — counting…");

    if (sc->label != NULL)
        gtk_label_set_text(sc->label, text);
    if (!done && sc->label != NULL)
        return G_SOURCE_CONTINUE;
    sc->timer = 0;
    size_count_unref(sc);
    return G_SOURCE_REMOVE;
}

static void on_dialog_destroy(GtkWidget* dialog, gpointer data)
{
    (void)dialog;
    SizeCount* sc = data;
    g_cancellable_cancel(sc->cancel);
    sc->label = NULL;
    size_count_unref(sc);
}

/* Fills `label` with the total size of `files`, updating as it counts. */
static void start_size_count(GtkWidget* dialog, GtkWidget* label, GList* files)
{
    SizeCount* sc = g_new0(SizeCount, 1);

    sc->ref = 3; /* thread, timer, dialog */
    g_mutex_init(&sc->lock);
    sc->cancel = g_cancellable_new();
    sc->files = g_list_copy_deep(files, ref_copy, NULL);
    sc->label = GTK_LABEL(label);
    g_signal_connect(dialog, "destroy", G_CALLBACK(on_dialog_destroy), sc);
    sc->timer = g_timeout_add(SIZE_REFRESH_MS, refresh_size, sc);
    g_thread_unref(g_thread_new("sfe-size", count_thread, sc));
}

/* ------------------------------------------------------------------------ */
/* Layout helpers                                                            */
/* ------------------------------------------------------------------------ */

static GtkWidget* add_row(GtkGrid* grid, int* row, const char* key, const char* value)
{
    GtkWidget* k = gtk_label_new(key);
    GtkWidget* v = gtk_label_new(value);

    gtk_label_set_xalign(GTK_LABEL(k), 1);
    gtk_style_context_add_class(gtk_widget_get_style_context(k), "sfe-placeholder");
    gtk_label_set_xalign(GTK_LABEL(v), 0);
    gtk_label_set_selectable(GTK_LABEL(v), TRUE);
    gtk_label_set_line_wrap(GTK_LABEL(v), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(v), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(v), 48);
    gtk_widget_set_hexpand(v, TRUE);
    gtk_grid_attach(grid, k, 0, *row, 1, 1);
    gtk_grid_attach(grid, v, 1, *row, 1, 1);
    (*row)++;
    return v;
}

static char* format_time(GFileInfo* info, const char* attribute)
{
    if (!g_file_info_has_attribute(info, attribute))
        return NULL;
    g_autoptr(GDateTime) utc = g_date_time_new_from_unix_utc((gint64)g_file_info_get_attribute_uint64(info, attribute));
    g_autoptr(GDateTime) local = g_date_time_to_local(utc);
    return g_date_time_format(local, "%Y-%m-%d %H:%M:%S");
}

/* ------------------------------------------------------------------------ */
/* Permissions                                                               */
/* ------------------------------------------------------------------------ */

typedef struct {
    GFile* file;
    guint32 mode;
    bool reverting; /* ignore the toggle we cause when undoing a failure */
} PermState;

static void perm_state_free(gpointer p)
{
    PermState* ps = p;
    g_object_unref(ps->file);
    g_free(ps);
}

static void on_perm_toggled(GtkToggleButton* check, gpointer data)
{
    PermState* ps = data;
    if (ps->reverting)
        return;
    guint32 bit = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(check), "sfe-bit"));
    guint32 mode = gtk_toggle_button_get_active(check) ? ps->mode | bit : ps->mode & ~bit;
    g_autoptr(GError) error = NULL;

    if (g_file_set_attribute_uint32(ps->file, G_FILE_ATTRIBUTE_UNIX_MODE, mode,
            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, &error)) {
        ps->mode = mode;
        return;
    }
    sfe_show_error(GTK_WIDGET(check), "Cannot change the permissions", error->message);
    ps->reverting = true;
    gtk_toggle_button_set_active(check, (ps->mode & bit) != 0);
    ps->reverting = false;
}

static void add_permissions(GtkGrid* grid, int* row, GtkWidget* dialog, GFile* file, GFileInfo* info)
{
    static const char* const who[] = { "Owner", "Group", "Others" };
    static const char* const what[] = { "Read", "Write", "Execute" };
    static const guint32 bits[3][3] = {
        { S_IRUSR, S_IWUSR, S_IXUSR },
        { S_IRGRP, S_IWGRP, S_IXGRP },
        { S_IROTH, S_IWOTH, S_IXOTH },
    };
    PermState* ps = g_new0(PermState, 1);
    /* Only the owner (or root) may chmod, whether or not the file is writable. */
    guint32 uid = g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_UID);
    bool editable = g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_UID) && (uid == getuid() || getuid() == 0);

    ps->file = g_object_ref(file);
    ps->mode = g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_MODE) & 07777;
    g_object_set_data_full(G_OBJECT(dialog), "sfe-perms", ps, perm_state_free);

    for (int w = 0; w < 3; w++) {
        GtkWidget* label = gtk_label_new(who[w]);
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_label_set_xalign(GTK_LABEL(label), 1);
        gtk_style_context_add_class(gtk_widget_get_style_context(label), "sfe-placeholder");
        for (int p = 0; p < 3; p++) {
            GtkWidget* check = gtk_check_button_new_with_label(what[p]);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), (ps->mode & bits[w][p]) != 0);
            gtk_widget_set_sensitive(check, editable);
            g_object_set_data(G_OBJECT(check), "sfe-bit", GUINT_TO_POINTER(bits[w][p]));
            g_signal_connect(check, "toggled", G_CALLBACK(on_perm_toggled), ps);
            gtk_box_pack_start(GTK_BOX(box), check, FALSE, FALSE, 0);
        }
        gtk_grid_attach(grid, label, 0, *row, 1, 1);
        gtk_grid_attach(grid, box, 1, *row, 1, 1);
        (*row)++;
    }
}

/* ------------------------------------------------------------------------ */
/* The dialog                                                                */
/* ------------------------------------------------------------------------ */

static void fill_single(GtkGrid* grid, int* row, GtkWidget* dialog, GFile* file)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GFileInfo) info = g_file_query_info(file, INFO_ATTRIBUTES, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, &error);
    if (info == NULL) {
        add_row(grid, row, "Error", error->message);
        return;
    }

    g_autoptr(GFile) parent = g_file_get_parent(file);
    const char* type = g_file_info_get_content_type(info);
    g_autofree char* desc = type ? g_content_type_get_description(type) : g_strdup("Unknown");
    g_autofree char* type_text = type ? g_strdup_printf("%s (%s)", desc, type) : g_strdup(desc);
    bool is_dir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;

    add_row(grid, row, "Name", g_file_info_get_display_name(info));
    add_row(grid, row, "Type", type_text);
    if (parent != NULL)
        add_row(grid, row, "Location", g_file_peek_path(parent));
    if (g_file_info_get_is_symlink(info))
        add_row(grid, row, "Link to", g_file_info_get_symlink_target(info));

    if (is_dir) {
        GList one = { .data = file };
        start_size_count(dialog, add_row(grid, row, "Size", "counting…"), &one);
    } else {
        g_autofree char* size = g_format_size_full(g_file_info_get_size(info), G_FORMAT_SIZE_LONG_FORMAT);
        add_row(grid, row, "Size", size);
    }

    const char* labels[] = { "Modified", "Accessed", "Created" };
    const char* attrs[] = { G_FILE_ATTRIBUTE_TIME_MODIFIED, G_FILE_ATTRIBUTE_TIME_ACCESS, G_FILE_ATTRIBUTE_TIME_CREATED };
    for (int i = 0; i < 3; i++) {
        g_autofree char* when = format_time(info, attrs[i]);
        if (when != NULL)
            add_row(grid, row, labels[i], when);
    }

    if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_OWNER_USER)) {
        g_autofree char* owner = g_strdup_printf("%s : %s",
            g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_OWNER_USER),
            g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_OWNER_GROUP));
        add_row(grid, row, "Owned by", owner);
    }
    if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_MODE) && !g_file_info_get_is_symlink(info))
        add_permissions(grid, row, dialog, file, info);
}

static void fill_multiple(GtkGrid* grid, int* row, GtkWidget* dialog, GList* files)
{
    g_autofree char* count = g_strdup_printf("%u items", g_list_length(files));
    g_autoptr(GFile) parent = g_file_get_parent(files->data);

    add_row(grid, row, "Selection", count);
    if (parent != NULL)
        add_row(grid, row, "Location", g_file_peek_path(parent));
    start_size_count(dialog, add_row(grid, row, "Total size", "counting…"), files);
}

void sfe_show_properties(GtkWidget* parent, GList* files)
{
    if (files == NULL)
        return;

    g_autofree char* base = g_file_get_basename(files->data);
    g_autofree char* title = files->next ? g_strdup("Properties") : g_strdup_printf("%s — Properties", base);
    GtkWidget* dialog = sfe_dialog_new(parent, title);
    GtkWidget* grid = gtk_grid_new();
    int row = 0;

    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 16);

    if (files->next == NULL)
        fill_single(GTK_GRID(grid), &row, dialog, files->data);
    else
        fill_multiple(GTK_GRID(grid), &row, dialog, files);

    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), grid, TRUE, TRUE, 0);
    gtk_dialog_add_button(GTK_DIALOG(dialog), "Close", GTK_RESPONSE_CLOSE);
    g_signal_connect(dialog, "response", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_widget_show_all(dialog);
}
