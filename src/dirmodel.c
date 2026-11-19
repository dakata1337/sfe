#include "dirmodel.h"

#include "fuzzy.h"
#include "filefilter.h"
#include "icons.h"
#include <cairo-gobject.h>
#include <string.h>

#define BATCH_SIZE 256
#define COLLATE_KEY_ATTR "sfe::collate-key" /* in-memory only, never written */
#define SCORE_ATTR "sfe::search-score" /* in-memory only: match score of the query */

struct _SfeDirModel {
    GObject parent;

    GFile* dir;
    GtkListStore* store;
    GHashTable* rows; /* file name -> GtkTreeIter* into store (shown files) */
    GHashTable* infos; /* file name -> GFileInfo*, every file incl. hidden */
    GHashTable* thumbs; /* file name -> GdkPixbuf*, or NO_THUMB (none to be had) */
    bool building; /* filling a fresh store, not reacting to changes */
    char* query; /* search filter, NULL when not searching */
    const SfeFileFilter* filter; /* a file dialog's type filter (borrowed), or NULL */
    GFileMonitor* monitor;
    GCancellable* cancel; /* cancels the load and monitor queries of `dir` */

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

/* The row's picture: its thumbnail if it has one, else the theme icon. */
static cairo_surface_t* icon_for(SfeDirModel* self, GFileInfo* info, int size)
{
    gpointer thumb = self->thumbs ? g_hash_table_lookup(self->thumbs, g_file_info_get_name(info)) : NULL;
    if (thumb != NULL && thumb != NO_THUMB) {
        cairo_surface_t* surface = thumb_surface(thumb, size, self->scale);
        return info_is_dir(info) ? with_folder_badge(surface, info, self->dir, size, self->scale) : surface;
    }
    return sfe_icon_for_info(info, self->dir, size, self->scale);
}

static void set_icons(SfeDirModel* self, GtkListStore* store, GtkTreeIter* iter, GFileInfo* info)
{
    cairo_surface_t* big = icon_for(self, info, self->grid_size);
    cairo_surface_t* small = icon_for(self, info, self->list_size);

    gtk_list_store_set(store, iter, SFE_COL_ICON, big, SFE_COL_ICON_SMALL, small, -1);
    cairo_surface_destroy(big);
    cairo_surface_destroy(small);
}

/* Writes a whole row in one call, so a new row is inserted complete: an empty
 * row that is filled in later confuses GtkTreeModelFilter. */
static void store_row(SfeDirModel* self, GtkListStore* store, GtkTreeIter* iter, bool insert,
    GFile* file, GFileInfo* info)
{
    bool is_dir = info_is_dir(info);
    g_autofree char* size = is_dir ? g_strdup("") : g_format_size(g_file_info_get_size(info));
    g_autofree char* mtime = format_mtime(info);
    g_autofree char* collate = g_utf8_collate_key_for_filename(g_file_info_get_display_name(info), -1);
    cairo_surface_t* big = icon_for(self, info, self->grid_size);
    cairo_surface_t* small = icon_for(self, info, self->list_size);

    g_file_info_set_attribute_string(info, COLLATE_KEY_ATTR, collate);

    int columns[] = {
        SFE_COL_FILE, SFE_COL_INFO, SFE_COL_NAME, SFE_COL_ICON, SFE_COL_ICON_SMALL,
        SFE_COL_SIZE_TEXT, SFE_COL_MTIME_TEXT, SFE_COL_TYPE_TEXT, SFE_COL_IS_DIR,
    };
    GValue values[G_N_ELEMENTS(columns)] = { G_VALUE_INIT };
    g_value_set_object(g_value_init(&values[0], G_TYPE_FILE), file);
    g_value_set_object(g_value_init(&values[1], G_TYPE_FILE_INFO), info);
    g_value_set_string(g_value_init(&values[2], G_TYPE_STRING), g_file_info_get_display_name(info));
    g_value_take_boxed(g_value_init(&values[3], CAIRO_GOBJECT_TYPE_SURFACE), big);
    g_value_take_boxed(g_value_init(&values[4], CAIRO_GOBJECT_TYPE_SURFACE), small);
    g_value_set_string(g_value_init(&values[5], G_TYPE_STRING), size);
    g_value_set_string(g_value_init(&values[6], G_TYPE_STRING), mtime);
    g_value_set_string(g_value_init(&values[7], G_TYPE_STRING), is_dir ? "Folder" : type_description(info));
    g_value_set_boolean(g_value_init(&values[8], G_TYPE_BOOLEAN), is_dir);

    if (insert)
        gtk_list_store_insert_with_valuesv(store, iter, -1, columns, values, G_N_ELEMENTS(columns));
    else
        gtk_list_store_set_valuesv(store, iter, columns, values, G_N_ELEMENTS(columns));

    for (size_t i = 0; i < G_N_ELEMENTS(values); i++)
        g_value_unset(&values[i]);
}

static bool passes_hidden(SfeDirModel* self, GFileInfo* info)
{
    return self->show_hidden || !g_file_info_get_is_hidden(info);
}

/* While searching, a file is shown only if it matches; its score is kept on
 * the info for sorting. */
static bool is_shown(SfeDirModel* self, GFileInfo* info)
{
    if (!passes_hidden(self, info))
        return false;
    if (!sfe_file_filter_shows(self->filter, g_file_info_get_display_name(info),
            g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE), info_is_dir(info)))
        return false;
    if (self->query == NULL)
        return true;
    int score = sfe_fuzzy_score(self->query, g_file_info_get_display_name(info));
    g_file_info_set_attribute_int32(info, SCORE_ATTR, score);
    return score != SFE_FUZZY_NO_MATCH;
}

