#include "dirtree.h"

#include <glib/gstdio.h>
#include <string.h>

enum {
    COL_NAME,
    COL_ICON, /* icon name */
    COL_FILE, /* GFile*, NULL for placeholders */
    COL_STATE, /* NodeState */
    COL_COLLATE, /* sort key */
    N_COLS,
};

/* Unloaded directories get one placeholder child so they show an expander. */
typedef enum {
    NODE_PLACEHOLDER,
    NODE_UNLOADED,
    NODE_LOADING,
    NODE_LOADED,
} NodeState;

#define LOAD_ATTRIBUTES "standard::name,standard::display-name,standard::type,standard::is-hidden"

struct _SfeDirTree {
    GtkTreeView parent;

    GtkTreeStore* store;
    GFile* root;
    GFile* location; /* what the tree should highlight */
    GCancellable* cancel; /* cancelled when the tree is rebuilt */
    GHashTable* monitors; /* path of a loaded directory -> GFileMonitor* */
    bool show_hidden;
    bool selecting; /* selection changes made by us, not the user */
};

G_DEFINE_TYPE(SfeDirTree, sfe_dir_tree, GTK_TYPE_TREE_VIEW)

enum {
    SIGNAL_OPEN_LOCATION,
    N_SIGNALS,
};
static guint signals[N_SIGNALS];

static void load_node(SfeDirTree* self, GtkTreeIter* iter, bool expand);
static void reveal(SfeDirTree* self);
static void probe_for_subdirs(SfeDirTree* self, GFile* dir);

/* ------------------------------------------------------------------------ */
/* Rows                                                                      */
/* ------------------------------------------------------------------------ */

static GtkTreeModel* model(SfeDirTree* self)
{
    return GTK_TREE_MODEL(self->store);
}

static int node_state(SfeDirTree* self, GtkTreeIter* iter)
{
    int state = NODE_PLACEHOLDER;
    gtk_tree_model_get(model(self), iter, COL_STATE, &state, -1);
    return state;
}

static GFile* node_file(SfeDirTree* self, GtkTreeIter* iter)
{
    GFile* file = NULL;
    gtk_tree_model_get(model(self), iter, COL_FILE, &file, -1);
    return file; /* new reference */
}

/* Adds an unloaded directory row (with its placeholder child). */
static void add_dir_row(SfeDirTree* self, GtkTreeIter* parent, GFile* file,
    const char* name, const char* icon)
{
    GtkTreeIter iter, placeholder;
    g_autofree char* collate = g_utf8_collate_key_for_filename(name, -1);

    gtk_tree_store_insert_with_values(self->store, &iter, parent, -1,
        COL_NAME, name, COL_ICON, icon, COL_FILE, file,
        COL_STATE, NODE_UNLOADED, COL_COLLATE, collate, -1);
    gtk_tree_store_insert_with_values(self->store, &placeholder, &iter, -1,
        COL_STATE, NODE_PLACEHOLDER, COL_COLLATE, "", -1);
}

/* Finds the child of `parent` named `name`. */
static bool find_child(SfeDirTree* self, GtkTreeIter* parent, const char* name, GtkTreeIter* out)
{
    if (!gtk_tree_model_iter_children(model(self), out, parent))
        return false;
    do {
        g_autoptr(GFile) file = node_file(self, out);
        g_autofree char* base = file ? g_file_get_basename(file) : NULL;
        if (g_strcmp0(base, name) == 0)
            return true;
    } while (gtk_tree_model_iter_next(model(self), out));
    return false;
}

static void remove_placeholders(SfeDirTree* self, GtkTreeIter* parent)
{
    GtkTreeIter child;
    bool valid = gtk_tree_model_iter_children(model(self), &child, parent);

    while (valid) {
        if (node_state(self, &child) == NODE_PLACEHOLDER)
            valid = gtk_tree_store_remove(self->store, &child);
        else
            valid = gtk_tree_model_iter_next(model(self), &child);
    }
}

