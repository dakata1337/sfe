#include "actions.h"

#include "app.h"
#include "archive.h"
#include "bookmarks.h"
#include "clipboard.h"
#include "compressdlg.h"
#include "dialogs.h"
#include "fileops.h"
#include "opener.h"
#include "prefs.h"
#include "props.h"
#include "renamedlg.h"
#include "split.h"
#include "trash.h"
#include "undo.h"
#include <gio/gdesktopappinfo.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/* Targets and helpers                                                       */
/* ------------------------------------------------------------------------ */

SfeActionTarget sfe_action_target_for(SfeWindow* window)
{
    SfeActionTarget t = { .window = window, .pane = sfe_window_get_active_pane(window) };
    t.files = sfe_pane_get_selected_files(t.pane);
    t.dir = g_object_ref(sfe_pane_get_dir(t.pane));
    return t;
}

void sfe_action_target_clear(SfeActionTarget* t)
{
    g_list_free_full(t->files, g_object_unref);
    g_clear_object(&t->dir);
}

static GtkWidget* widget_of(const SfeActionTarget* t)
{
    return GTK_WIDGET(t->window);
}

static void run_op(const SfeActionTarget* t, SfeOpKind kind, GList* files, GFile* dest)
{
    g_autoptr(SfeFileOp) op = sfe_file_op_new(kind, files, dest);
    sfe_window_run_op(t->window, op);
}

static bool is_dir(GFile* file)
{
    return g_file_query_file_type(file, G_FILE_QUERY_INFO_NONE, NULL) == G_FILE_TYPE_DIRECTORY;
}

static void launch_app(GAppInfo* app, GList* files, GtkWidget* widget)
{
    g_autoptr(GdkAppLaunchContext) ctx = gdk_display_get_app_launch_context(gtk_widget_get_display(widget));
    g_autoptr(GError) error = NULL;

    if (!g_app_info_launch(app, files, G_APP_LAUNCH_CONTEXT(ctx), &error)) {
        g_autofree char* title = g_strdup_printf("Cannot start %s", g_app_info_get_name(app));
        sfe_show_error(widget, title, error->message);
    }
}

/* ------------------------------------------------------------------------ */
/* Menu visibility                                                           */
/* ------------------------------------------------------------------------ */

static bool has_files(const SfeActionTarget* t)
{
    return t->files != NULL;
}

static bool single_dir(const SfeActionTarget* t)
{
    return t->files != NULL && t->files->next == NULL && is_dir(t->files->data);
}

static bool can_paste(const SfeActionTarget* t)
{
    (void)t;
    return sfe_clipboard_has_files();
}

static bool can_paste_into(const SfeActionTarget* t)
{
    return single_dir(t) && sfe_clipboard_has_files();
}

/* Every selected file is an archive that an installed tool can extract. */
static bool can_extract(const SfeActionTarget* t)
{
    if (t->files == NULL)
        return false;
    for (GList* l = t->files; l != NULL; l = l->next) {
        g_autofree char* name = g_file_get_basename(l->data);
        if (is_dir(l->data) || !sfe_archive_can_extract(name))
            return false;
    }
    return true;
}

static bool can_bookmark_selection(const SfeActionTarget* t)
{
    return single_dir(t) && sfe_bookmarks_find(sfe_config(), t->files->data) < 0;
}

static bool can_unbookmark_selection(const SfeActionTarget* t)
{
    return single_dir(t) && sfe_bookmarks_find(sfe_config(), t->files->data) >= 0;
}

static bool can_bookmark_folder(const SfeActionTarget* t)
{
    return sfe_bookmarks_find(sfe_config(), t->dir) < 0;
}

static bool can_unbookmark_folder(const SfeActionTarget* t)
{
    return sfe_bookmarks_find(sfe_config(), t->dir) >= 0;
}

/* ------------------------------------------------------------------------ */
/* Navigation and layout                                                     */
/* ------------------------------------------------------------------------ */

