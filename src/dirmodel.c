#include "dirmodel.h"

#include "app.h"
#include "fuzzy.h"
#include "filefilter.h"
#include "icons.h"
#include <cairo-gobject.h>
#include <string.h>

#define BATCH_SIZE 256
#define COLLATE_KEY_ATTR "sfe::collate-key" /* in-memory only, never written */

/* One directory's contents, shared by every pane showing it: the files, the
 * rows without search or file-dialog filter, the thumbnails and the change
 * monitor. A new split on a big folder then neither reads it again nor
 * builds and keeps a second copy of its rows. */
typedef struct {
    int refs; /* attached models, running loads and queries */
    char* uri; /* key in `folders` while it is listed there */
    GFile* dir;
    GList* models; /* SfeDirModel* showing it or waiting for it (not referenced) */
    GCancellable* cancel; /* the load and the monitor queries */
    GtkListStore* store; /* every shown file (NULL until loaded) */
    GHashTable* rows; /* file name -> GtkTreeIter* into store */
    GHashTable* infos; /* file name -> GFileInfo*, every file incl. hidden */
    GHashTable* thumbs; /* file name -> GdkPixbuf*, or NO_THUMB (none to be had) */
    GFileMonitor* monitor;
    bool loading;
    bool building; /* filling fresh stores, not reacting to changes */

    /* How rows look; global settings, so the same in every pane (shortcut:
     * the last pane to set them wins; upgrade if panes ever differ). */
    bool show_hidden;
    SfeSortKey sort_key;
    GtkSortType sort_order;
    bool dirs_first;
    int grid_size, list_size, scale;
} Folder;

static GHashTable* folders; /* uri -> Folder*, those loading or loaded */

struct _SfeDirModel {
    GObject parent;

    Folder* folder; /* the one shown (NULL before the first load) */
    Folder* pending; /* the one being loaded to replace it */
    guint switch_source; /* idle that shows an already loaded `pending` */

    /* Only while searching or with a file-dialog filter: this pane's own
     * rows, built from the folder's files. Otherwise the folder's are shown. */
    GtkListStore* own_store;
    GHashTable* own_rows;
    GHashTable* scores; /* file name -> match score of the query */
    char* query; /* search filter, NULL when not searching */
    int matched; /* files the query matches (may exceed the rows); kept up to date as files come and go */
    const SfeFileFilter* filter; /* a file dialog's type filter (borrowed), or NULL */

    /* Recursive search: the files in the folders below, found by a walk. */
    GPtrArray* deep; /* GFileInfo*, named by their path ("sub/x.txt"); NULL until walked */
    GFile* walk_root; /* the folder walked or being walked, NULL when none */
    GCancellable* walk_cancel;

    /* The settings this pane wants, given to every folder it shows. */
    bool show_hidden;
    SfeSortKey sort_key;
    GtkSortType sort_order;
    bool dirs_first;
    int grid_size, list_size, scale;
};

G_DEFINE_TYPE(SfeDirModel, sfe_dir_model, G_TYPE_OBJECT)

enum {
    SIGNAL_LOADED,
    SIGNAL_ERROR,
    SIGNAL_GONE,
    SIGNAL_FILE_ADDED,
    SIGNAL_FILES_CHANGED,
    SIGNAL_THUMBNAILS_CHANGED,
    N_SIGNALS,
};
static guint signals[N_SIGNALS];

static char no_thumb_marker;
#define NO_THUMB ((gpointer)&no_thumb_marker)

static void thumb_unref(gpointer value)
{
    if (value != NO_THUMB)
        g_object_unref(value);
}

static bool own_rows(SfeDirModel* self)
{
    return self->query != NULL || self->filter != NULL;
}

/* The store and rows the views show. */
static GtkListStore* shown_store(SfeDirModel* self)
{
    return self->own_store ? self->own_store : self->folder ? self->folder->store : NULL;
}

static GHashTable* shown_rows(SfeDirModel* self)
{
    return self->own_store ? self->own_rows : self->folder ? self->folder->rows : NULL;
}

/* ------------------------------------------------------------------------ */
/* Row contents                                                              */
/* ------------------------------------------------------------------------ */

static bool info_is_dir(GFileInfo* info)
{
    return g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
}

static const char* type_description(GFileInfo* info)
{
    static GHashTable* cache; /* content type -> description */
    const char* type = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE);

    if (type == NULL)
        return "";
    if (cache == NULL)
        cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    const char* desc = g_hash_table_lookup(cache, type);
    if (desc == NULL) {
        char* d = g_content_type_get_description(type);
        g_hash_table_insert(cache, g_strdup(type), d);
        desc = d;
    }
    return desc;
}

static char* format_mtime(GFileInfo* info)
{
    g_autoptr(GDateTime) utc = g_file_info_get_modification_date_time(info);
    if (utc == NULL)
        return g_strdup("");
    g_autoptr(GDateTime) local = g_date_time_to_local(utc);
    return g_date_time_format(local, "%Y-%m-%d %H:%M");
}

/* A thumbnail fitted into a `size` square, for `scale`. */
static cairo_surface_t* thumb_surface(GdkPixbuf* pixbuf, int size, int scale)
{
    int box = size * scale;
    int w = gdk_pixbuf_get_width(pixbuf), h = gdk_pixbuf_get_height(pixbuf);
    double k = MIN(1.0, MIN((double)box / w, (double)box / h));
    g_autoptr(GdkPixbuf) fitted = k < 1.0
        ? gdk_pixbuf_scale_simple(pixbuf, MAX(1, (int)(w * k + 0.5)), MAX(1, (int)(h * k + 0.5)), GDK_INTERP_BILINEAR)
        : g_object_ref(pixbuf);
    return gdk_cairo_surface_create_from_pixbuf(fitted, scale, NULL);
}

/* A folder's cover with a small folder icon in its bottom-right corner, so
 * it does not pass for a picture file. Takes `cover`. */
static cairo_surface_t* with_folder_badge(cairo_surface_t* cover, GFileInfo* info, GFile* parent, int size, int scale)
{
    if (size < 32) /* (list view: too small for a badge) */
        return cover;
    double w = (double)cairo_image_surface_get_width(cover) / scale;
    double h = (double)cairo_image_surface_get_height(cover) / scale;
    int badge_size = MAX(14, size * 3 / 8);
    cairo_surface_t* out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, cairo_image_surface_get_width(cover),
        cairo_image_surface_get_height(cover));
    cairo_surface_set_device_scale(out, scale, scale);
    cairo_t* cr = cairo_create(out);
    cairo_set_source_surface(cr, cover, 0, 0);
    cairo_paint(cr);
    cairo_surface_t* badge = sfe_icon_for_info(info, parent, badge_size, scale);
    cairo_set_source_surface(cr, badge, MAX(0, w - badge_size), MAX(0, h - badge_size));
    cairo_paint(cr);
    cairo_surface_destroy(badge);
    cairo_destroy(cr);
    cairo_surface_destroy(cover);
    return out;
}