static int compare_rows(GtkTreeModel* m, GtkTreeIter* a, GtkTreeIter* b, gpointer data)
{
    (void)data;
    g_autofree char* ka = NULL;
    g_autofree char* kb = NULL;
    gtk_tree_model_get(m, a, COL_COLLATE, &ka, -1);
    gtk_tree_model_get(m, b, COL_COLLATE, &kb, -1);
    return g_strcmp0(ka, kb);
}

/* ------------------------------------------------------------------------ */
/* Live updates of loaded directories                                        */
/* ------------------------------------------------------------------------ */

/* Finds the row of directory `dir` by walking down from the root. Rows are
 * found by path instead of GtkTreeRowReference: a reference freed while the
 * store is being cleared crashes GTK. */
static bool iter_for(SfeDirTree* self, GFile* dir, GtkTreeIter* out)
{
    GtkTreeIter iter, child;
    if (!gtk_tree_model_get_iter_first(model(self), &iter))
        return false;

    g_autoptr(GFile) root = node_file(self, &iter);
    if (root == NULL)
        return false;
    if (!g_file_equal(root, dir)) {
        g_autofree char* rel = g_file_get_relative_path(root, dir);
        if (rel == NULL)
            return false;
        g_auto(GStrv) parts = g_strsplit(rel, "/", -1);
        for (char** part = parts; *part != NULL; part++) {
            if (!find_child(self, &iter, *part, &child))
                return false;
            iter = child;
        }
    }
    *out = iter;
    return true;
}

static void on_node_changed(GFileMonitor* monitor, GFile* file, GFile* other,
    GFileMonitorEvent event, gpointer data)
{
    SfeDirTree* self = data;
    GFile* dir = g_object_get_data(G_OBJECT(monitor), "sfe-dir");
    GtkTreeIter parent, child;
    GFile* added = NULL;
    GFile* removed = NULL;

    if (!iter_for(self, dir, &parent))
        return;

    switch (event) {
    case G_FILE_MONITOR_EVENT_CREATED:
    case G_FILE_MONITOR_EVENT_MOVED_IN:
        added = file;
        break;
    case G_FILE_MONITOR_EVENT_DELETED:
    case G_FILE_MONITOR_EVENT_MOVED_OUT:
        removed = file;
        break;
    case G_FILE_MONITOR_EVENT_RENAMED:
        removed = file;
        added = other;
        break;
    default:
        return;
    }

    if (removed != NULL) {
        g_autofree char* name = g_file_get_basename(removed);
        g_autofree char* path = g_file_get_path(removed);
        if (find_child(self, &parent, name, &child)) {
            self->selecting = true; /* removing the selected row is not a click */
            gtk_tree_store_remove(self->store, &child);
            self->selecting = false;
        }
        if (path != NULL)
            g_hash_table_remove(self->monitors, path);
    }
    if (added != NULL && g_file_query_file_type(added, G_FILE_QUERY_INFO_NONE, NULL) == G_FILE_TYPE_DIRECTORY) {
        g_autofree char* name = g_file_get_basename(added);
        if ((self->show_hidden || name[0] != '.') && !find_child(self, &parent, name, &child)) {
            add_dir_row(self, &parent, added, name, "folder-symbolic");
            probe_for_subdirs(self, added);
        }
    }
}

static void watch_dir(SfeDirTree* self, GFile* dir)
{
    g_autofree char* path = g_file_get_path(dir);
    if (path == NULL || g_hash_table_contains(self->monitors, path))
        return;
    GFileMonitor* monitor = g_file_monitor_directory(dir, G_FILE_MONITOR_WATCH_MOVES, self->cancel, NULL);
    if (monitor == NULL)
        return;

    g_object_set_data_full(G_OBJECT(monitor), "sfe-dir", g_object_ref(dir), g_object_unref);
    g_signal_connect(monitor, "changed", G_CALLBACK(on_node_changed), self);
    g_hash_table_insert(self->monitors, g_steal_pointer(&path), monitor);
}