static void run_back(const SfeActionTarget* t) { sfe_pane_go_back(t->pane); }
static void run_forward(const SfeActionTarget* t) { sfe_pane_go_forward(t->pane); }
static void run_up(const SfeActionTarget* t) { sfe_pane_go_up(t->pane); }
static void run_reload(const SfeActionTarget* t) { sfe_pane_reload(t->pane); }
static void run_location(const SfeActionTarget* t) { sfe_pane_focus_location(t->pane); }
static void run_view_grid(const SfeActionTarget* t) { sfe_pane_set_view(t->pane, SFE_VIEW_GRID); }
static void run_view_list(const SfeActionTarget* t) { sfe_pane_set_view(t->pane, SFE_VIEW_LIST); }

/* Hidden files are a global, persisted setting. */
static void run_toggle_hidden(const SfeActionTarget* t)
{
    (void)t;
    SfeConfig* cfg = sfe_application_dup_config(sfe_app());
    cfg->show_hidden = !cfg->show_hidden;
    sfe_application_set_config(sfe_app(), cfg);
}

static bool hidden_state(void)
{
    return sfe_config()->show_hidden;
}

static void toggle_bookmark(GFile* dir)
{
    SfeConfig* cfg = sfe_application_dup_config(sfe_app());
    int index = sfe_bookmarks_find(cfg, dir);

    if (index >= 0)
        sfe_bookmarks_remove(cfg, index);
    else
        sfe_bookmarks_add(cfg, dir);
    sfe_application_set_config(sfe_app(), cfg);
}

static void run_bookmark(const SfeActionTarget* t) { toggle_bookmark(t->dir); }

static void run_bookmark_selection(const SfeActionTarget* t)
{
    if (single_dir(t))
        toggle_bookmark(t->files->data);
}

static void run_split_vertical(const SfeActionTarget* t)
{
    sfe_split_split(sfe_window_get_split(t->window), t->pane, SFE_SPLIT_VERTICAL);
}

static void run_split_horizontal(const SfeActionTarget* t)
{
    sfe_split_split(sfe_window_get_split(t->window), t->pane, SFE_SPLIT_HORIZONTAL);
}

/* Closes the active pane, or the window when it is the last one. */
static void run_close_pane(const SfeActionTarget* t)
{
    SfeSplit* split = sfe_window_get_split(t->window);
    if (sfe_split_get_n_panes(split) > 1)
        sfe_split_close(split, t->pane);
    else
        gtk_window_close(GTK_WINDOW(t->window));
}

static void run_next_pane(const SfeActionTarget* t)
{
    sfe_split_focus_next(sfe_window_get_split(t->window));
}

/* ------------------------------------------------------------------------ */
/* Files                                                                     */
/* ------------------------------------------------------------------------ */

static void run_open(const SfeActionTarget* t)
{
    sfe_pane_open_files(t->pane, t->files);
}