/* The row's picture: its thumbnail if it has one, else the theme icon.
 * Returns a new reference. A thumbnail is scaled once and kept on the
 * pixbuf, since rows are drawn from this on every paint: one copy for the
 * grid and one for the list, replaced when the size or scale changes. */
static cairo_surface_t* icon_for(Folder* f, GFileInfo* info, int size)
{
    static cairo_user_data_key_t made_for; /* size * 16 + scale */
    gpointer thumb = f->thumbs ? g_hash_table_lookup(f->thumbs, g_file_info_get_name(info)) : NULL;
    if (thumb != NULL && thumb != NO_THUMB) {
        const char* key = size == f->grid_size ? "sfe-grid" : "sfe-list";
        int tag = size * 16 + f->scale;
        cairo_surface_t* surface = g_object_get_data(thumb, key);
        if (surface == NULL || GPOINTER_TO_INT(cairo_surface_get_user_data(surface, &made_for)) != tag) {
            surface = thumb_surface(thumb, size, f->scale);
            if (info_is_dir(info))
                surface = with_folder_badge(surface, info, f->dir, size, f->scale);
            cairo_surface_set_user_data(surface, &made_for, GINT_TO_POINTER(tag), NULL);
            g_object_set_data_full(thumb, key, surface, (GDestroyNotify)cairo_surface_destroy); /* drops the old */
        }
        return cairo_surface_reference(surface);
    }
    return sfe_icon_for_info(info, f->dir, size, f->scale);
}

cairo_surface_t* sfe_dir_model_icon(SfeDirModel* self, GFileInfo* info, bool grid)
{
    Folder* f = self->folder;
    if (f == NULL)
        return NULL;
    return icon_for(f, info, grid ? f->grid_size : f->list_size);
}

char* sfe_dir_model_text(GFileInfo* info, SfeSortKey key)
{
    bool is_dir = info_is_dir(info);
    switch (key) {
    case SFE_SORT_SIZE:
        return is_dir ? g_strdup("") : g_format_size(g_file_info_get_size(info));
    case SFE_SORT_MODIFIED:
        return format_mtime(info);
    case SFE_SORT_TYPE:
        return g_strdup(is_dir ? "Folder" : type_description(info));
    case SFE_SORT_NAME:
        break;
    }
    return g_strdup(g_file_info_get_display_name(info));
}

/* The views draw icons from sfe_dir_model_icon(); tell them a row changed. */
static void row_changed(GtkListStore* store, GtkTreeIter* iter)
{
    g_autoptr(GtkTreePath) path = gtk_tree_model_get_path(GTK_TREE_MODEL(store), iter);
    gtk_tree_model_row_changed(GTK_TREE_MODEL(store), path, iter);
}

/* Writes a whole row in one call, so a new row is inserted complete: an empty
 * row that is filled in later confuses GtkTreeModelFilter. */
static void store_row(Folder* f, GtkListStore* store, GtkTreeIter* iter, bool insert, GFileInfo* info)
{
    g_autoptr(GFile) file = g_file_resolve_relative_path(f->dir, g_file_info_get_name(info)); /* "sub/x.txt" too */
    int columns[] = { SFE_COL_FILE, SFE_COL_INFO, SFE_COL_NAME, SFE_COL_IS_DIR };
    GValue values[G_N_ELEMENTS(columns)] = { G_VALUE_INIT };
    g_value_set_object(g_value_init(&values[0], G_TYPE_FILE), file);
    g_value_set_object(g_value_init(&values[1], G_TYPE_FILE_INFO), info);
    g_value_set_string(g_value_init(&values[2], G_TYPE_STRING), g_file_info_get_display_name(info));
    g_value_set_boolean(g_value_init(&values[3], G_TYPE_BOOLEAN), info_is_dir(info));

    if (insert)
        gtk_list_store_insert_with_valuesv(store, iter, -1, columns, values, G_N_ELEMENTS(columns));
    else
        gtk_list_store_set_valuesv(store, iter, columns, values, G_N_ELEMENTS(columns));

    for (size_t i = 0; i < G_N_ELEMENTS(values); i++)
        g_value_unset(&values[i]);
}

/* Shows, updates or removes the row of `info` in `store`. Returns whether a
 * row was added. */
static bool put_row(Folder* f, GtkListStore* store, GHashTable* rows, GFileInfo* info, bool shown)
{
    const char* name = g_file_info_get_name(info);
    GtkTreeIter* iter = g_hash_table_lookup(rows, name);

    if (!shown) {
        if (iter != NULL) {
            gtk_list_store_remove(store, iter);
            g_hash_table_remove(rows, name);
        }
        return false;
    }
    bool insert = iter == NULL;
    if (insert) {
        iter = g_new(GtkTreeIter, 1);
        g_hash_table_insert(rows, g_strdup(name), iter);
    }
    store_row(f, store, iter, insert, info);
    return insert;
}

static bool passes_hidden(Folder* f, GFileInfo* info)
{
    return f->show_hidden || !g_file_info_get_is_hidden(info);
}

/* In this pane's own rows: matches the file-dialog filter and the query (its
 * score is kept for sorting). */
static bool is_shown(SfeDirModel* self, GFileInfo* info)
{
    if (!passes_hidden(self->folder, info))
        return false;
    if (!sfe_file_filter_shows(self->filter, g_file_info_get_display_name(info),
            g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE), info_is_dir(info)))
        return false;
    if (self->query == NULL)
        return true;
    int score = sfe_fuzzy_score(self->query, g_file_info_get_display_name(info));
    g_hash_table_replace(self->scores, g_strdup(g_file_info_get_name(info)), GINT_TO_POINTER(score));
    return score != SFE_FUZZY_NO_MATCH;
}