/* Value destructor of the monitors table. */
static void drop_monitor(gpointer p)
{
    GFileMonitor* monitor = p;
    g_signal_handlers_disconnect_matched(monitor, G_SIGNAL_MATCH_ID, g_signal_lookup("changed", G_TYPE_FILE_MONITOR), 0, NULL, NULL, NULL);
    g_file_monitor_cancel(monitor);
    g_object_unref(monitor);
}

/* ------------------------------------------------------------------------ */
/* Lazy loading                                                              */
/* ------------------------------------------------------------------------ */

typedef struct {
    SfeDirTree* self;
    GFile* dir;
    GCancellable* cancel;
    GPtrArray* infos;
    bool expand;
} LoadJob;

static void load_job_free(LoadJob* job)
{
    g_object_unref(job->self);
    g_object_unref(job->dir);
    g_object_unref(job->cancel);
    g_ptr_array_unref(job->infos);
    g_free(job);
}

/* ------------------------------------------------------------------------ */
/* Expanders only where there is something to expand                         */
/* ------------------------------------------------------------------------ */

/* Checks in the background whether `dir` has a (shown) subfolder; if not,
 * its placeholder goes and the expander disappears. */
typedef struct {
    SfeDirTree* self;
    GFile* dir;
    GCancellable* cancel;
} Probe;

static void probe_free(Probe* probe)
{
    g_object_unref(probe->self);
    g_object_unref(probe->dir);
    g_object_unref(probe->cancel);
    g_free(probe);
}

/* No subfolders: drop the expander, but keep watching for new ones. */
static void mark_childless(Probe* probe)
{
    SfeDirTree* self = probe->self;
    GtkTreeIter iter;

    if (g_cancellable_is_cancelled(probe->cancel) || !iter_for(self, probe->dir, &iter)
        || node_state(self, &iter) != NODE_UNLOADED)
        return;
    remove_placeholders(self, &iter);
    gtk_tree_store_set(self->store, &iter, COL_STATE, NODE_LOADED, -1);
    watch_dir(self, probe->dir);
}

static void on_probe_files(GObject* source, GAsyncResult* res, gpointer data)
{
    Probe* probe = data;
    GFileEnumerator* en = G_FILE_ENUMERATOR(source);
    GList* files = g_file_enumerator_next_files_finish(en, res, NULL);
    bool found = false;

    for (GList* l = files; l != NULL; l = l->next) {
        GFileInfo* info = l->data;
        found |= g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY
            && (probe->self->show_hidden || !g_file_info_get_is_hidden(info));
    }
    g_list_free_full(files, g_object_unref);

    if (found || files == NULL) {
        if (!found)
            mark_childless(probe);
        g_object_unref(en);
        probe_free(probe);
        return;
    }
    g_file_enumerator_next_files_async(en, 64, G_PRIORITY_LOW, probe->cancel, on_probe_files, probe);
}

static void on_probe_enumerate(GObject* source, GAsyncResult* res, gpointer data)
{
    Probe* probe = data;
    GFileEnumerator* en = g_file_enumerate_children_finish(G_FILE(source), res, NULL);

    if (en == NULL) { /* unreadable: nothing to expand either */
        mark_childless(probe);
        probe_free(probe);
        return;
    }
    g_file_enumerator_next_files_async(en, 64, G_PRIORITY_LOW, probe->cancel, on_probe_files, probe);
}

static void probe_for_subdirs(SfeDirTree* self, GFile* dir)
{
    Probe* probe = g_new0(Probe, 1);
    probe->self = g_object_ref(self);
    probe->dir = g_object_ref(dir);
    probe->cancel = g_object_ref(self->cancel);
    g_file_enumerate_children_async(dir, LOAD_ATTRIBUTES, G_FILE_QUERY_INFO_NONE, G_PRIORITY_LOW,
        probe->cancel, on_probe_enumerate, probe);
}