static void remove_row(SfeDirModel* self, const char* name)
{
    GtkTreeIter* iter = g_hash_table_lookup(self->rows, name);
    if (iter != NULL) {
        gtk_list_store_remove(self->store, iter);
        g_hash_table_remove(self->rows, name);
    }
}

/* Remembers `info` and shows, updates or hides its row. The store only
 * holds the rows that are shown: no GtkTreeModelFilter, whose reference
 * counting breaks when shown rows are removed. */
static void add_or_update(SfeDirModel* self, GFileInfo* info)
{
    const char* name = g_file_info_get_name(info);
    GtkTreeIter* iter = g_hash_table_lookup(self->rows, name);

    g_hash_table_replace(self->infos, g_strdup(name), g_object_ref(info));
    if (!self->building) {
        g_hash_table_remove(self->thumbs, name); /* changed on disk: a new thumbnail */
        g_signal_emit(self, signals[SIGNAL_FILES_CHANGED], 0);
    }
    if (!is_shown(self, info)) {
        remove_row(self, name);
        return;
    }

    g_autoptr(GFile) file = g_file_get_child(self->dir, name);
    bool insert = iter == NULL;
    if (insert) {
        iter = g_new(GtkTreeIter, 1);
        g_hash_table_insert(self->rows, g_strdup(name), iter);
    }
    store_row(self, self->store, iter, insert, file, info);
    if (insert && !self->building)
        g_signal_emit(self, signals[SIGNAL_FILE_ADDED], 0, name);
}

static void forget_file(SfeDirModel* self, const char* name)
{
    remove_row(self, name);
    g_hash_table_remove(self->infos, name);
    g_hash_table_remove(self->thumbs, name);
}

static void refresh_icons(SfeDirModel* self)
{
    GtkTreeIter iter;
    if (self->store == NULL || !gtk_tree_model_get_iter_first(GTK_TREE_MODEL(self->store), &iter))
        return;
    do {
        g_autoptr(GFileInfo) info = NULL;
        gtk_tree_model_get(GTK_TREE_MODEL(self->store), &iter, SFE_COL_INFO, &info, -1);
        set_icons(self, self->store, &iter, info);
    } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(self->store), &iter));
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