/* Updates this pane's own rows for `info`; returns whether a row was added. */
static bool own_put(SfeDirModel* self, GFileInfo* info)
{
    const char* name = g_file_info_get_name(info);
    bool had = g_hash_table_contains(self->own_rows, name);
    bool added = put_row(self->folder, self->own_store, self->own_rows, info, is_shown(self, info));
    if (self->query != NULL && !self->folder->building)
        self->matched += added ? 1 : had && !g_hash_table_contains(self->own_rows, name) ? -1 : 0;
    return added;
}

static void own_remove(SfeDirModel* self, const char* name)
{
    GtkTreeIter* iter = g_hash_table_lookup(self->own_rows, name);
    if (iter == NULL)
        return;
    gtk_list_store_remove(self->own_store, iter);
    g_hash_table_remove(self->own_rows, name);
    if (self->query != NULL)
        self->matched--;
}

/* The panes showing `f` (copied and referenced: signal handlers may load
 * other folders meanwhile). */
static GList* shown_by(Folder* f)
{
    GList* out = NULL;
    for (GList* l = f->models; l != NULL; l = l->next) {
        if (SFE_DIR_MODEL(l->data)->folder == f)
            out = g_list_prepend(out, g_object_ref(l->data));
    }
    return out;
}

/* Remembers `info` and updates its rows everywhere. The stores only hold
 * the rows that are shown: no GtkTreeModelFilter, whose reference counting
 * breaks when shown rows are removed. */
static void add_or_update(Folder* f, GFileInfo* info)
{
    const char* name = g_file_info_get_name(info);
    g_autofree char* collate = g_utf8_collate_key_for_filename(g_file_info_get_display_name(info), -1);
    g_file_info_set_attribute_string(info, COLLATE_KEY_ATTR, collate);

    g_hash_table_replace(f->infos, g_strdup(name), g_object_ref(info));
    if (!f->building)
        g_hash_table_remove(f->thumbs, name); /* changed on disk: a new thumbnail */
    bool added = put_row(f, f->store, f->rows, info, passes_hidden(f, info));
    if (f->building)
        return;

    GList* models = shown_by(f);
    for (GList* l = models; l != NULL; l = l->next) {
        SfeDirModel* m = l->data;
        bool shown_added = m->own_store ? own_put(m, info) : added;
        g_signal_emit(m, signals[SIGNAL_FILES_CHANGED], 0);
        if (shown_added)
            g_signal_emit(m, signals[SIGNAL_FILE_ADDED], 0, name);
    }
    g_list_free_full(models, g_object_unref);
}

static void each_store(Folder* f, void (*fn)(Folder*, GtkListStore*))
{
    if (f->store != NULL)
        fn(f, f->store);
    for (GList* l = f->models; l != NULL; l = l->next) {
        SfeDirModel* m = l->data;
        if (m->folder == f && m->own_store != NULL)
            fn(f, m->own_store);
    }
}

static void refresh_store_icons(Folder* f, GtkListStore* store)
{
    (void)f;
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &iter))
        return;
    do
        row_changed(store, &iter);
    while (gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &iter));
}

static void refresh_icons(Folder* f)
{
    each_store(f, refresh_store_icons);
}

/* ------------------------------------------------------------------------ */
/* Sorting                                                                   */
/* ------------------------------------------------------------------------ */

/* The store keeps its own reference, so the returned info is borrowed. */
static GFileInfo* row_info(GtkTreeModel* model, GtkTreeIter* iter)
{
    GFileInfo* info = NULL;
    gtk_tree_model_get(model, iter, SFE_COL_INFO, &info, -1);
    if (info != NULL)
        g_object_unref(info);
    return info;
}

static int compare_numbers(guint64 a, guint64 b)
{
    return (a > b) - (a < b);
}

static int score_of(GHashTable* scores, GFileInfo* info)
{
    return GPOINTER_TO_INT(g_hash_table_lookup(scores, g_file_info_get_name(info)));
}

/* `scores` while searching (the best match comes first, whatever the sort). */
static int compare_infos(Folder* f, GHashTable* scores, GFileInfo* a, GFileInfo* b)
{
    int result = 0;

    if (scores != NULL) {
        result = compare_numbers(score_of(scores, b), score_of(scores, a));
        if (result != 0)
            return f->sort_order == GTK_SORT_DESCENDING ? -result : result;
    }

    if (f->dirs_first && info_is_dir(a) != info_is_dir(b)) {
        /* Directories stay on top in both sort orders. The store inverts
         * our result for descending order, so pre-invert it here. */
        result = info_is_dir(a) ? -1 : 1;
        return f->sort_order == GTK_SORT_DESCENDING ? -result : result;
    }

    switch (f->sort_key) {
    case SFE_SORT_SIZE:
        result = compare_numbers(g_file_info_get_size(a), g_file_info_get_size(b));
        break;
    case SFE_SORT_MODIFIED:
        result = compare_numbers(
            g_file_info_get_attribute_uint64(a, G_FILE_ATTRIBUTE_TIME_MODIFIED),
            g_file_info_get_attribute_uint64(b, G_FILE_ATTRIBUTE_TIME_MODIFIED));
        break;
    case SFE_SORT_TYPE:
        result = g_strcmp0(
            g_file_info_get_attribute_string(a, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE),
            g_file_info_get_attribute_string(b, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE));
        break;
    case SFE_SORT_NAME:
        break;
    }
    if (result == 0)
        result = g_strcmp0(g_file_info_get_attribute_string(a, COLLATE_KEY_ATTR),
            g_file_info_get_attribute_string(b, COLLATE_KEY_ATTR));
    return result;
}

static int compare_folder_rows(GtkTreeModel* model, GtkTreeIter* ia, GtkTreeIter* ib, gpointer data)
{
    return compare_infos(data, NULL, row_info(model, ia), row_info(model, ib));
}

static int compare_own_rows(GtkTreeModel* model, GtkTreeIter* ia, GtkTreeIter* ib, gpointer data)
{
    SfeDirModel* self = data;
    return compare_infos(self->folder, self->query ? self->scores : NULL, row_info(model, ia), row_info(model, ib));
}

static void sort_store(Folder* f, GtkListStore* store)
{
    GtkTreeSortable* sortable = GTK_TREE_SORTABLE(store);
    /* Going through "unsorted" forces a resort even when the column and the
     * order did not change (e.g. only dirs_first did). */
    gtk_tree_sortable_set_sort_column_id(sortable, GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID, f->sort_order);
    gtk_tree_sortable_set_sort_column_id(sortable, 0, f->sort_order);
}

/* ------------------------------------------------------------------------ */
/* Folders                                                                   */
/* ------------------------------------------------------------------------ */