static void finish_load(LoadJob* job)
{
    SfeDirTree* self = job->self;
    GtkTreeIter iter;

    if (g_cancellable_is_cancelled(job->cancel) || !iter_for(self, job->dir, &iter))
        return;

    remove_placeholders(self, &iter);
    for (guint i = 0; i < job->infos->len; i++) {
        GFileInfo* info = g_ptr_array_index(job->infos, i);
        g_autoptr(GFile) child = g_file_get_child(job->dir, g_file_info_get_name(info));
        add_dir_row(self, &iter, child, g_file_info_get_display_name(info), "folder-symbolic");
        probe_for_subdirs(self, child);
    }
    gtk_tree_store_set(self->store, &iter, COL_STATE, NODE_LOADED, -1);
    watch_dir(self, job->dir);

    if (job->expand) {
        g_autoptr(GtkTreePath) path = gtk_tree_model_get_path(model(self), &iter);
        gtk_tree_view_expand_row(GTK_TREE_VIEW(self), path, FALSE);
    }
    reveal(self);
}

static void on_next_files(GObject* source, GAsyncResult* res, gpointer data)
{
    LoadJob* job = data;
    GFileEnumerator* en = G_FILE_ENUMERATOR(source);
    GList* files = g_file_enumerator_next_files_finish(en, res, NULL);

    if (files == NULL) { /* done, or failed: show what we have */
        g_object_unref(en);
        finish_load(job);
        load_job_free(job);
        return;
    }
    for (GList* l = files; l != NULL; l = l->next) {
        GFileInfo* info = l->data;
        bool keep = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY
            && (job->self->show_hidden || !g_file_info_get_is_hidden(info));
        if (keep)
            g_ptr_array_add(job->infos, info);
        else
            g_object_unref(info);
    }
    g_list_free(files);
    g_file_enumerator_next_files_async(en, 256, G_PRIORITY_DEFAULT, job->cancel, on_next_files, job);
}

static void on_enumerate(GObject* source, GAsyncResult* res, gpointer data)
{
    LoadJob* job = data;
    GFileEnumerator* en = g_file_enumerate_children_finish(G_FILE(source), res, NULL);

    if (en == NULL) { /* unreadable: mark loaded with no children */
        finish_load(job);
        load_job_free(job);
        return;
    }
    g_file_enumerator_next_files_async(en, 256, G_PRIORITY_DEFAULT, job->cancel, on_next_files, job);
}

static void load_node(SfeDirTree* self, GtkTreeIter* iter, bool expand)
{
    if (node_state(self, iter) != NODE_UNLOADED)
        return;

    LoadJob* job = g_new0(LoadJob, 1);
    job->self = g_object_ref(self);
    job->dir = node_file(self, iter);
    job->cancel = g_object_ref(self->cancel);
    job->infos = g_ptr_array_new_with_free_func(g_object_unref);
    job->expand = expand;

    gtk_tree_store_set(self->store, iter, COL_STATE, NODE_LOADING, -1);
    g_file_enumerate_children_async(job->dir, LOAD_ATTRIBUTES, G_FILE_QUERY_INFO_NONE,
        G_PRIORITY_DEFAULT, job->cancel, on_enumerate, job);
}

/* Expanding an unloaded row loads it first and expands once loaded. */
static gboolean on_test_expand_row(GtkTreeView* view, GtkTreeIter* iter, GtkTreePath* path, gpointer data)
{
    (void)path, (void)data;
    SfeDirTree* self = SFE_DIR_TREE(view);

    if (node_state(self, iter) == NODE_LOADED)
        return FALSE;
    load_node(self, iter, true);
    return TRUE; /* not yet */
}

/* ------------------------------------------------------------------------ */
/* Root and reveal                                                           */
/* ------------------------------------------------------------------------ */

/* Home for anything inside it, otherwise the root of the filesystem `dir`
 * lives on (found by walking up while the device number stays the same). */