static int compare_rows(GtkTreeModel* model, GtkTreeIter* ia, GtkTreeIter* ib, gpointer data)
{
    SfeDirModel* self = data;
    GFileInfo* a = row_info(model, ia);
    GFileInfo* b = row_info(model, ib);
    int result = 0;

    /* While searching, the best match comes first, whatever the sort. */
    if (self->query != NULL) {
        result = compare_numbers(g_file_info_get_attribute_int32(b, SCORE_ATTR),
            g_file_info_get_attribute_int32(a, SCORE_ATTR));
        if (result != 0)
            return self->sort_order == GTK_SORT_DESCENDING ? -result : result;
    }

    if (self->dirs_first && info_is_dir(a) != info_is_dir(b)) {
        /* Directories stay on top in both sort orders. The store inverts
         * our result for descending order, so pre-invert it here. */
        result = info_is_dir(a) ? -1 : 1;
        return self->sort_order == GTK_SORT_DESCENDING ? -result : result;
    }

    switch (self->sort_key) {
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

static void apply_sort(SfeDirModel* self)
{
    GtkTreeSortable* sortable = GTK_TREE_SORTABLE(self->store);
    if (sortable == NULL)
        return;
    /* Going through "unsorted" forces a resort even when the column and the
     * order did not change (e.g. only dirs_first did). */
    gtk_tree_sortable_set_sort_column_id(sortable, GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID, self->sort_order);
    gtk_tree_sortable_set_sort_column_id(sortable, 0, self->sort_order);
}

/* ------------------------------------------------------------------------ */
/* Directory monitor                                                         */
/* ------------------------------------------------------------------------ */

static void on_query_done(GObject* source, GAsyncResult* res, gpointer data)
{
    g_autoptr(SfeDirModel) self = data;
    g_autoptr(GFileInfo) info = g_file_query_info_finish(G_FILE(source), res, NULL);
    g_autoptr(GFile) parent = g_file_get_parent(G_FILE(source));

    /* Ignore results that arrive after the pane moved to another directory,
     * or for a file renamed or deleted meanwhile (that event was already
     * handled: adding it now would leave a ghost, e.g. a download's .part). */
    if (info != NULL && self->store != NULL && parent != NULL && g_file_equal(parent, self->dir)
        && g_file_query_exists(G_FILE(source), NULL))
        add_or_update(self, info);
}

static void query_and_update(SfeDirModel* self, GFile* file)
{
    g_file_query_info_async(file, SFE_FILE_ATTRIBUTES, G_FILE_QUERY_INFO_NONE,
        G_PRIORITY_DEFAULT, self->cancel, on_query_done, g_object_ref(self));
}

static bool is_child(SfeDirModel* self, GFile* file)
{
    g_autoptr(GFile) parent = file ? g_file_get_parent(file) : NULL;
    return parent != NULL && g_file_equal(parent, self->dir);
}

static void on_dir_changed(GFileMonitor* monitor, GFile* file, GFile* other,
    GFileMonitorEvent event, gpointer data)
{
    (void)monitor;
    SfeDirModel* self = data;
    g_autofree char* name = g_file_get_basename(file);

    if (g_file_equal(file, self->dir)) {
        if (event == G_FILE_MONITOR_EVENT_DELETED || event == G_FILE_MONITOR_EVENT_MOVED_OUT
            || event == G_FILE_MONITOR_EVENT_UNMOUNTED)
            g_signal_emit(self, signals[SIGNAL_GONE], 0);
        return;
    }

    switch (event) {
    case G_FILE_MONITOR_EVENT_CREATED:
    case G_FILE_MONITOR_EVENT_MOVED_IN:
    case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT:
    case G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED:
        query_and_update(self, file);
        break;
    case G_FILE_MONITOR_EVENT_DELETED:
    case G_FILE_MONITOR_EVENT_MOVED_OUT:
        forget_file(self, name);
        break;
    case G_FILE_MONITOR_EVENT_RENAMED:
        forget_file(self, name);
        if (is_child(self, other))
            query_and_update(self, other);
        break;
    default:
        break;
    }
}

static void start_monitor(SfeDirModel* self)
{
    g_autoptr(GError) error = NULL;

    self->monitor = g_file_monitor_directory(self->dir, G_FILE_MONITOR_WATCH_MOVES, self->cancel, &error);
    if (self->monitor == NULL) {
        g_debug("not watching %s: %s", g_file_peek_path(self->dir), error->message);
        return;
    }
    g_signal_connect(self->monitor, "changed", G_CALLBACK(on_dir_changed), self);
}

/* ------------------------------------------------------------------------ */
/* Loading                                                                   */
/* ------------------------------------------------------------------------ */

typedef struct {
    SfeDirModel* self;
    GFile* dir;
    GCancellable* cancel;
    GPtrArray* infos; /* GFileInfo* */
} LoadJob;

static void load_job_free(LoadJob* job)
{
    g_object_unref(job->self);
    g_object_unref(job->dir);
    g_object_unref(job->cancel);
    g_ptr_array_unref(job->infos);
    g_free(job);
}

static GtkListStore* new_store(void)
{
    return gtk_list_store_new(SFE_N_COLUMNS,
        G_TYPE_FILE, G_TYPE_FILE_INFO, G_TYPE_STRING,
        CAIRO_GOBJECT_TYPE_SURFACE, CAIRO_GOBJECT_TYPE_SURFACE,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN);
}

static void clear_contents(SfeDirModel* self)
{
    if (self->monitor != NULL) {
        g_signal_handlers_disconnect_by_data(self->monitor, self);
        g_clear_object(&self->monitor);
    }
    g_clear_object(&self->store);
    g_clear_pointer(&self->rows, g_hash_table_unref);
    g_clear_pointer(&self->infos, g_hash_table_unref);
    g_clear_pointer(&self->thumbs, g_hash_table_unref);
}

/* Builds the whole store at once, then sorts it once: much faster than
 * inserting rows one by one into a sorted, displayed model. */
static void finish_load(LoadJob* job)
{
    SfeDirModel* self = job->self;

    if (g_cancellable_is_cancelled(job->cancel)) /* superseded by a newer load */
        return;

    clear_contents(self);
    g_set_object(&self->dir, job->dir);
    g_set_object(&self->cancel, job->cancel);

    self->store = new_store();
    self->rows = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    self->infos = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
    self->thumbs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, thumb_unref);
    self->building = true;
    for (guint i = 0; i < job->infos->len; i++)
        add_or_update(self, g_ptr_array_index(job->infos, i));
    self->building = false;

    gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(self->store), 0, compare_rows, self, NULL);
    apply_sort(self);

    start_monitor(self);
    g_signal_emit(self, signals[SIGNAL_LOADED], 0);
}