static GtkListStore* new_store(void)
{
    return gtk_list_store_new(SFE_N_COLUMNS,
        G_TYPE_FILE, G_TYPE_FILE_INFO, G_TYPE_STRING, G_TYPE_BOOLEAN);
}

static GHashTable* new_rows(void)
{
    return g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
}

#ifdef __GLIBC__
#include <malloc.h>

/* glibc keeps freed memory for reuse instead of returning it to the system,
 * so after leaving (or closing) a folder of 100,000 files sfe kept ~150 MB
 * it no longer used. Once the views have let go of the old rows (an idle
 * later), the free memory is handed back. */
#define BIG_FOLDER_FILES 5000

static guint trim_source;

static gboolean trim_memory(gpointer data)
{
    (void)data;
    trim_source = 0;
    malloc_trim(0);
    return G_SOURCE_REMOVE;
}

static void release_freed_memory(guint dropped_files)
{
    if (dropped_files >= BIG_FOLDER_FILES && trim_source == 0)
        trim_source = g_idle_add_full(G_PRIORITY_LOW, trim_memory, NULL, NULL);
}
#else
static void release_freed_memory(guint dropped_files) { (void)dropped_files; }
#endif

static void on_icon_theme_changed(GtkIconTheme* theme, gpointer data)
{
    (void)theme;
    refresh_icons(data);
}

static void stop_monitor(Folder* f)
{
    if (f->monitor != NULL) {
        g_signal_handlers_disconnect_by_data(f->monitor, f);
        g_clear_object(&f->monitor);
    }
}

static void clear_contents(Folder* f)
{
    release_freed_memory(f->infos ? g_hash_table_size(f->infos) : 0);
    stop_monitor(f);
    g_clear_object(&f->store);
    g_clear_pointer(&f->rows, g_hash_table_unref);
    g_clear_pointer(&f->infos, g_hash_table_unref);
    g_clear_pointer(&f->thumbs, g_hash_table_unref);
}

static Folder* folder_ref(Folder* f)
{
    f->refs++;
    return f;
}

/* No longer listed: the next pane to open the directory reads it afresh. */
static void folder_unlist(Folder* f)
{
    if (f->uri != NULL && folders != NULL && g_hash_table_lookup(folders, f->uri) == f)
        g_hash_table_remove(folders, f->uri);
    g_clear_pointer(&f->uri, g_free);
}

static void folder_unref(Folder* f)
{
    if (--f->refs > 0)
        return;
    folder_unlist(f);
    g_signal_handlers_disconnect_by_data(gtk_icon_theme_get_default(), f);
    clear_contents(f);
    g_clear_object(&f->cancel);
    g_clear_object(&f->dir);
    g_free(f);
}

static Folder* folder_get(GFile* dir)
{
    if (folders == NULL)
        folders = g_hash_table_new(g_str_hash, g_str_equal);
    g_autofree char* uri = g_file_get_uri(dir);
    Folder* f = g_hash_table_lookup(folders, uri);
    if (f != NULL)
        return folder_ref(f);

    f = g_new0(Folder, 1);
    f->refs = 1;
    f->uri = g_steal_pointer(&uri);
    f->dir = g_object_ref(dir);
    f->sort_order = GTK_SORT_ASCENDING;
    f->dirs_first = true;
    f->grid_size = 64;
    f->list_size = 16;
    f->scale = 1;
    /* Connected "after" so the icon cache is already cleared. */
    g_signal_connect_after(gtk_icon_theme_get_default(), "changed", G_CALLBACK(on_icon_theme_changed), f);
    g_hash_table_insert(folders, f->uri, f);
    return f;
}

/* A pane stops showing or waiting for `f`. The last one cancels its load. */
static void folder_detach(Folder* f, SfeDirModel* model)
{
    f->models = g_list_remove(f->models, model);
    if (f->models == NULL) {
        folder_unlist(f);
        if (f->cancel != NULL)
            g_cancellable_cancel(f->cancel);
        stop_monitor(f);
    }
    folder_unref(f);
}

/* ------------------------------------------------------------------------ */
/* Directory monitor                                                         */
/* ------------------------------------------------------------------------ */

static void on_query_done(GObject* source, GAsyncResult* res, gpointer data)
{
    Folder* f = data;
    g_autoptr(GFileInfo) info = g_file_query_info_finish(G_FILE(source), res, NULL);

    /* Ignore a file renamed or deleted meanwhile (that event was already
     * handled: adding it now would leave a ghost, e.g. a download's .part). */
    if (info != NULL && f->store != NULL && g_file_query_exists(G_FILE(source), NULL))
        add_or_update(f, info);
    folder_unref(f);
}

static void query_and_update(Folder* f, GFile* file)
{
    g_file_query_info_async(file, SFE_FILE_ATTRIBUTES, G_FILE_QUERY_INFO_NONE,
        G_PRIORITY_DEFAULT, f->cancel, on_query_done, folder_ref(f));
}

static bool is_child(Folder* f, GFile* file)
{
    g_autoptr(GFile) parent = file ? g_file_get_parent(file) : NULL;
    return parent != NULL && g_file_equal(parent, f->dir);
}

static void remove_file(Folder* f, const char* name)
{
    if (f->infos == NULL || !g_hash_table_contains(f->infos, name))
        return;
    GtkTreeIter* iter = g_hash_table_lookup(f->rows, name);
    if (iter != NULL) {
        gtk_list_store_remove(f->store, iter);
        g_hash_table_remove(f->rows, name);
    }
    g_hash_table_remove(f->infos, name);
    g_hash_table_remove(f->thumbs, name);

    GList* models = shown_by(f);
    for (GList* l = models; l != NULL; l = l->next) {
        SfeDirModel* m = l->data;
        if (m->own_store != NULL)
            own_remove(m, name);
        g_signal_emit(m, signals[SIGNAL_FILES_CHANGED], 0);
    }
    g_list_free_full(models, g_object_unref);
}