static GFile* root_for(GFile* dir)
{
    g_autofree char* path = g_file_get_path(dir);
    const char* home = g_get_home_dir();
    GStatBuf st;

    if (path == NULL)
        return g_object_ref(dir);
    if (g_str_has_prefix(path, home) && (path[strlen(home)] == '\0' || path[strlen(home)] == '/'))
        return g_file_new_for_path(home);
    if (g_stat(path, &st) != 0)
        return g_file_new_for_path("/");

    GFile* root = g_object_ref(dir);
    for (;;) {
        g_autoptr(GFile) parent = g_file_get_parent(root);
        g_autofree char* parent_path = parent ? g_file_get_path(parent) : NULL;
        GStatBuf pst;
        if (parent_path == NULL || g_stat(parent_path, &pst) != 0 || pst.st_dev != st.st_dev)
            return root;
        g_object_unref(root);
        root = g_steal_pointer(&parent);
    }
}

static void rebuild(SfeDirTree* self, GFile* root)
{
    GtkTreeIter iter;
    g_autofree char* path = g_file_get_path(root);
    bool is_home = g_strcmp0(path, g_get_home_dir()) == 0;
    bool is_fs_root = g_strcmp0(path, "/") == 0;
    g_autofree char* base = g_file_get_basename(root);

    g_cancellable_cancel(self->cancel);
    g_object_unref(self->cancel);
    self->cancel = g_cancellable_new();
    g_set_object(&self->root, root);

    g_hash_table_remove_all(self->monitors); /* before the rows they watch go */
    /* Clearing deselects the current row; that is not the user choosing a
     * folder. Reacting to it re-entered rebuild() inside gtk_tree_store_clear
     * and corrupted the store (crash when entering /sys/fs/bpf from /sys). */
    self->selecting = true;
    gtk_tree_store_clear(self->store);
    add_dir_row(self, NULL, root,
        is_home ? "Home" : is_fs_root ? "File System" : base,
        is_home ? "user-home-symbolic" : "drive-harddisk-symbolic");
    self->selecting = false;
    gtk_tree_model_get_iter_first(model(self), &iter);
    load_node(self, &iter, true);
}

static void select_iter(SfeDirTree* self, GtkTreeIter* iter)
{
    g_autoptr(GtkTreePath) path = gtk_tree_model_get_path(model(self), iter);
    GtkTreeSelection* selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(self));

    self->selecting = true;
    gtk_tree_selection_select_iter(selection, iter);
    gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(self), path, NULL, FALSE, 0, 0);
    self->selecting = false;
}

/* Walks from the root towards `location`, loading and expanding on the way.
 * Called again whenever a load finishes, until the target is reached. */
static void reveal(SfeDirTree* self)
{
    GtkTreeIter iter, child;

    if (self->location == NULL || !gtk_tree_model_get_iter_first(model(self), &iter))
        return;

    for (;;) {
        g_autoptr(GFile) file = node_file(self, &iter);

        if (g_file_equal(file, self->location)) {
            select_iter(self, &iter);
            return;
        }
        if (node_state(self, &iter) != NODE_LOADED) {
            load_node(self, &iter, true); /* reveal() runs again when done */
            return;
        }

        g_autoptr(GtkTreePath) path = gtk_tree_model_get_path(model(self), &iter);
        gtk_tree_view_expand_row(GTK_TREE_VIEW(self), path, FALSE);

        g_autofree char* rel = g_file_get_relative_path(file, self->location);
        g_auto(GStrv) parts = rel ? g_strsplit(rel, "/", 2) : NULL;
        if (parts == NULL || !find_child(self, &iter, parts[0], &child)) {
            /* e.g. a hidden directory: highlight the closest ancestor */
            gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(GTK_TREE_VIEW(self)));
            return;
        }
        iter = child;
    }
}