static void fail_load(LoadJob* job, GError* error)
{
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_signal_emit(job->self, signals[SIGNAL_ERROR], 0, error);
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

void sfe_dir_model_load(SfeDirModel* self, GFile* dir)
{
    LoadJob* job = g_new0(LoadJob, 1);

    if (self->cancel != NULL)
        g_cancellable_cancel(self->cancel);

    job->self = g_object_ref(self);
    job->dir = g_object_ref(dir);
    job->cancel = g_cancellable_new();
    job->infos = g_ptr_array_new_with_free_func(g_object_unref);
    g_set_object(&self->cancel, job->cancel);

    g_file_enumerate_children_async(dir, SFE_FILE_ATTRIBUTES, G_FILE_QUERY_INFO_NONE,
        G_PRIORITY_DEFAULT, job->cancel, on_enumerate, job);
}

void sfe_dir_model_reload(SfeDirModel* self)
{
    if (self->dir != NULL)
        sfe_dir_model_load(self, self->dir);
}

/* ------------------------------------------------------------------------ */
/* Public accessors                                                          */
/* ------------------------------------------------------------------------ */

GtkTreeModel* sfe_dir_model_get_model(SfeDirModel* self)
{
    return GTK_TREE_MODEL(self->store);
}

/* Refills the store from the remembered infos (used when the search query
 * changes: one bulk build is much faster than thousands of row updates in a
 * sorted store). Emits "loaded" so the views attach the new store. */
static void rebuild_store(SfeDirModel* self)
{
    if (self->infos == NULL)
        return;

    GtkListStore* old_store = self->store;
    GHashTable* old_rows = self->rows;
    g_autoptr(GList) infos = g_hash_table_get_values(self->infos);

    self->store = new_store();
    self->rows = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    self->building = true;
    for (GList* l = infos; l != NULL; l = l->next) {
        g_autoptr(GFileInfo) info = g_object_ref(l->data);
        add_or_update(self, info);
    }
    self->building = false;
    gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(self->store), 0, compare_rows, self, NULL);
    apply_sort(self);

    g_signal_emit(self, signals[SIGNAL_LOADED], 0);
    g_object_unref(old_store);
    g_hash_table_unref(old_rows);
}

void sfe_dir_model_set_query(SfeDirModel* self, const char* query)
{
    const char* q = query && *query ? query : NULL;
    if (g_strcmp0(q, self->query) == 0)
        return;
    g_free(self->query);
    self->query = g_strdup(q);
    rebuild_store(self);
}

void sfe_dir_model_get_counts(SfeDirModel* self, int* shown, int* total)
{
    GHashTableIter it;
    gpointer info;

    *shown = self->rows ? (int)g_hash_table_size(self->rows) : 0;
    *total = 0;
    if (self->infos == NULL)
        return;
    g_hash_table_iter_init(&it, self->infos);
    while (g_hash_table_iter_next(&it, NULL, &info))
        *total += passes_hidden(self, info);
}

void sfe_dir_model_set_filter(SfeDirModel* self, const SfeFileFilter* filter)
{
    if (self->filter == filter)
        return;
    self->filter = filter;
    rebuild_store(self);
}

void sfe_dir_model_set_show_hidden(SfeDirModel* self, bool show)
{
    if (self->show_hidden == show)
        return;
    self->show_hidden = show;
    if (self->infos == NULL)
        return;
    /* Re-evaluate every file: hidden ones appear or disappear in place. */
    g_autoptr(GList) infos = g_hash_table_get_values(self->infos);
    for (GList* l = infos; l != NULL; l = l->next) {
        g_autoptr(GFileInfo) info = g_object_ref(l->data);
        add_or_update(self, info);
    }
}