static void on_dir_changed(GFileMonitor* monitor, GFile* file, GFile* other,
    GFileMonitorEvent event, gpointer data)
{
    (void)monitor;
    Folder* f = data;
    g_autofree char* name = g_file_get_basename(file);

    if (g_file_equal(file, f->dir)) {
        if (event == G_FILE_MONITOR_EVENT_DELETED || event == G_FILE_MONITOR_EVENT_MOVED_OUT
            || event == G_FILE_MONITOR_EVENT_UNMOUNTED) {
            folder_unlist(f); /* if it comes back, read it again */
            GList* models = shown_by(f);
            for (GList* l = models; l != NULL; l = l->next)
                g_signal_emit(l->data, signals[SIGNAL_GONE], 0);
            g_list_free_full(models, g_object_unref);
        }
        return;
    }

    switch (event) {
    case G_FILE_MONITOR_EVENT_CREATED:
    case G_FILE_MONITOR_EVENT_MOVED_IN:
    case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT:
    case G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED:
        query_and_update(f, file);
        break;
    case G_FILE_MONITOR_EVENT_DELETED:
    case G_FILE_MONITOR_EVENT_MOVED_OUT:
        remove_file(f, name);
        break;
    case G_FILE_MONITOR_EVENT_RENAMED:
        remove_file(f, name);
        if (is_child(f, other))
            query_and_update(f, other);
        break;
    default:
        break;
    }
}

static void start_monitor(Folder* f)
{
    g_autoptr(GError) error = NULL;

    f->monitor = g_file_monitor_directory(f->dir, G_FILE_MONITOR_WATCH_MOVES, f->cancel, &error);
    if (f->monitor == NULL) {
        g_debug("not watching %s: %s", g_file_peek_path(f->dir), error->message);
        return;
    }
    g_signal_connect(f->monitor, "changed", G_CALLBACK(on_dir_changed), f);
}

/* ------------------------------------------------------------------------ */
/* A pane's own rows (search, file-dialog filter)                            */
/* ------------------------------------------------------------------------ */

/* Better match first; equal ones by name, so the cut is always the same. */
static GHashTable* sort_scores; /* for best_first (g_ptr_array_sort has no user data) */

static int best_first(gconstpointer pa, gconstpointer pb)
{
    GFileInfo* a = *(GFileInfo* const*)pa;
    GFileInfo* b = *(GFileInfo* const*)pb;
    int result = compare_numbers(score_of(sort_scores, b), score_of(sort_scores, a));
    return result != 0 ? result : g_strcmp0(g_file_info_get_name(a), g_file_info_get_name(b));
}

/* While searching, only the SFE_SEARCH_MAX_SHOWN best matches are shown:
 * GTK lays out every shown row again after each change (about 2 s for
 * 20,000), so a big folder froze at every key. Returns the infos to show. */
static GPtrArray* shown_infos(SfeDirModel* self)
{
    GPtrArray* shown = g_ptr_array_new();
    GHashTableIter it;
    gpointer info;
    g_hash_table_iter_init(&it, self->folder->infos);
    while (g_hash_table_iter_next(&it, NULL, &info)) {
        if (is_shown(self, info)) /* (scores it) */
            g_ptr_array_add(shown, info);
    }
    for (guint i = 0; self->query != NULL && self->deep != NULL && i < self->deep->len; i++) {
        if (is_shown(self, g_ptr_array_index(self->deep, i)))
            g_ptr_array_add(shown, g_ptr_array_index(self->deep, i));
    }
    self->matched = (int)shown->len;
    if (self->query != NULL && shown->len > SFE_SEARCH_MAX_SHOWN) {
        sort_scores = self->scores;
        g_ptr_array_sort(shown, best_first);
        sort_scores = NULL;
        g_ptr_array_set_size(shown, SFE_SEARCH_MAX_SHOWN);
    }
    return shown;
}

/* Builds the pane's own rows from the folder's files (search or filter),
 * or drops them to show the folder's. The caller emits "loaded". */
static void rebuild_own(SfeDirModel* self)
{
    g_clear_object(&self->own_store);
    g_clear_pointer(&self->own_rows, g_hash_table_unref);
    g_hash_table_remove_all(self->scores);
    if (self->folder == NULL || self->folder->infos == NULL || !own_rows(self))
        return;

    Folder* f = self->folder;
    g_autoptr(GPtrArray) infos = shown_infos(self);
    self->own_store = new_store();
    self->own_rows = new_rows();
    for (guint i = 0; i < infos->len; i++)
        put_row(f, self->own_store, self->own_rows, g_ptr_array_index(infos, i), true);
    gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(self->own_store), 0, compare_own_rows, self, NULL);
    sort_store(f, self->own_store);
}

/* Recursive search walks the folders below in a thread, breadth first:
 * not into symlinks, other file systems (/proc, mounted drives) or hidden
 * folders (unless hidden files are shown). Every file found is named by its
 * path from the folder, so the query can match "photos/2024". */
#define WALK_MAX 100000 /* shortcut: files beyond this are not searched; raise if trees that big matter */

typedef struct {
    GFile* root;
    bool hidden;
} WalkJob;

static void walk_job_free(WalkJob* job)
{
    g_object_unref(job->root);
    g_free(job);
}

static void walk_thread(GTask* task, gpointer source, gpointer data, GCancellable* cancel)
{
    (void)source;
    WalkJob* job = data;
    GPtrArray* found = g_ptr_array_new_with_free_func(g_object_unref);
    g_autoptr(GFileInfo) root_info = g_file_query_info(job->root, G_FILE_ATTRIBUTE_UNIX_DEVICE,
        G_FILE_QUERY_INFO_NONE, cancel, NULL);
    guint32 device = root_info ? g_file_info_get_attribute_uint32(root_info, G_FILE_ATTRIBUTE_UNIX_DEVICE) : 0;
    GQueue dirs = G_QUEUE_INIT; /* paths from the root, "" the root itself */
    g_queue_push_tail(&dirs, g_strdup(""));

    while (!g_queue_is_empty(&dirs) && found->len < WALK_MAX && !g_cancellable_is_cancelled(cancel)) {
        g_autofree char* rel = g_queue_pop_head(&dirs);
        g_autoptr(GFile) dir = g_file_resolve_relative_path(job->root, rel);
        g_autoptr(GFileEnumerator) e = g_file_enumerate_children(dir, SFE_FILE_ATTRIBUTES "," G_FILE_ATTRIBUTE_UNIX_DEVICE,
            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancel, NULL);
        GFileInfo* info;
        while (e != NULL && found->len < WALK_MAX && (info = g_file_enumerator_next_file(e, cancel, NULL)) != NULL) {
            char* path = *rel ? g_build_filename(rel, g_file_info_get_name(info), NULL) : g_strdup(g_file_info_get_name(info));
            if (info_is_dir(info) && !g_file_info_get_is_symlink(info) && (job->hidden || !g_file_info_get_is_hidden(info))
                && g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_DEVICE) == device)
                g_queue_push_tail(&dirs, g_strdup(path));
            if (*rel == '\0') { /* the folder's own files are shown already */
                g_free(path);
                g_object_unref(info);
                continue;
            }
            g_autofree char* display = g_filename_display_name(path);
            g_autofree char* collate = g_utf8_collate_key_for_filename(display, -1);
            g_file_info_set_name(info, path);
            g_file_info_set_display_name(info, display);
            g_file_info_set_attribute_string(info, COLLATE_KEY_ATTR, collate);
            g_ptr_array_add(found, info);
            g_free(path);
        }
    }
    g_queue_clear_full(&dirs, g_free);
    g_task_return_pointer(task, found, (GDestroyNotify)g_ptr_array_unref);
}