static void on_app_chosen(GtkDialog* dialog, int response, gpointer data)
{
    GList* files = data;
    if (response == GTK_RESPONSE_OK) {
        g_autoptr(GAppInfo) app = gtk_app_chooser_get_app_info(GTK_APP_CHOOSER(dialog));
        if (app != NULL)
            launch_app(app, files, GTK_WIDGET(dialog));
    }
    g_list_free_full(files, g_object_unref);
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

static gpointer ref_copy(gconstpointer object, gpointer data)
{
    (void)data;
    return g_object_ref((gpointer)object);
}

static void run_open_with_other(const SfeActionTarget* t)
{
    if (t->files == NULL)
        return;
    GtkWidget* dialog = gtk_app_chooser_dialog_new(GTK_WINDOW(t->window), GTK_DIALOG_MODAL, t->files->data);
    /* Both classes, so its app list, entries and buttons use the palette. */
    gtk_style_context_add_class(gtk_widget_get_style_context(dialog), "sfe-window");
    gtk_style_context_add_class(gtk_widget_get_style_context(dialog), "sfe-dialog");
    g_signal_connect(dialog, "response", G_CALLBACK(on_app_chosen), g_list_copy_deep(t->files, ref_copy, NULL));
    gtk_widget_show(dialog);
}

static void run_cut(const SfeActionTarget* t)
{
    if (t->files != NULL)
        sfe_clipboard_set(t->files, true);
}

static void run_copy(const SfeActionTarget* t)
{
    if (t->files != NULL)
        sfe_clipboard_set(t->files, false);
}

typedef struct {
    SfeWindow* window;
    GFile* dest;
} PasteRequest;

static void on_cut_paste_done(SfeFileOp* op, GError* error, gpointer data)
{
    (void)op, (void)data;
    if (error == NULL)
        sfe_clipboard_clear_if_cut(); /* the files have moved */
}

static void on_clipboard_files(GList* files, bool cut, gpointer data)
{
    PasteRequest* req = data;

    if (files != NULL) {
        g_autoptr(SfeFileOp) op = sfe_file_op_new(cut ? SFE_OP_MOVE : SFE_OP_COPY, files, req->dest);
        if (cut)
            g_signal_connect(op, "finished", G_CALLBACK(on_cut_paste_done), NULL);
        sfe_window_run_op(req->window, op);
    }
    g_object_unref(req->window);
    g_object_unref(req->dest);
    g_free(req);
}

static void paste_into(SfeWindow* window, GFile* dest)
{
    PasteRequest* req = g_new0(PasteRequest, 1);
    req->window = g_object_ref(window);
    req->dest = g_object_ref(dest);
    sfe_clipboard_get(on_clipboard_files, req);
}

static void run_paste(const SfeActionTarget* t) { paste_into(t->window, t->dir); }

void sfe_actions_paste_into(SfeWindow* window, GFile* dest)
{
    paste_into(window, dest);
}

void sfe_actions_open_with(SfeWindow* window, GAppInfo* app, GList* files)
{
    launch_app(app, files, GTK_WIDGET(window));
}

static void run_paste_into(const SfeActionTarget* t)
{
    if (single_dir(t))
        paste_into(t->window, t->files->data);
}

/* Something is selected and some archive format can be created. */
static bool can_compress(const SfeActionTarget* t)
{
    return t->files != NULL && sfe_compress_available();
}

typedef struct {
    SfeWindow* window;
    GList* files;
} CompressRequest;

static void compress_request_free(gpointer p)
{
    CompressRequest* req = p;
    g_object_unref(req->window);
    g_list_free_full(req->files, g_object_unref);
    g_free(req);
}

static void on_compress(GFile* archive, gpointer data)
{
    CompressRequest* req = data;
    g_autoptr(SfeFileOp) op = sfe_file_op_new(SFE_OP_COMPRESS, req->files, archive);
    sfe_window_run_op(req->window, op);
}

/* The selection becomes one archive in this folder. */
static void run_compress(const SfeActionTarget* t)
{
    if (!can_compress(t))
        return;
    CompressRequest* req = g_new0(CompressRequest, 1);
    req->window = g_object_ref(t->window);
    req->files = g_list_copy_deep(t->files, ref_copy, NULL);
    sfe_compress_ask(GTK_WIDGET(t->window), t->files, t->dir, on_compress, req, compress_request_free);
}

/* Each archive goes into a new folder named after it, in this folder. */
static void run_extract(const SfeActionTarget* t)
{
    if (can_extract(t))
        run_op(t, SFE_OP_EXTRACT, t->files, t->dir);
}

static bool in_trash(const SfeActionTarget* t)
{
    return sfe_trash_is_trash_dir(t->dir);
}

static bool can_trash(const SfeActionTarget* t)
{
    return t->files != NULL && !in_trash(t);
}

static void run_delete(const SfeActionTarget* t);

/* In the trash itself, Delete deletes permanently (after asking). */
static void run_trash(const SfeActionTarget* t)
{
    if (in_trash(t))
        run_delete(t);
    else if (t->files != NULL)
        run_op(t, SFE_OP_TRASH, t->files, NULL);
}

static bool can_restore(const SfeActionTarget* t)
{
    return t->files != NULL && in_trash(t);
}

/* Puts trashed items back where they came from (a rename: the home trash
 * is on the home filesystem); problems are listed at the end. */
static void run_restore(const SfeActionTarget* t)
{
    g_autoptr(GString) problems = g_string_new(NULL);
    guint failed = 0;

    if (!can_restore(t))
        return;
    for (GList* l = t->files; l != NULL; l = l->next) {
        g_autoptr(GError) error = NULL;
        if (!sfe_trash_restore(l->data, &error)) {
            g_string_append_printf(problems, "%s%s", failed++ ? "\n" : "", error->message);
        }
    }
    if (failed > 0)
        sfe_show_error(widget_of(t), failed == 1 ? "Cannot restore an item" : "Cannot restore some items", problems->str);
}

typedef struct {
    SfeWindow* window;
} EmptyRequest;

static void empty_request_free(gpointer p)
{
    EmptyRequest* req = p;
    g_object_unref(req->window);
    g_free(req);
}

static void do_empty_trash(gpointer data)
{
    EmptyRequest* req = data;
    GList* all = sfe_trash_everything();
    if (all != NULL) {
        g_autoptr(SfeFileOp) op = sfe_file_op_new(SFE_OP_DELETE, all, NULL);
        sfe_window_run_op(req->window, op);
    }
    g_list_free_full(all, g_object_unref);
}

static bool can_empty_trash(const SfeActionTarget* t)
{
    return in_trash(t) && !sfe_trash_is_empty();
}

/* Also from the sidebar's Trash row, wherever the active view is. */
static void run_empty_trash(const SfeActionTarget* t)
{
    if (sfe_trash_is_empty())
        return;
    EmptyRequest* req = g_new0(EmptyRequest, 1);
    req->window = g_object_ref(t->window);
    sfe_confirm(widget_of(t), "Empty the trash?", "Everything in the trash is deleted permanently.",
        "Empty Trash", true, do_empty_trash, req, empty_request_free);
}

typedef struct {
    SfeWindow* window;
    GList* files;
} DeleteRequest;

static void delete_request_free(gpointer p)
{
    DeleteRequest* req = p;
    g_object_unref(req->window);
    g_list_free_full(req->files, g_object_unref);
    g_free(req);
}

static void do_delete(gpointer data)
{
    DeleteRequest* req = data;
    g_autoptr(SfeFileOp) op = sfe_file_op_new(SFE_OP_DELETE, req->files, NULL);
    sfe_window_run_op(req->window, op);
}

static void run_delete(const SfeActionTarget* t)
{
    if (t->files == NULL)
        return;

    DeleteRequest* req = g_new0(DeleteRequest, 1);
    req->window = g_object_ref(t->window);
    req->files = sfe_trash_with_info(t->files); /* trashed items lose their info files too */
    if (!sfe_config()->confirm_delete) {
        do_delete(req);
        delete_request_free(req);
        return;
    }

    guint n = g_list_length(t->files);
    g_autofree char* name = g_file_get_basename(t->files->data);
    g_autofree char* title = n == 1 ? g_strdup_printf("Permanently delete “%s”?", name)
                                    : g_strdup_printf("Permanently delete %u items?", n);
    sfe_confirm(widget_of(t), title, "This cannot be undone.", "Delete", true, do_delete, req, delete_request_free);
}

static void run_select_all(const SfeActionTarget* t) { sfe_pane_select_all(t->pane); }
static void run_search(const SfeActionTarget* t) { sfe_pane_start_search(t->pane); }
static void run_preferences(const SfeActionTarget* t) { sfe_show_preferences(widget_of(t)); }
/* Ctrl+Z in the path bar or a dialog's text field must not undo files. */
static void run_undo(const SfeActionTarget* t)
{
    if (!GTK_IS_EDITABLE(gtk_window_get_focus(GTK_WINDOW(t->window))))
        sfe_window_undo(t->window, 0);
}
static void run_toggle_sidebar(const SfeActionTarget* t) { sfe_window_toggle_sidebar(t->window); }
static void run_toggle_tree(const SfeActionTarget* t) { sfe_window_toggle_tree(t->window); }

typedef struct {
    SfePane* pane;
    GFile* dir;
    bool folder;
} CreateRequest;

static void create_request_free(gpointer p)
{
    CreateRequest* req = p;
    g_object_unref(req->pane);
    g_object_unref(req->dir);
    g_free(req);
}

static void on_new_name(const char* name, gpointer data)
{
    CreateRequest* req = data;
    g_autoptr(GFile) file = g_file_get_child(req->dir, name);
    g_autoptr(GError) error = NULL;
    bool ok;

    if (req->folder) {
        ok = g_file_make_directory(file, NULL, &error);
    } else {
        g_autoptr(GFileOutputStream) out = g_file_create(file, G_FILE_CREATE_NONE, NULL, &error);
        ok = out != NULL;
    }
    if (!ok) {
        g_autofree char* title = g_strdup_printf("Cannot create “%s”", name);
        sfe_show_error(GTK_WIDGET(req->pane), title, error->message);
        return;
    }
    sfe_undo_record_one(SFE_UNDO_CREATE, NULL, file);
    sfe_pane_reveal(req->pane, name);
}

static void ask_new(const SfeActionTarget* t, bool folder)
{
    CreateRequest* req = g_new0(CreateRequest, 1);
    req->pane = g_object_ref(t->pane);
    req->dir = g_object_ref(t->dir);
    req->folder = folder;
    sfe_ask_name(widget_of(t), folder ? "New Folder" : "New File", folder ? "New Folder" : "New File",
        "Create", on_new_name, req, create_request_free);
}

static void run_new_folder(const SfeActionTarget* t) { ask_new(t, true); }
static void run_new_file(const SfeActionTarget* t) { ask_new(t, false); }

/* After renaming, the first renamed file is selected again. */
static void on_renamed(GPtrArray* names, gpointer data)
{
    if (names->len > 0)
        sfe_pane_reveal(SFE_PANE(data), g_ptr_array_index(names, 0));
}

/* Not in the trash: a renamed item would lose its trash information. */
static bool can_rename(const SfeActionTarget* t)
{
    return t->files != NULL && !sfe_trash_is_trash_dir(t->dir);
}

static void run_rename(const SfeActionTarget* t)
{
    if (!can_rename(t))
        return;
    if (t->files->next == NULL)
        sfe_rename_one(widget_of(t), t->files->data, on_renamed, g_object_ref(t->pane), g_object_unref);
    else
        sfe_rename_many(widget_of(t), t->files, false, on_renamed, g_object_ref(t->pane), g_object_unref);
}

/* With nothing selected, every file shown in the folder is renamed. */
static void run_mass_rename(const SfeActionTarget* t)
{
    GList* all = t->files ? NULL : sfe_pane_get_all_files(t->pane);
    GList* files = t->files ? t->files : all;

    if (files != NULL) /* a whole folder: let the user pick files / folders */
        sfe_rename_many(widget_of(t), files, t->files == NULL, on_renamed, g_object_ref(t->pane), g_object_unref);
    g_list_free_full(all, g_object_unref);
}

static void run_properties(const SfeActionTarget* t)
{
    GList folder = { .data = t->dir };
    sfe_show_properties(widget_of(t), t->files ? t->files : &folder);
}

/* ------------------------------------------------------------------------ */
/* The table                                                                 */
/* ------------------------------------------------------------------------ */

typedef bool (*StateFunc)(void);

/* Toggle actions (shown with a check mark) and where they read their state. */
static const struct {
    const char* name;
    StateFunc state;
} toggles[] = {
    { "toggle-hidden", hidden_state },
};

#define ITEMS SFE_MENU_ITEMS
#define FOLDER SFE_MENU_FOLDER
#define NONE SFE_MENU_NONE

static const SfeAction actions[] = {
    /* name                 label                         shortcuts                         menu          section visible                 run */
    { "restore",            "Restore",                    { NULL },                         ITEMS,        0, can_restore,             run_restore },
    { "open",               "Open",                       { NULL },                         ITEMS,        0, has_files,               run_open },
    { "new-folder",         "New Folder…",                { "<Control><Shift>n", NULL },    FOLDER,       0, NULL,                    run_new_folder },
    { "new-file",           "New File…",                  { NULL },                         FOLDER,       0, NULL,                    run_new_file },
    { "cut",                "Cut",                        { "<Control>x", NULL },           ITEMS,        1, has_files,               run_cut },
    { "copy",               "Copy",                       { "<Control>c", NULL },           ITEMS,        1, has_files,               run_copy },
    { "paste",              "Paste",                      { "<Control>v", NULL },           FOLDER,       1, can_paste,               run_paste },
    { "paste-into",         "Paste Into Folder",          { NULL },                         ITEMS,        1, can_paste_into,          run_paste_into },
    { "rename",             "Rename…",                    { "F2", NULL },                   ITEMS,        2, can_rename,              run_rename },
    { "extract",            "Extract Here",               { NULL },                         ITEMS,        2, can_extract,             run_extract },
    { "compress",           "Compress…",                  { NULL },                         ITEMS,        2, can_compress,            run_compress },
    { "trash",              "Move to Trash",              { "Delete", NULL },               ITEMS,        3, can_trash,               run_trash },
    { "delete",             "Delete Permanently…",        { "<Shift>Delete", NULL },        ITEMS,        3, has_files,               run_delete },
    { "search",             "Search…",                    { "<Control>f", NULL },           FOLDER,       2, NULL,                    run_search },
    { "select-all",         "Select All",                 { "<Control>a", NULL },           FOLDER,       2, NULL,                    run_select_all },
    { "toggle-hidden",      "Show Hidden Files",          { "<Control>h", NULL },           FOLDER,       2, NULL,                    run_toggle_hidden },
    { "bookmark-selection", "Add to Bookmarks",           { NULL },                         ITEMS,        4, can_bookmark_selection,  run_bookmark_selection },
    { "unbookmark-selection", "Remove from Bookmarks",    { NULL },                         ITEMS,        4, can_unbookmark_selection, run_bookmark_selection },
    { "bookmark-folder",    "Add to Bookmarks",           { NULL },                         FOLDER,       4, can_bookmark_folder,     run_bookmark },
    { "unbookmark-folder",  "Remove from Bookmarks",      { NULL },                         FOLDER,       4, can_unbookmark_folder,   run_bookmark },
    { "empty-trash",        "Empty Trash…",               { NULL },                         FOLDER,       3, can_empty_trash,         run_empty_trash },
    { "properties",         "Properties",                 { "<Alt>Return", NULL },          ITEMS|FOLDER, 9, NULL,                    run_properties },

    { "preferences",        "Preferences",                { "<Control>p", NULL },           NONE,         0, NULL,                    run_preferences },
    { "mass-rename",        "Rename with Pattern…",       { "<Shift>F2", NULL },            NONE,         0, NULL,                    run_mass_rename },
    { "open-with-other",    "Other Application…",         { NULL },                         NONE,         0, NULL,                    run_open_with_other },
    { "back",               "Back",                       { "<Alt>Left", NULL },            NONE,         0, NULL,                    run_back },
    { "forward",            "Forward",                    { "<Alt>Right", NULL },           NONE,         0, NULL,                    run_forward },
    { "up",                 "Parent Folder",              { "<Alt>Up", NULL },              NONE,         0, NULL,                    run_up },
    { "reload",             "Reload",                     { "F5", "<Control>r", NULL },     NONE,         0, NULL,                    run_reload },
    { "location",           "Edit Location",              { "<Control>l", NULL },           NONE,         0, NULL,                    run_location },
    { "view-grid",          "Grid View",                  { "<Control>1", NULL },           NONE,         0, NULL,                    run_view_grid },
    { "view-list",          "List View",                  { "<Control>2", NULL },           NONE,         0, NULL,                    run_view_list },
    { "bookmark",           "Bookmark This Folder",       { "<Control>d", NULL },           NONE,         0, NULL,                    run_bookmark },
    { "split-vertical",     "Split Side by Side",         { "<Control>backslash", NULL },   NONE,         0, NULL,                    run_split_vertical },
    { "split-horizontal",   "Split Top and Bottom",       { "<Control>minus", NULL },       NONE,         0, NULL,                    run_split_horizontal },
    { "close-pane",         "Close View",                 { NULL },                         NONE,         0, NULL,                    run_close_pane },
    { "next-pane",          "Next View",                  { "F6", NULL },                   NONE,         0, NULL,                    run_next_pane },
    { "undo",               "Undo",                       { "<Control>z", NULL },           NONE,         0, NULL,                    run_undo },
    { "toggle-sidebar",     "Hide / Show the Sidebar",    { "<Control>s", NULL },           NONE,         0, NULL,                    run_toggle_sidebar },
    { "toggle-tree",        "Folder Tree Fills the Sidebar", { NULL },                      NONE,         0, NULL,                    run_toggle_tree },
};

const SfeAction* sfe_actions_get(size_t* n)
{
    *n = G_N_ELEMENTS(actions);
    return actions;
}

/* ------------------------------------------------------------------------ */
/* Registration                                                              */
/* ------------------------------------------------------------------------ */

typedef struct {
    SfeWindow* window;
    const SfeAction* action;
} Binding;

static void free_binding(gpointer data, GClosure* closure)
{
    (void)closure;
    g_free(data);
}

static void on_activate(GSimpleAction* simple, GVariant* param, gpointer data)
{
    (void)simple, (void)param;
    Binding* b = data;
    SfeActionTarget t = sfe_action_target_for(b->window);
    b->action->run(&t);
    sfe_action_target_clear(&t);
}

static void on_open_with(GSimpleAction* simple, GVariant* param, gpointer data)
{
    (void)simple;
    SfeActionTarget t = sfe_action_target_for(data);
    g_autoptr(GDesktopAppInfo) app = g_desktop_app_info_new(g_variant_get_string(param, NULL));

    if (app != NULL && t.files != NULL)
        launch_app(G_APP_INFO(app), t.files, GTK_WIDGET(t.window));
    sfe_action_target_clear(&t);
}

unsigned sfe_action_target_kinds(const SfeActionTarget* t)
{
    unsigned kinds = 0;
    if (t->files == NULL)
        return SFE_ACTION_DIRECTORIES; /* the current folder */
    for (GList* l = t->files; l != NULL; l = l->next)
        kinds |= is_dir(l->data) ? SFE_ACTION_DIRECTORIES : SFE_ACTION_FILES;
    return kinds;
}

const SfeUserAction* sfe_find_user_action(const char* name, unsigned kind)
{
    const SfeConfig* cfg = sfe_config();
    for (guint i = 0; i < cfg->actions->len; i++) {
        const SfeUserAction* ua = g_ptr_array_index(cfg->actions, i);
        if (strcmp(ua->name, name) == 0 && (ua->applies_to & kind))
            return ua;
    }
    return NULL;
}

static void run_user_command(const SfeUserAction* ua, GList* items, const SfeActionTarget* t)
{
    g_autoptr(GError) error = NULL;
    if (ua == NULL || items == NULL)
        return;
    if (!sfe_run_user_command(ua->command, items, t->dir, &error)) {
        g_autofree char* title = g_strdup_printf("Cannot run “%s”", ua->name);
        sfe_show_error(GTK_WIDGET(t->window), title, error->message);
    }
}

/* Files go to the command defined for files, directories to the one for
 * directories; with nothing selected the current folder is the directory. */
static void on_user_action(GSimpleAction* simple, GVariant* param, gpointer data)
{
    (void)simple;
    const char* name = g_variant_get_string(param, NULL);
    SfeActionTarget t = sfe_action_target_for(data);
    GList* files = NULL;
    GList* dirs = NULL;

    if (t.files == NULL)
        dirs = g_list_append(NULL, t.dir);
    for (GList* l = t.files; l != NULL; l = l->next) {
        if (is_dir(l->data))
            dirs = g_list_append(dirs, l->data);
        else
            files = g_list_append(files, l->data);
    }
    run_user_command(sfe_find_user_action(name, SFE_ACTION_FILES), files, &t);
    run_user_command(sfe_find_user_action(name, SFE_ACTION_DIRECTORIES), dirs, &t);
    g_list_free(files);
    g_list_free(dirs);
    sfe_action_target_clear(&t);
}

static StateFunc state_func(const char* name)
{
    for (size_t i = 0; i < G_N_ELEMENTS(toggles); i++) {
        if (strcmp(toggles[i].name, name) == 0)
            return toggles[i].state;
    }
    return NULL;
}

void sfe_actions_add_to_window(SfeWindow* window)
{
    for (size_t i = 0; i < G_N_ELEMENTS(actions); i++) {
        StateFunc state = state_func(actions[i].name);
        GSimpleAction* simple = state
            ? g_simple_action_new_stateful(actions[i].name, NULL, g_variant_new_boolean(state()))
            : g_simple_action_new(actions[i].name, NULL);
        Binding* b = g_new(Binding, 1);
        b->window = window;
        b->action = &actions[i];
        g_signal_connect_data(simple, "activate", G_CALLBACK(on_activate), b, free_binding, 0);
        g_action_map_add_action(G_ACTION_MAP(window), G_ACTION(simple));
        g_object_unref(simple);
    }

    GSimpleAction* open_with = g_simple_action_new("open-with", G_VARIANT_TYPE_STRING);
    g_signal_connect(open_with, "activate", G_CALLBACK(on_open_with), window);
    g_action_map_add_action(G_ACTION_MAP(window), G_ACTION(open_with));
    g_object_unref(open_with);

    GSimpleAction* user = g_simple_action_new("user-action", G_VARIANT_TYPE_STRING);
    g_signal_connect(user, "activate", G_CALLBACK(on_user_action), window);
    g_action_map_add_action(G_ACTION_MAP(window), G_ACTION(user));
    g_object_unref(user);
}

void sfe_actions_sync_state(SfeWindow* window)
{
    for (size_t i = 0; i < G_N_ELEMENTS(toggles); i++) {
        GAction* action = g_action_map_lookup_action(G_ACTION_MAP(window), toggles[i].name);
        if (action != NULL)
            g_simple_action_set_state(G_SIMPLE_ACTION(action), g_variant_new_boolean(toggles[i].state()));
    }
}

void sfe_actions_set_accels(GtkApplication* app)
{
    for (size_t i = 0; i < G_N_ELEMENTS(actions); i++) {
        if (actions[i].accels[0] == NULL)
            continue;
        g_autofree char* detailed = g_strconcat("win.", actions[i].name, NULL);
        gtk_application_set_accels_for_action(app, detailed, actions[i].accels);
    }
}