void sfe_dir_model_set_sort(SfeDirModel* self, SfeSortKey key, GtkSortType order, bool dirs_first)
{
    if (self->sort_key == key && self->sort_order == order && self->dirs_first == dirs_first)
        return;
    self->sort_key = key;
    self->sort_order = order;
    self->dirs_first = dirs_first;
    apply_sort(self);
}

void sfe_dir_model_get_sort(SfeDirModel* self, SfeSortKey* key, GtkSortType* order)
{
    *key = self->sort_key;
    *order = self->sort_order;
}

void sfe_dir_model_set_icon_sizes(SfeDirModel* self, int grid, int list, int scale)
{
    if (self->grid_size == grid && self->list_size == list && self->scale == scale)
        return;
    self->grid_size = grid;
    self->list_size = list;
    self->scale = scale;
    refresh_icons(self);
}

bool sfe_dir_model_wants_thumbnail(SfeDirModel* self, const char* name)
{
    return self->thumbs != NULL && !g_hash_table_contains(self->thumbs, name);
}

void sfe_dir_model_set_thumbnail(SfeDirModel* self, GFile* file, GdkPixbuf* pixbuf)
{
    g_autoptr(GFile) parent = g_file_get_parent(file);
    if (self->thumbs == NULL || self->dir == NULL || parent == NULL || !g_file_equal(parent, self->dir))
        return; /* from a folder no longer shown */
    g_autofree char* name = g_file_get_basename(file);
    g_hash_table_replace(self->thumbs, g_strdup(name), pixbuf ? g_object_ref(pixbuf) : NO_THUMB);
    GtkTreeIter* iter = g_hash_table_lookup(self->rows, name);
    GFileInfo* info = g_hash_table_lookup(self->infos, name);
    if (iter != NULL && info != NULL && pixbuf != NULL)
        set_icons(self, self->store, iter, info);
}

void sfe_dir_model_clear_thumbnails(SfeDirModel* self)
{
    if (self->thumbs == NULL)
        return;
    g_hash_table_remove_all(self->thumbs);
    refresh_icons(self);
}

GtkTreePath* sfe_dir_model_find(SfeDirModel* self, const char* name)
{
    GtkTreeIter* iter = self->rows ? g_hash_table_lookup(self->rows, name) : NULL;
    return iter ? gtk_tree_model_get_path(GTK_TREE_MODEL(self->store), iter) : NULL;
}

GFile* sfe_dir_model_file_at(SfeDirModel* self, GtkTreePath* path)
{
    GtkTreeIter iter;
    GFile* file = NULL;

    if (self->store != NULL && gtk_tree_model_get_iter(GTK_TREE_MODEL(self->store), &iter, path))
        gtk_tree_model_get(GTK_TREE_MODEL(self->store), &iter, SFE_COL_FILE, &file, -1);
    return file;
}

GFileInfo* sfe_dir_model_info_at(SfeDirModel* self, GtkTreePath* path)
{
    GtkTreeIter iter;
    GFileInfo* info = NULL;

    if (self->store != NULL && gtk_tree_model_get_iter(GTK_TREE_MODEL(self->store), &iter, path))
        gtk_tree_model_get(GTK_TREE_MODEL(self->store), &iter, SFE_COL_INFO, &info, -1);
    return info;
}

/* ------------------------------------------------------------------------ */
/* GObject                                                                   */
/* ------------------------------------------------------------------------ */

static void on_icon_theme_changed(GtkIconTheme* theme, gpointer data)
{
    (void)theme;
    refresh_icons(data);
}

static void sfe_dir_model_dispose(GObject* object)
{
    SfeDirModel* self = SFE_DIR_MODEL(object);

    g_signal_handlers_disconnect_by_data(gtk_icon_theme_get_default(), self);
    if (self->cancel != NULL)
        g_cancellable_cancel(self->cancel);
    clear_contents(self);
    g_clear_object(&self->cancel);
    g_clear_object(&self->dir);
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
    /* A file appeared or changed after loading (thumbnails are due). */
    signals[SIGNAL_FILES_CHANGED] = g_signal_new("files-changed", G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void sfe_dir_model_init(SfeDirModel* self)
{
    self->sort_order = GTK_SORT_ASCENDING;
    self->dirs_first = true;
    self->grid_size = 64;
    self->list_size = 16;
    self->scale = 1;
    /* Connected "after" so the icon cache is already cleared. */
    g_signal_connect_after(gtk_icon_theme_get_default(), "changed", G_CALLBACK(on_icon_theme_changed), self);
}

SfeDirModel* sfe_dir_model_new(void)
{
    return g_object_new(SFE_TYPE_DIR_MODEL, NULL);
}