static void stop_walk(SfeDirModel* self)
{
    if (self->walk_cancel != NULL)
        g_cancellable_cancel(self->walk_cancel);
    g_clear_object(&self->walk_cancel);
    g_clear_object(&self->walk_root);
    if (self->deep != NULL)
        release_freed_memory(self->deep->len);
    g_clear_pointer(&self->deep, g_ptr_array_unref);
}

static void refilter(SfeDirModel* self);

static void on_walked(GObject* source, GAsyncResult* res, gpointer data)
{
    (void)data;
    SfeDirModel* self = SFE_DIR_MODEL(source);
    GCancellable* cancel = g_task_get_cancellable(G_TASK(res));
    GPtrArray* found = g_task_propagate_pointer(G_TASK(res), NULL);
    if (g_cancellable_is_cancelled(cancel) || cancel != self->walk_cancel) { /* superseded */
        if (found != NULL)
            g_ptr_array_unref(found);
        return;
    }
    self->deep = found;
    refilter(self);
}

/* Starts, keeps or stops the walk to match the query, the setting and the
 * folder shown. */
static void update_walk(SfeDirModel* self)
{
    bool want = self->query != NULL && self->folder != NULL && sfe_config()->search_recursive;
    if (want && self->walk_root != NULL && g_file_equal(self->walk_root, self->folder->dir))
        return;
    stop_walk(self);
    if (!want)
        return;
    self->walk_root = g_object_ref(self->folder->dir);
    self->walk_cancel = g_cancellable_new();
    WalkJob* job = g_new0(WalkJob, 1);
    job->root = g_object_ref(self->folder->dir);
    job->hidden = self->show_hidden;
    GTask* task = g_task_new(self, self->walk_cancel, on_walked, NULL);
    g_task_set_task_data(task, job, (GDestroyNotify)walk_job_free);
    g_task_run_in_thread(task, walk_thread);
    g_object_unref(task);
}

static void refilter(SfeDirModel* self)
{
    if (self->folder == NULL || self->folder->store == NULL)
        return;
    rebuild_own(self);
    g_signal_emit(self, signals[SIGNAL_LOADED], 0);
}

/* ------------------------------------------------------------------------ */
/* Loading                                                                   */
/* ------------------------------------------------------------------------ */

/* The pane's settings, given to a folder it shows; changes there are redone. */
static void apply_settings(SfeDirModel* self, Folder* f);

/* Shows `pending` (loaded) instead of the current folder. */
static void switch_folder(SfeDirModel* self)
{
    Folder* old = self->folder;
    self->folder = g_steal_pointer(&self->pending);
    g_clear_handle_id(&self->switch_source, g_source_remove);
    apply_settings(self, self->folder);
    update_walk(self);
    rebuild_own(self);
    g_signal_emit(self, signals[SIGNAL_LOADED], 0);
    if (old != NULL)
        folder_detach(old, self);
}

static gboolean switch_later(gpointer data)
{
    SfeDirModel* self = data;
    self->switch_source = 0;
    if (self->pending != NULL && self->pending->store != NULL)
        switch_folder(self);
    return G_SOURCE_REMOVE;
}

typedef struct {
    Folder* folder;
    GCancellable* cancel;
    GPtrArray* infos; /* GFileInfo* */
} LoadJob;

static void load_job_free(LoadJob* job)
{
    folder_unref(job->folder);
    g_object_unref(job->cancel);
    g_ptr_array_unref(job->infos);
    g_free(job);
}

/* Builds the whole store at once, then sorts it once: much faster than
 * inserting rows one by one into a sorted, displayed model. */
static void finish_load(LoadJob* job)
{
    Folder* f = job->folder;

    if (g_cancellable_is_cancelled(job->cancel)) /* superseded by a newer load */
        return;

    f->loading = false;
    clear_contents(f);
    f->store = new_store();
    f->rows = new_rows();
    f->infos = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
    f->thumbs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, thumb_unref);
    f->building = true;
    for (guint i = 0; i < job->infos->len; i++)
        add_or_update(f, g_ptr_array_index(job->infos, i));
    gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(f->store), 0, compare_folder_rows, f, NULL);
    sort_store(f, f->store);
    start_monitor(f);
    f->building = false;

    /* Every pane waiting for it shows it now; the ones showing it (a
     * reload) get their own rows rebuilt. */
    GList* models = NULL;
    for (GList* l = f->models; l != NULL; l = l->next)
        models = g_list_prepend(models, g_object_ref(l->data));
    for (GList* l = models; l != NULL; l = l->next) {
        SfeDirModel* m = l->data;
        if (m->pending == f) {
            switch_folder(m);
        } else if (m->folder == f) {
            rebuild_own(m);
            g_signal_emit(m, signals[SIGNAL_LOADED], 0);
        }
    }
    g_list_free_full(models, g_object_unref);
}

static void fail_load(LoadJob* job, GError* error)
{
    Folder* f = job->folder;
    if (!g_cancellable_is_cancelled(job->cancel)) {
        f->loading = false;
        folder_unlist(f); /* the next try reads it again */
        GList* models = NULL;
        for (GList* l = f->models; l != NULL; l = l->next)
            models = g_list_prepend(models, g_object_ref(l->data));
        for (GList* l = models; l != NULL; l = l->next) {
            SfeDirModel* m = l->data;
            if (m->pending == f) { /* keeps showing what it showed */
                m->pending = NULL;
                folder_detach(f, m);
            }
            if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                g_signal_emit(m, signals[SIGNAL_ERROR], 0, error);
        }
        g_list_free_full(models, g_object_unref);
    }
    load_job_free(job);
}