void sfe_dir_tree_set_location(SfeDirTree* self, GFile* dir)
{
    g_autoptr(GFile) root = root_for(dir);
    g_debug("tree: location %s (root %s)", g_file_peek_path(dir), g_file_peek_path(root));

    g_set_object(&self->location, dir);
    if (self->root == NULL || !g_file_equal(root, self->root))
        rebuild(self, root);
    else
        reveal(self);
}

void sfe_dir_tree_set_show_hidden(SfeDirTree* self, bool show)
{
    if (self->show_hidden == show)
        return;
    self->show_hidden = show;
    if (self->root != NULL) {
        g_autoptr(GFile) root = g_object_ref(self->root);
        rebuild(self, root);
    }
}

/* ------------------------------------------------------------------------ */
/* GObject                                                                   */
/* ------------------------------------------------------------------------ */

static void on_selection_changed(GtkTreeSelection* selection, gpointer data)
{
    SfeDirTree* self = data;
    GtkTreeIter iter;

    if (self->selecting || !gtk_tree_selection_get_selected(selection, NULL, &iter))
        return;
    g_autoptr(GFile) file = node_file(self, &iter);
    if (file != NULL && (self->location == NULL || !g_file_equal(file, self->location)))
        g_signal_emit(self, signals[SIGNAL_OPEN_LOCATION], 0, file);
}

static void sfe_dir_tree_dispose(GObject* object)
{
    SfeDirTree* self = SFE_DIR_TREE(object);

    if (self->cancel != NULL)
        g_cancellable_cancel(self->cancel);
    g_clear_object(&self->cancel);
    g_clear_pointer(&self->monitors, g_hash_table_unref);
    g_clear_object(&self->store);
    g_clear_object(&self->root);
    g_clear_object(&self->location);
    G_OBJECT_CLASS(sfe_dir_tree_parent_class)->dispose(object);
}

static void sfe_dir_tree_class_init(SfeDirTreeClass* klass)
{
    G_OBJECT_CLASS(klass)->dispose = sfe_dir_tree_dispose;
    signals[SIGNAL_OPEN_LOCATION] = g_signal_new("open-location", G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_FILE);
}

static void sfe_dir_tree_init(SfeDirTree* self)
{
    GtkTreeView* view = GTK_TREE_VIEW(self);
    GtkTreeViewColumn* column = gtk_tree_view_column_new();
    GtkCellRenderer* icon = gtk_cell_renderer_pixbuf_new();
    GtkCellRenderer* text = gtk_cell_renderer_text_new();

    self->cancel = g_cancellable_new();
    self->monitors = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, drop_monitor);
    self->store = gtk_tree_store_new(N_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_FILE,
        G_TYPE_INT, G_TYPE_STRING);
    gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(self->store), COL_COLLATE, compare_rows, NULL, NULL);
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(self->store), COL_COLLATE, GTK_SORT_ASCENDING);
    gtk_tree_view_set_model(view, GTK_TREE_MODEL(self->store));

    g_object_set(icon, "xpad", 2, "ypad", 5, NULL); /* rows as tall as the list's */
    g_object_set(text, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    gtk_tree_view_column_pack_start(column, icon, FALSE);
    gtk_tree_view_column_add_attribute(column, icon, "icon-name", COL_ICON);
    gtk_tree_view_column_pack_start(column, text, TRUE);
    gtk_tree_view_column_add_attribute(column, text, "text", COL_NAME);
    gtk_tree_view_append_column(view, column);

    gtk_tree_view_set_headers_visible(view, FALSE);
    gtk_tree_view_set_enable_tree_lines(view, TRUE);
    gtk_tree_view_set_enable_search(view, FALSE);
    gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(self)), "sfe-dir-tree");

    g_signal_connect(view, "test-expand-row", G_CALLBACK(on_test_expand_row), NULL);
    g_signal_connect(gtk_tree_view_get_selection(view), "changed", G_CALLBACK(on_selection_changed), self);
}

GtkWidget* sfe_dir_tree_new(void)
{
    return g_object_new(SFE_TYPE_DIR_TREE, NULL);
}