static void on_next_files(GObject* source, GAsyncResult* res, gpointer data)
{
    LoadJob* job = data;
    GFileEnumerator* enumerator = G_FILE_ENUMERATOR(source);
    g_autoptr(GError) error = NULL;
    GList* files = g_file_enumerator_next_files_finish(enumerator, res, &error);

    if (error != NULL) {
        fail_load(job, error);
        g_object_unref(enumerator);
        return;
    }
    if (files == NULL) { /* end of directory */
        g_file_enumerator_close_async(enumerator, G_PRIORITY_DEFAULT, NULL, NULL, NULL);
        g_object_unref(enumerator);
        finish_load(job);
        load_job_free(job);
        return;
    }
    for (GList* l = files; l != NULL; l = l->next)
        g_ptr_array_add(job->infos, l->data); /* takes the reference */
    g_list_free(files);

    g_file_enumerator_next_files_async(enumerator, BATCH_SIZE, G_PRIORITY_DEFAULT,
        job->cancel, on_next_files, job);
}

static void on_enumerate(GObject* source, GAsyncResult* res, gpointer data)
{
    LoadJob* job = data;
    g_autoptr(GError) error = NULL;
    GFileEnumerator* enumerator = g_file_enumerate_children_finish(G_FILE(source), res, &error);

    if (enumerator == NULL) {
        fail_load(job, error);
        return;
    }
    g_file_enumerator_next_files_async(enumerator, BATCH_SIZE, G_PRIORITY_DEFAULT,
        job->cancel, on_next_files, job);
}

/* Reads the folder (again); a load still running is cancelled. */
static void folder_load(Folder* f)
{
    LoadJob* job = g_new0(LoadJob, 1);

    if (f->cancel != NULL)
        g_cancellable_cancel(f->cancel);
    g_clear_object(&f->cancel);
    f->cancel = g_cancellable_new();
    f->loading = true;

    job->folder = folder_ref(f);
    job->cancel = g_object_ref(f->cancel);
    job->infos = g_ptr_array_new_with_free_func(g_object_unref);
    g_file_enumerate_children_async(f->dir, SFE_FILE_ATTRIBUTES, G_FILE_QUERY_INFO_NONE,
        G_PRIORITY_DEFAULT, job->cancel, on_enumerate, job);
}

void sfe_dir_model_load(SfeDirModel* self, GFile* dir)
{
    if (self->folder != NULL && self->pending == NULL && g_file_equal(self->folder->dir, dir)
        && self->folder->uri != NULL) {
        folder_load(self->folder); /* the same folder: read it again (every pane showing it) */
        return;
    }
    if (self->pending != NULL && g_file_equal(self->pending->dir, dir))
        return; /* already on its way */
    if (self->pending != NULL) { /* superseded */
        g_clear_handle_id(&self->switch_source, g_source_remove);
        folder_detach(g_steal_pointer(&self->pending), self);
    }

    Folder* f = folder_get(dir);
    f->models = g_list_prepend(f->models, self);
    self->pending = f;
    /* A folder another pane shows gets this pane's settings only when it is
     * shown here (switch_folder): by then the scale factor is known, and a
     * wrong one would redraw every icon of a big folder twice. */
    if (f->store == NULL)
        apply_settings(self, f);
    if (f->store != NULL && !f->loading)
        self->switch_source = g_idle_add(switch_later, self); /* already read: no disk access */
    else if (!f->loading)
        folder_load(f);
}

void sfe_dir_model_reload(SfeDirModel* self)
{
    Folder* f = self->pending ? self->pending : self->folder;
    if (f != NULL)
        sfe_dir_model_load(self, f->dir);
}

/* ------------------------------------------------------------------------ */
/* Public accessors                                                          */
/* ------------------------------------------------------------------------ */

GtkTreeModel* sfe_dir_model_get_model(SfeDirModel* self)
{
    return GTK_TREE_MODEL(shown_store(self));
}

void sfe_dir_model_set_query(SfeDirModel* self, const char* query)
{
    const char* q = query && *query ? query : NULL;
    if (g_strcmp0(q, self->query) == 0)
        return;
    g_free(self->query);
    self->query = g_strdup(q);
    update_walk(self);
    refilter(self);
}

const char* sfe_dir_model_get_query(SfeDirModel* self)
{
    return self->query;
}

void sfe_dir_model_get_counts(SfeDirModel* self, int* shown, int* matched, int* total)
{
    GHashTable* rows = shown_rows(self);
    *shown = rows ? (int)g_hash_table_size(rows) : 0;
    *matched = self->query != NULL ? MAX(self->matched, *shown) : *shown;
    *total = 0;
    if (self->folder == NULL || self->folder->infos == NULL)
        return;
    GHashTableIter it;
    gpointer info;
    g_hash_table_iter_init(&it, self->folder->infos);
    while (g_hash_table_iter_next(&it, NULL, &info))
        *total += passes_hidden(self->folder, info);
    for (guint i = 0; self->query != NULL && self->deep != NULL && i < self->deep->len; i++)
        *total += passes_hidden(self->folder, g_ptr_array_index(self->deep, i));
}

void sfe_dir_model_set_filter(SfeDirModel* self, const SfeFileFilter* filter)
{
    if (self->filter == filter)
        return;
    self->filter = filter;
    refilter(self);
}

/* Hidden files appear or disappear in place, in every pane showing `f`. */
static void folder_set_hidden(Folder* f, bool show)
{
    if (f->show_hidden == show)
        return;
    f->show_hidden = show;
    if (f->infos == NULL)
        return;
    GList* models = shown_by(f);
    GHashTableIter it;
    gpointer info;
    g_hash_table_iter_init(&it, f->infos);
    while (g_hash_table_iter_next(&it, NULL, &info)) {
        put_row(f, f->store, f->rows, info, passes_hidden(f, info));
        for (GList* l = models; l != NULL; l = l->next) {
            SfeDirModel* m = l->data;
            if (m->own_store != NULL)
                own_put(m, info);
        }
    }
    for (GList* l = models; l != NULL; l = l->next)
        g_signal_emit(l->data, signals[SIGNAL_FILES_CHANGED], 0); /* thumbnails for the new rows */
    g_list_free_full(models, g_object_unref);
}

static void folder_set_sort(Folder* f, SfeSortKey key, GtkSortType order, bool dirs_first)
{
    if (f->sort_key == key && f->sort_order == order && f->dirs_first == dirs_first)
        return;
    f->sort_key = key;
    f->sort_order = order;
    f->dirs_first = dirs_first;
    each_store(f, sort_store);
}

static void folder_set_icon_sizes(Folder* f, int grid, int list, int scale)
{
    if (f->grid_size == grid && f->list_size == list && f->scale == scale)
        return;
    f->grid_size = grid;
    f->list_size = list;
    f->scale = scale;
    refresh_icons(f);
}

static void apply_settings(SfeDirModel* self, Folder* f)
{
    folder_set_hidden(f, self->show_hidden);
    folder_set_sort(f, self->sort_key, self->sort_order, self->dirs_first);
    folder_set_icon_sizes(f, self->grid_size, self->list_size, self->scale);
}

void sfe_dir_model_set_show_hidden(SfeDirModel* self, bool show)
{
    self->show_hidden = show;
    if (self->folder != NULL)
        folder_set_hidden(self->folder, show);
}

void sfe_dir_model_set_sort(SfeDirModel* self, SfeSortKey key, GtkSortType order, bool dirs_first)
{
    self->sort_key = key;
    self->sort_order = order;
    self->dirs_first = dirs_first;
    if (self->folder != NULL)
        folder_set_sort(self->folder, key, order, dirs_first);
}

void sfe_dir_model_get_sort(SfeDirModel* self, SfeSortKey* key, GtkSortType* order)
{
    *key = self->sort_key;
    *order = self->sort_order;
}

void sfe_dir_model_set_icon_sizes(SfeDirModel* self, int grid, int list, int scale)
{
    self->grid_size = grid;
    self->list_size = list;
    self->scale = scale;
    if (self->folder != NULL)
        folder_set_icon_sizes(self->folder, grid, list, scale);
}

bool sfe_dir_model_wants_thumbnail(SfeDirModel* self, const char* name)
{
    return self->folder != NULL && self->folder->thumbs != NULL && strchr(name, '/') == NULL /* (a recursive search's: shortcut, no thumbnails) */
        && !g_hash_table_contains(self->folder->thumbs, name);
}

void sfe_dir_model_set_thumbnail(SfeDirModel* self, GFile* file, GdkPixbuf* pixbuf)
{
    Folder* f = self->folder;
    g_autoptr(GFile) parent = g_file_get_parent(file);
    if (f == NULL || f->thumbs == NULL || parent == NULL || !g_file_equal(parent, f->dir))
        return; /* from a folder no longer shown */
    g_autofree char* name = g_file_get_basename(file);
    g_hash_table_replace(f->thumbs, g_strdup(name), pixbuf ? g_object_ref(pixbuf) : NO_THUMB);
    if (pixbuf == NULL || !g_hash_table_contains(f->infos, name))
        return;
    GList* models = shown_by(f);
    for (GList* l = models; l != NULL; l = l->next)
        g_signal_emit(l->data, signals[SIGNAL_THUMBNAILS_CHANGED], 0);
    g_list_free_full(models, g_object_unref);
}

void sfe_dir_model_clear_thumbnails(SfeDirModel* self)
{
    if (self->folder == NULL || self->folder->thumbs == NULL)
        return;
    g_hash_table_remove_all(self->folder->thumbs);
    refresh_icons(self->folder);
}

GtkTreePath* sfe_dir_model_find(SfeDirModel* self, const char* name)
{
    GHashTable* rows = shown_rows(self);
    GtkTreeIter* iter = rows ? g_hash_table_lookup(rows, name) : NULL;
    return iter ? gtk_tree_model_get_path(GTK_TREE_MODEL(shown_store(self)), iter) : NULL;
}

GFile* sfe_dir_model_file_at(SfeDirModel* self, GtkTreePath* path)
{
    GtkTreeModel* model = GTK_TREE_MODEL(shown_store(self));
    GtkTreeIter iter;
    GFile* file = NULL;

    if (model != NULL && gtk_tree_model_get_iter(model, &iter, path))
        gtk_tree_model_get(model, &iter, SFE_COL_FILE, &file, -1);
    return file;
}

GFileInfo* sfe_dir_model_info_at(SfeDirModel* self, GtkTreePath* path)
{
    GtkTreeModel* model = GTK_TREE_MODEL(shown_store(self));
    GtkTreeIter iter;
    GFileInfo* info = NULL;

    if (model != NULL && gtk_tree_model_get_iter(model, &iter, path))
        gtk_tree_model_get(model, &iter, SFE_COL_INFO, &info, -1);
    return info;
}

/* ------------------------------------------------------------------------ */
/* GObject                                                                   */
/* ------------------------------------------------------------------------ */

static void sfe_dir_model_dispose(GObject* object)
{
    SfeDirModel* self = SFE_DIR_MODEL(object);

    g_clear_handle_id(&self->switch_source, g_source_remove);
    stop_walk(self);
    g_clear_object(&self->own_store);
    g_clear_pointer(&self->own_rows, g_hash_table_unref);
    if (self->pending != NULL)
        folder_detach(g_steal_pointer(&self->pending), self);
    if (self->folder != NULL)
        folder_detach(g_steal_pointer(&self->folder), self);
    g_clear_pointer(&self->scores, g_hash_table_unref);
    g_clear_pointer(&self->query, g_free);
    G_OBJECT_CLASS(sfe_dir_model_parent_class)->dispose(object);
}

static void sfe_dir_model_class_init(SfeDirModelClass* klass)
{
    G_OBJECT_CLASS(klass)->dispose = sfe_dir_model_dispose;

    signals[SIGNAL_LOADED] = g_signal_new("loaded", G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    signals[SIGNAL_ERROR] = g_signal_new("error", G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_ERROR);
    signals[SIGNAL_GONE] = g_signal_new("gone", G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    signals[SIGNAL_FILE_ADDED] = g_signal_new("file-added", G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
    /* A file appeared, changed or went after loading (thumbnails are due,
     * counts change). */
    signals[SIGNAL_FILES_CHANGED] = g_signal_new("files-changed", G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    /* A thumbnail arrived: redraw (rows are not touched; the views draw
     * icons from sfe_dir_model_icon()). */
    signals[SIGNAL_THUMBNAILS_CHANGED] = g_signal_new("thumbnails-changed", G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void sfe_dir_model_init(SfeDirModel* self)
{
    self->scores = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    self->sort_order = GTK_SORT_ASCENDING;
    self->dirs_first = true;
    self->grid_size = 64;
    self->list_size = 16;
    self->scale = 1;
}

SfeDirModel* sfe_dir_model_new(void)
{
    return g_object_new(SFE_TYPE_DIR_MODEL, NULL);
}
