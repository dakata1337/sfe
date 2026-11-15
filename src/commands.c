#include "commands.h"

#include "actions.h"
#include "app.h"
#include "bookmarks.h"
#include "devices.h"
#include "dialogs.h"
#include "fileops.h"
#include "rename.h"
#include "split.h"
#include "util.h"
#include <stdarg.h>
#include <string.h>

#define SFE_COMMAND_ERROR (g_quark_from_static_string("sfe-command-error"))

/* Where a command runs. */
typedef struct {
    SfeWindow* window;
    SfeSplit* split;
    SfePane* pane; /* the active one */
    GFile* dir; /* its folder */
} Ctx;

typedef bool (*RunFunc)(Ctx* c, int argc, char** argv, GError** error);

/* Adds candidates for argument `argi` (argv holds the ones before it, argv[0]
 * the command); may point `rank` at the part of `query` to rank by. */
typedef void (*CompleteFunc)(Ctx* c, int argi, char** argv, const char* query, const char** rank, GPtrArray* out);

typedef struct {
    SfeCommandInfo info;
    CompleteFunc complete;
    RunFunc run;
} Command;

static G_GNUC_PRINTF(2, 3) bool fail(GError** error, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    g_autofree char* text = g_strdup_vprintf(format, args);
    va_end(args);
    g_set_error_literal(error, SFE_COMMAND_ERROR, 0, text);
    return false;
}

/* ------------------------------------------------------------------------ */
/* Paths and files                                                           */
/* ------------------------------------------------------------------------ */

/* "~/x", "/x" or relative to the active view's folder. */
static GFile* resolve(Ctx* c, const char* arg)
{
    g_autofree char* expanded = sfe_expand_home(arg);
    if (g_path_is_absolute(expanded))
        return g_file_new_for_path(expanded);
    return g_file_resolve_relative_path(c->dir, expanded);
}

static GList* resolve_all(Ctx* c, int argc, char** argv)
{
    GList* files = NULL;
    for (int i = 0; i < argc; i++)
        files = g_list_prepend(files, resolve(c, argv[i]));
    return g_list_reverse(files);
}

static bool is_dir(GFile* file)
{
    return g_file_query_file_type(file, G_FILE_QUERY_INFO_NONE, NULL) == G_FILE_TYPE_DIRECTORY;
}

static char* display(GFile* file)
{
    g_autofree char* path = g_file_get_path(file);
    return path ? sfe_contract_home(path) : g_file_get_uri(file);
}

/* All given files exist; else a message naming the first missing one. */
static bool all_exist(GList* files, GError** error)
{
    for (GList* l = files; l != NULL; l = l->next) {
        if (!g_file_query_exists(l->data, NULL)) {
            g_autofree char* name = display(l->data);
            return fail(error, "%s does not exist", name);
        }
    }
    return true;
}

/* ------------------------------------------------------------------------ */
/* Actions as commands                                                       */
/* ------------------------------------------------------------------------ */

/* The action for a command name ("select_all" -> "select-all"). */
static const SfeAction* find_action(const char* command)
{
    g_autofree char* name = g_strdup(command);
    g_strdelimit(name, "_", '-');
    size_t n = 0;
    const SfeAction* table = sfe_actions_get(&n);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(table[i].name, name) == 0)
            return &table[i];
    }
    return NULL;
}

/* Runs an action on `files` (taken), or on the selection when NULL. */
static bool run_action(Ctx* c, const char* name, GList* files, GError** error)
{
    const SfeAction* a = find_action(name);
    if (a == NULL) {
        g_list_free_full(files, g_object_unref);
        return fail(error, "unknown command: %s", name);
    }
    SfeActionTarget t = { .window = c->window, .pane = c->pane };
    t.files = files ? files : sfe_pane_get_selected_files(c->pane);
    t.dir = g_object_ref(c->dir);
    a->run(&t);
    sfe_action_target_clear(&t);
    return true;
}

/* A command that runs its action on the given files or the selection. */
static bool run_on_files(Ctx* c, const char* action, int argc, char** argv, GError** error)
{
    GList* files = argc > 1 ? resolve_all(c, argc - 1, argv + 1) : NULL;
    if (!all_exist(files, error)) {
        g_list_free_full(files, g_object_unref);
        return false;
    }
    return run_action(c, action, files, error);
}

/* ------------------------------------------------------------------------ */
/* Commands                                                                  */
/* ------------------------------------------------------------------------ */

static bool run_cd(Ctx* c, int argc, char** argv, GError** error)
{
    g_autoptr(GFile) dir = argc > 1 ? resolve(c, argv[1]) : g_file_new_for_path(g_get_home_dir());
    if (!is_dir(dir)) {
        g_autofree char* name = display(dir);
        return fail(error, "%s is not a folder", name);
    }
    sfe_pane_navigate(c->pane, dir);
    return true;
}

static bool run_open(Ctx* c, int argc, char** argv, GError** error)
{
    return run_on_files(c, "open", argc, argv, error);
}

/* copy / move: sources… then an existing destination folder. */
static bool run_transfer(Ctx* c, SfeOpKind kind, int argc, char** argv, GError** error)
{
    if (argc < 3)
        return fail(error, "usage: %s <file>… <destination folder>", argv[0]);
    g_autoptr(GFile) dest = resolve(c, argv[argc - 1]);
    if (!is_dir(dest)) {
        g_autofree char* name = display(dest);
        return fail(error, "%s is not a folder", name);
    }
    GList* sources = resolve_all(c, argc - 2, argv + 1);
    bool ok = all_exist(sources, error);
    if (ok) {
        g_autoptr(SfeFileOp) op = sfe_file_op_new(kind, sources, dest);
        sfe_window_run_op(c->window, op);
    }
    g_list_free_full(sources, g_object_unref);
    return ok;
}

static bool run_copy(Ctx* c, int argc, char** argv, GError** error)
{
    return run_transfer(c, SFE_OP_COPY, argc, argv, error);
}

static bool run_move(Ctx* c, int argc, char** argv, GError** error)
{
    return run_transfer(c, SFE_OP_MOVE, argc, argv, error);
}

/* Links in a folder (named like their targets) or, for one target, a link
 * with the given name. */
static bool run_symlink(Ctx* c, int argc, char** argv, GError** error)
{
    if (argc < 3)
        return fail(error, "usage: symlink <target>… <destination folder or link name>");
    g_autoptr(GFile) dest = resolve(c, argv[argc - 1]);
    bool into = is_dir(dest);
    if (!into && argc > 3)
        return fail(error, "several targets need a destination folder");

    for (int i = 1; i < argc - 1; i++) {
        g_autoptr(GFile) target = resolve(c, argv[i]);
        g_autofree char* target_path = g_file_get_path(target);
        g_autofree char* base = g_file_get_basename(target);
        g_autoptr(GFile) link = into ? g_file_get_child(dest, base) : g_object_ref(dest);
        if (target_path == NULL || !g_file_query_exists(target, NULL)) {
            g_autofree char* name = display(target);
            return fail(error, "%s does not exist", name);
        }
        if (!g_file_make_symbolic_link(link, target_path, NULL, error))
            return false;
    }
    return true;
}

static bool run_rename(Ctx* c, int argc, char** argv, GError** error)
{
    if (argc == 1)
        return run_action(c, "rename", NULL, error);
    if (argc != 3)
        return fail(error, "usage: rename <file> <new name>");
    g_autoptr(GFile) file = resolve(c, argv[1]);
    GFile* files[] = { file };
    char* names[] = { argv[2] };
    SfeRenameStatus status = SFE_RENAME_OK;

    if (!g_file_query_exists(file, NULL)) {
        g_autofree char* name = display(file);
        return fail(error, "%s does not exist", name);
    }
    if (sfe_rename_check(files, names, 1, &status) > 0)
        return fail(error, "cannot rename to “%s”: %s", argv[2], sfe_rename_status_text(status));
    if (status == SFE_RENAME_UNCHANGED)
        return true;
    return sfe_rename_files(files, names, 1, error);
}

static bool run_mass_rename(Ctx* c, int argc, char** argv, GError** error)
{
    return run_on_files(c, "mass-rename", argc, argv, error);
}

/* new_file / new_directory: names in the active folder ("a/b" makes the
 * folders on the way for new_directory). */
static bool run_new(Ctx* c, bool directory, int argc, char** argv, GError** error)
{
    if (argc == 1)
        return run_action(c, directory ? "new-folder" : "new-file", NULL, error);
    for (int i = 1; i < argc; i++) {
        g_autoptr(GFile) file = resolve(c, argv[i]);
        bool ok;
        if (directory) {
            ok = g_file_make_directory_with_parents(file, NULL, error);
        } else {
            g_autoptr(GFileOutputStream) out = g_file_create(file, G_FILE_CREATE_NONE, NULL, error);
            ok = out != NULL;
        }
        if (!ok)
            return false;
        if (argc == 2 && strchr(argv[1], '/') == NULL)
            sfe_pane_reveal(c->pane, argv[1]); /* select it once it shows up */
    }
    return true;
}

static bool run_new_file(Ctx* c, int argc, char** argv, GError** error)
{
    return run_new(c, false, argc, argv, error);
}

static bool run_new_directory(Ctx* c, int argc, char** argv, GError** error)
{
    return run_new(c, true, argc, argv, error);
}

static bool run_trash(Ctx* c, int argc, char** argv, GError** error)
{
    return run_on_files(c, "trash", argc, argv, error);
}

static bool run_delete(Ctx* c, int argc, char** argv, GError** error)
{
    return run_on_files(c, "delete", argc, argv, error);
}

static bool run_copy_clipboard(Ctx* c, int argc, char** argv, GError** error)
{
    return run_on_files(c, "copy", argc, argv, error);
}

static bool run_cut(Ctx* c, int argc, char** argv, GError** error)
{
    return run_on_files(c, "cut", argc, argv, error);
}

/* ---- Devices ------------------------------------------------------------ */

static const SfeDevice* find_device(const char* name)
{
    GPtrArray* list = sfe_devices_get_list(sfe_devices_get_default());
    for (guint i = 0; list != NULL && i < list->len; i++) {
        const SfeDevice* d = g_ptr_array_index(list, i);
        g_autofree char* mount = d->mount_path ? sfe_contract_home(d->mount_path) : NULL;
        if (g_strcmp0(d->name, name) == 0 || g_strcmp0(d->id, name) == 0 || g_strcmp0(d->mount_path, name) == 0
            || g_strcmp0(mount, name) == 0)
            return d;
    }
    return NULL;
}

static void on_mounted(GObject* source, GAsyncResult* res, gpointer data)
{
    g_autoptr(SfeWindow) window = data;
    g_autoptr(GError) error = NULL;
    g_autofree char* path = sfe_devices_mount_finish(SFE_DEVICES(source), res, &error);
    if (path == NULL) {
        sfe_show_error(GTK_WIDGET(window), "Cannot mount the device", error->message);
        return;
    }
    g_autoptr(GFile) dir = g_file_new_for_path(path);
    sfe_pane_navigate(sfe_window_get_active_pane(window), dir);
}

static void on_unmounted(GObject* source, GAsyncResult* res, gpointer data)
{
    g_autoptr(SfeWindow) window = data;
    g_autoptr(GError) error = NULL;
    if (!sfe_devices_unmount_finish(SFE_DEVICES(source), res, &error))
        sfe_show_error(GTK_WIDGET(window), "Cannot unmount the device", error->message);
}

static bool run_device(Ctx* c, int argc, char** argv, bool mount, bool eject, GError** error)
{
    if (argc != 2)
        return fail(error, "usage: %s <device>", argv[0]);
    const SfeDevice* d = find_device(argv[1]);
    if (d == NULL)
        return fail(error, "no device “%s”", argv[1]);
    if (mount) {
        if (d->mount_path != NULL) {
            g_autoptr(GFile) dir = g_file_new_for_path(d->mount_path);
            sfe_pane_navigate(c->pane, dir); /* already mounted: just go there */
            return true;
        }
        sfe_devices_mount(sfe_devices_get_default(), d->id, NULL, on_mounted, g_object_ref(c->window));
    } else {
        if (d->mount_path == NULL && !eject)
            return fail(error, "“%s” is not mounted", d->name);
        sfe_devices_unmount(sfe_devices_get_default(), d->id, eject, NULL, on_unmounted, g_object_ref(c->window));
    }
    return true;
}

static bool run_mount(Ctx* c, int argc, char** argv, GError** error)
{
    return run_device(c, argc, argv, true, false, error);
}

static bool run_unmount(Ctx* c, int argc, char** argv, GError** error)
{
    return run_device(c, argc, argv, false, false, error);
}

static bool run_eject(Ctx* c, int argc, char** argv, GError** error)
{
    return run_device(c, argc, argv, false, true, error);
}

/* ---- Splits ------------------------------------------------------------- */

static const struct {
    const char* name;
    SfeDirection direction;
} directions[] = {
    { "left", SFE_DIRECTION_LEFT },
    { "right", SFE_DIRECTION_RIGHT },
    { "up", SFE_DIRECTION_UP },
    { "down", SFE_DIRECTION_DOWN },
};

static bool parse_direction(const char* name, SfeDirection* direction)
{
    for (size_t i = 0; i < G_N_ELEMENTS(directions); i++) {
        if (g_strcmp0(directions[i].name, name) == 0) {
            *direction = directions[i].direction;
            return true;
        }
    }
    return false;
}

static bool run_split(Ctx* c, int argc, char** argv, GError** error)
{
    const char* sub = argc > 1 ? argv[1] : "";
    SfeDirection direction;

    if (strcmp(sub, "vertical") == 0 || strcmp(sub, "horizontal") == 0) {
        int before = sfe_split_get_n_panes(c->split);
        sfe_split_split(c->split, c->pane, sub[0] == 'v' ? SFE_SPLIT_VERTICAL : SFE_SPLIT_HORIZONTAL);
        if (sfe_split_get_n_panes(c->split) == before)
            return fail(error, "this view cannot split %s", sub[0] == 'v' ? "side by side" : "top / bottom");
        return true;
    }
    if (strcmp(sub, "next") == 0) {
        sfe_split_focus_next(c->split);
        return true;
    }
    if (strcmp(sub, "close") == 0)
        return run_action(c, "close-pane", NULL, error);
    if (parse_direction(sub, &direction)) {
        SfePane* other = sfe_split_neighbor(c->split, c->pane, direction);
        if (other == NULL)
            return fail(error, "no view %s", sub);
        sfe_split_focus(c->split, other);
        return true;
    }
    if (strcmp(sub, "move") == 0) {
        if (argc != 3 || !parse_direction(argv[2], &direction))
            return fail(error, "usage: split move left|right|up|down");
        if (!sfe_split_swap(c->split, direction))
            return fail(error, "no view %s to swap with", argv[2]);
        return true;
    }
    if (strcmp(sub, "absolute") == 0) {
        SfePane* pane = argc == 3 ? sfe_split_find_position(c->split, argv[2]) : NULL;
        if (pane == NULL)
            return fail(error, "no view at %s", argc == 3 ? argv[2] : "(missing position)");
        sfe_split_focus(c->split, pane);
        return true;
    }
    return fail(error, "usage: split move|next|vertical|horizontal|left|right|up|down|absolute|close");
}

/* ---- Bookmarks ---------------------------------------------------------- */

static bool run_bookmark(Ctx* c, int argc, char** argv, GError** error)
{
    g_autoptr(GFile) dir = argc > 1 ? resolve(c, argv[1]) : g_object_ref(c->dir);
    if (!is_dir(dir)) {
        g_autofree char* name = display(dir);
        return fail(error, "%s is not a folder", name);
    }
    if (sfe_bookmarks_find(sfe_config(), dir) >= 0)
        return true;
    SfeConfig* cfg = sfe_application_dup_config(sfe_app());
    sfe_bookmarks_add(cfg, dir);
    sfe_application_set_config(sfe_app(), cfg);
    return true;
}

/* A bookmark by path or by its label. */
static bool run_unbookmark(Ctx* c, int argc, char** argv, GError** error)
{
    g_autoptr(GFile) dir = argc > 1 ? resolve(c, argv[1]) : g_object_ref(c->dir);
    int index = sfe_bookmarks_find(sfe_config(), dir);
    if (index < 0 && argc > 1) {
        g_autoptr(GPtrArray) list = sfe_bookmarks_list(sfe_config(), NULL);
        for (guint i = 0; i < list->len; i++) {
            const SfeBookmark* b = g_ptr_array_index(list, i);
            if (b->config_index >= 0 && g_strcmp0(b->label, argv[1]) == 0)
                index = b->config_index;
        }
    }
    if (index < 0)
        return fail(error, "not one of sfe's bookmarks (GTK bookmarks are edited elsewhere)");
    SfeConfig* cfg = sfe_application_dup_config(sfe_app());
    sfe_bookmarks_remove(cfg, index);
    sfe_application_set_config(sfe_app(), cfg);
    return true;
}

/* ---- Small settings ------------------------------------------------------ */

static bool run_hidden(Ctx* c, int argc, char** argv, GError** error)
{
    (void)c;
    const char* how = argc > 1 ? argv[1] : "toggle";
    bool show;
    if (strcmp(how, "on") == 0)
        show = true;
    else if (strcmp(how, "off") == 0)
        show = false;
    else if (strcmp(how, "toggle") == 0)
        show = !sfe_config()->show_hidden;
    else
        return fail(error, "usage: hidden on|off|toggle");
    if (show != sfe_config()->show_hidden) {
        SfeConfig* cfg = sfe_application_dup_config(sfe_app());
        cfg->show_hidden = show;
        sfe_application_set_config(sfe_app(), cfg);
    }
    return true;
}

static bool run_view(Ctx* c, int argc, char** argv, GError** error)
{
    if (argc == 2 && strcmp(argv[1], "grid") == 0)
        sfe_pane_set_view(c->pane, SFE_VIEW_GRID);
    else if (argc == 2 && strcmp(argv[1], "list") == 0)
        sfe_pane_set_view(c->pane, SFE_VIEW_LIST);
    else
        return fail(error, "usage: view grid|list");
    return true;
}

static bool run_q(Ctx* c, int argc, char** argv, GError** error)
{
    (void)argc, (void)argv;
    return run_action(c, "close-pane", NULL, error);
}

static bool run_cursor(Ctx* c, int argc, char** argv, GError** error)
{
    static const struct {
        const char* name;
        SfeCursorMove move;
    } moves[] = {
        { "down", SFE_CURSOR_DOWN },
        { "up", SFE_CURSOR_UP },
        { "first", SFE_CURSOR_FIRST },
        { "last", SFE_CURSOR_LAST },
        { "previous", SFE_CURSOR_PREVIOUS },
        { "next", SFE_CURSOR_NEXT },
    };
    for (size_t i = 0; argc == 2 && i < G_N_ELEMENTS(moves); i++) {
        if (strcmp(argv[1], moves[i].name) == 0) {
            sfe_pane_move_cursor(c->pane, moves[i].move);
            return true;
        }
    }
    return fail(error, "usage: cursor down|up|first|last|previous|next");
}

static bool run_select(Ctx* c, int argc, char** argv, GError** error)
{
    if (argc == 2 && strcmp(argv[1], "toggle") == 0)
        sfe_pane_toggle_cursor_item(c->pane);
    else if (argc == 2 && strcmp(argv[1], "all") == 0)
        sfe_pane_select_all(c->pane);
    else
        return fail(error, "usage: select toggle|all");
    return true;
}

static bool run_visual(Ctx* c, int argc, char** argv, GError** error)
{
    if (argc == 1)
        sfe_pane_visual_chunk(c->pane);
    else if (argc == 2 && strcmp(argv[1], "off") == 0)
        sfe_pane_visual_end(c->pane);
    else if (argc == 2 && strcmp(argv[1], "swap") == 0)
        sfe_pane_visual_swap(c->pane);
    else
        return fail(error, "usage: visual [off|swap]");
    return true;
}

static bool run_command_line(Ctx* c, int argc, char** argv, GError** error)
{
    (void)argc, (void)argv, (void)error;
    sfe_window_show_command_line(c->window, NULL);
    return true;
}

/* ------------------------------------------------------------------------ */
/* Completion sources                                                        */
/* ------------------------------------------------------------------------ */

/* The panes after the active one, clockwise: right-top, right-bottom, left. */
static int n_others(Ctx* c)
{
    return sfe_split_get_n_panes(c->split) - 1;
}

static SfePane* other_pane(Ctx* c, int k, const char** position)
{
    int n = sfe_split_get_n_panes(c->split);
    int active = 0;
    for (int i = 0; i < n; i++) {
        if (sfe_split_get_pane(c->split, i) == c->pane)
            active = i;
    }
    int index = (active + 1 + k) % n;
    if (position != NULL)
        *position = sfe_split_position_name(c->split, index);
    return sfe_split_get_pane(c->split, index);
}

static const char* active_position(Ctx* c)
{
    int n = sfe_split_get_n_panes(c->split);
    for (int i = 0; i < n; i++) {
        if (sfe_split_get_pane(c->split, i) == c->pane)
            return n > 1 ? sfe_split_position_name(c->split, i) : "here";
    }
    return "here";
}

static void add(GPtrArray* out, const char* insert, const char* match, const char* tag, int group)
{
    g_ptr_array_add(out, sfe_candidate_new(insert, match, tag, group));
}

static void add_words(GPtrArray* out, const char* const* words, int group)
{
    for (int i = 0; words[i] != NULL; i++)
        add(out, words[i], NULL, NULL, group);
}

/* The entries of `pane`: names for the active view, "~/..." paths for the
 * others; folders end in "/" so typing on walks into them. */
static void add_pane_entries(GPtrArray* out, SfePane* pane, bool full_paths, bool dirs_only, const char* tag, int group)
{
    g_autoptr(GPtrArray) entries = sfe_pane_get_entries(pane);
    for (guint i = 0; i < entries->len; i++) {
        const SfePaneEntry* e = g_ptr_array_index(entries, i);
        if (dirs_only && !e->is_dir)
            continue;
        g_autofree char* base = full_paths ? display(e->file) : g_strdup(e->name);
        g_autofree char* insert = g_strconcat(base, e->is_dir ? "/" : "", NULL);
        add(out, insert, e->name, tag, group);
    }
}

/* Typed text with a "/": the entries of the folder it names, matched
 * against what follows the last "/". Returns false without a "/". */
static bool add_walk(Ctx* c, const char* query, bool dirs_only, const char** rank, GPtrArray* out)
{
    const char* slash = strrchr(query, '/');
    if (slash == NULL)
        return false;
    g_autofree char* prefix = g_strndup(query, slash - query + 1);
    g_autoptr(GFile) dir = resolve(c, prefix);
    g_autoptr(GFileEnumerator) en = g_file_enumerate_children(dir,
        G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN,
        G_FILE_QUERY_INFO_NONE, NULL, NULL);
    bool hidden = sfe_config()->show_hidden || slash[1] == '.';
    GFileInfo* info;

    *rank = slash + 1;
    while (en != NULL && (info = g_file_enumerator_next_file(en, NULL, NULL)) != NULL) {
        bool d = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
        if ((!dirs_only || d) && (hidden || !g_file_info_get_is_hidden(info))) {
            const char* name = g_file_info_get_display_name(info);
            g_autofree char* insert = g_strconcat(prefix, name, d ? "/" : "", NULL);
            add(out, insert, name, NULL, 0);
        }
        g_object_unref(info);
    }
    return true;
}

/* Files: the active view first, then the others clockwise. */
static void add_files(Ctx* c, const char* query, bool dirs_only, const char** rank, GPtrArray* out, int group)
{
    if (add_walk(c, query, dirs_only, rank, out))
        return;
    add_pane_entries(out, c->pane, false, dirs_only, active_position(c), group);
    for (int k = 0; k < n_others(c); k++) {
        const char* position = NULL;
        SfePane* pane = other_pane(c, k, &position);
        add_pane_entries(out, pane, true, dirs_only, position, group + 1 + k);
    }
}

/* The other views' folders themselves. */
static void add_other_folders(Ctx* c, GPtrArray* out, int group)
{
    for (int k = 0; k < n_others(c); k++) {
        const char* position = NULL;
        SfePane* pane = other_pane(c, k, &position);
        GFile* dir = sfe_pane_get_dir(pane);
        g_autofree char* path = display(dir);
        g_autofree char* insert = g_strconcat(path, g_str_has_suffix(path, "/") ? "" : "/", NULL);
        g_autofree char* name = g_file_get_basename(dir);
        add(out, insert, name, position, group);
    }
}

static void add_bookmarks(GPtrArray* out, bool sfe_only, int group)
{
    g_autoptr(GPtrArray) list = sfe_bookmarks_list(sfe_config(), NULL);
    for (guint i = 0; i < list->len; i++) {
        const SfeBookmark* b = g_ptr_array_index(list, i);
        if (sfe_only && b->config_index < 0)
            continue;
        g_autofree char* path = display(b->file);
        g_autofree char* insert = g_strconcat(path, g_str_has_suffix(path, "/") ? "" : "/", NULL);
        add(out, insert, b->label, "bookmark", group);
    }
}

/* ------------------------------------------------------------------------ */
/* Completers                                                                */
/* ------------------------------------------------------------------------ */

static void complete_files(Ctx* c, int argi, char** argv, const char* query, const char** rank, GPtrArray* out)
{
    (void)argi, (void)argv;
    add_files(c, query, false, rank, out, 0);
}

static void complete_dirs(Ctx* c, int argi, char** argv, const char* query, const char** rank, GPtrArray* out)
{
    (void)argv;
    if (argi != 1)
        return;
    if (add_walk(c, query, true, rank, out))
        return;
    add_pane_entries(out, c->pane, false, true, active_position(c), 0);
    add_other_folders(c, out, 1);
    add_bookmarks(out, false, 2);
}

/* The first argument: files; later ones: the other views' folders first
 * (the usual destination), then the files of the views. */
static void complete_transfer(Ctx* c, int argi, char** argv, const char* query, const char** rank, GPtrArray* out)
{
    (void)argv;
    if (argi == 1 || strchr(query, '/') != NULL) {
        add_files(c, query, false, rank, out, 0);
        return;
    }
    add_other_folders(c, out, 0);
    add_files(c, query, false, rank, out, 1);
}

static void complete_rename(Ctx* c, int argi, char** argv, const char* query, const char** rank, GPtrArray* out)
{
    if (argi == 1) {
        add_files(c, query, false, rank, out, 0);
    } else if (argi == 2) { /* the current name, to edit */
        g_autoptr(GFile) file = resolve(c, argv[1]);
        g_autofree char* name = g_file_get_basename(file);
        add(out, name, NULL, "current name", 0);
        *rank = "";
    }
}

static void complete_devices(Ctx* c, int argi, char** argv, const char* query, const char** rank, GPtrArray* out)
{
    (void)c, (void)query, (void)rank;
    if (argi != 1)
        return;
    bool mount = strcmp(argv[0], "mount") == 0;
    bool eject = strcmp(argv[0], "eject") == 0;
    GPtrArray* list = sfe_devices_get_list(sfe_devices_get_default());
    for (guint i = 0; list != NULL && i < list->len; i++) {
        const SfeDevice* d = g_ptr_array_index(list, i);
        bool mounted = d->mount_path != NULL;
        if ((mount && mounted) || (!mount && !eject && !mounted) || (eject && !d->removable))
            continue;
        add(out, d->name, NULL, mounted ? "mounted" : "device", 0);
    }
}

static void complete_unbookmark(Ctx* c, int argi, char** argv, const char* query, const char** rank, GPtrArray* out)
{
    (void)c, (void)argv, (void)query, (void)rank;
    if (argi == 1)
        add_bookmarks(out, true, 0);
}

static void complete_split(Ctx* c, int argi, char** argv, const char* query, const char** rank, GPtrArray* out)
{
    (void)query, (void)rank;
    if (argi == 1) {
        static const char* const first[] = { "move", "next", NULL };
        static const char* const rest[] = { "vertical", "horizontal", "left", "right", "up", "down", "absolute", "close", NULL };
        add_words(out, first, 0);
        add_words(out, rest, 1);
    } else if (argi == 2 && strcmp(argv[1], "move") == 0) {
        for (size_t i = 0; i < G_N_ELEMENTS(directions); i++) {
            if (sfe_split_neighbor(c->split, c->pane, directions[i].direction) != NULL)
                add(out, directions[i].name, NULL, NULL, 0);
        }
    } else if (argi == 2 && strcmp(argv[1], "absolute") == 0) {
        for (int i = 0; sfe_split_position_name(c->split, i) != NULL; i++)
            add(out, sfe_split_position_name(c->split, i), NULL, NULL, 0);
    }
}

#define WORDS_COMPLETER(fn, ...)                                                                              \
    static void fn(Ctx* c, int argi, char** argv, const char* query, const char** rank, GPtrArray* out)       \
    {                                                                                                         \
        (void)c, (void)argv, (void)query, (void)rank;                                                         \
        static const char* const words[] = { __VA_ARGS__, NULL };                                             \
        if (argi == 1)                                                                                        \
            add_words(out, words, 0);                                                                         \
    }

WORDS_COMPLETER(complete_on_off, "toggle", "on", "off")
WORDS_COMPLETER(complete_view, "grid", "list")
WORDS_COMPLETER(complete_cursor, "down", "up", "first", "last", "previous", "next")
WORDS_COMPLETER(complete_select, "toggle", "all")
WORDS_COMPLETER(complete_visual, "off", "swap")

/* ------------------------------------------------------------------------ */
/* The table                                                                 */
/* ------------------------------------------------------------------------ */

static const Command commands[] = {
    { { "cd", NULL, "[<folder>]", "go to a folder (home without one)" }, complete_dirs, run_cd },
    { { "open", NULL, "[<file>…]", "open files and folders (the selection without any)" }, complete_files, run_open },
    { { "copy", "cp", "<file>… <destination>", "copy into a folder" }, complete_transfer, run_copy },
    { { "move", "mv", "<file>… <destination>", "move into a folder" }, complete_transfer, run_move },
    { { "symlink", "ln", "<target>… <folder or link name>", "make symbolic links" }, complete_transfer, run_symlink },
    { { "rename", NULL, "[<file> <new name>]", "rename (the dialog without arguments)" }, complete_rename, run_rename },
    { { "mass_rename", NULL, "[<file>…]", "rename with a pattern" }, complete_files, run_mass_rename },
    { { "new_file", "touch", "[<name>…]", "create empty files (the dialog without a name)" }, NULL, run_new_file },
    { { "new_directory", "mkdir", "[<name>…]", "create folders (the dialog without a name)" }, NULL, run_new_directory },
    { { "trash", NULL, "[<file>…]", "move to the trash" }, complete_files, run_trash },
    { { "delete", NULL, "[<file>…]", "delete permanently (asks)" }, complete_files, run_delete },
    { { "copy_clipboard", "yank", "[<file>…]", "copy to the clipboard" }, complete_files, run_copy_clipboard },
    { { "cut", NULL, "[<file>…]", "cut to the clipboard" }, complete_files, run_cut },
    { { "mount", NULL, "<device>", "mount a drive and open it" }, complete_devices, run_mount },
    { { "unmount", NULL, "<device>", "unmount a drive" }, complete_devices, run_unmount },
    { { "eject", NULL, "<device>", "unmount and power off a removable drive" }, complete_devices, run_eject },
    { { "split", NULL, "<sub-command>", "move|next|vertical|horizontal|left|right|up|down|absolute|close" }, complete_split, run_split },
    { { "bookmark", NULL, "[<folder>]", "bookmark a folder (this one without an argument)" }, complete_dirs, run_bookmark },
    { { "unbookmark", NULL, "[<bookmark>]", "remove a bookmark" }, complete_unbookmark, run_unbookmark },
    { { "hidden", NULL, "on|off|toggle", "show or hide hidden files" }, complete_on_off, run_hidden },
    { { "view", NULL, "grid|list", "the active view's layout" }, complete_view, run_view },
    { { "q", "quit", "", "close the active view (the window if it is the last)" }, NULL, run_q },
    { { "cursor", NULL, "down|up|first|last|previous|next", "move the cursor" }, complete_cursor, run_cursor },
    { { "select", NULL, "toggle|all", "toggle the item under the cursor / select all" }, complete_select, run_select },
    { { "visual", NULL, "[off|swap]", "visual mode: start it, end / start a chunk; leave it; jump to the chunk's other end" }, complete_visual, run_visual },
    { { "command_line", NULL, "", "open the command line" }, NULL, run_command_line },
};

static SfeCommandInfo infos[G_N_ELEMENTS(commands)];

const SfeCommandInfo* sfe_commands_info(size_t* n)
{
    for (size_t i = 0; i < G_N_ELEMENTS(commands); i++)
        infos[i] = commands[i].info;
    *n = G_N_ELEMENTS(commands);
    return infos;
}

static bool stays_visual(const Command* cmd)
{
    static const char* const keep[] = { "cursor", "select", "visual", "split" };
    for (size_t i = 0; cmd != NULL && i < G_N_ELEMENTS(keep); i++) {
        if (strcmp(cmd->info.name, keep[i]) == 0)
            return true;
    }
    return false;
}

static const Command* find_command(const char* name)
{
    for (size_t i = 0; i < G_N_ELEMENTS(commands); i++) {
        if (strcmp(commands[i].info.name, name) == 0 || g_strcmp0(commands[i].info.alias, name) == 0)
            return &commands[i];
    }
    return NULL;
}

static Ctx context(SfeWindow* window)
{
    Ctx c = { .window = window, .split = sfe_window_get_split(window) };
    c.pane = sfe_window_get_active_pane(window);
    c.dir = sfe_pane_get_dir(c.pane);
    return c;
}

bool sfe_command_run(SfeWindow* window, const char* line, GError** error)
{
    g_auto(GStrv) argv = sfe_cmdline_split(line, error);
    if (argv == NULL)
        return false;
    int argc = (int)g_strv_length(argv);
    if (argc == 0)
        return true;

    Ctx c = context(window);
    const Command* cmd = find_command(argv[0]);
    g_autoptr(SfePane) pane = g_object_ref(c.pane); /* the command may close it */
    bool ok;
    if (cmd != NULL) {
        ok = cmd->run(&c, argc, argv, error);
    } else if (find_action(argv[0]) != NULL) {
        if (argc > 1)
            return fail(error, "%s takes no arguments", argv[0]);
        ok = run_action(&c, argv[0], NULL, error);
    } else {
        return fail(error, "unknown command: %s", argv[0]);
    }
    /* Visual mode is for building a selection; any command that acts on it
     * (yank, trash, :copy ...) ends it, keeping the selection. */
    if (ok && !stays_visual(cmd) && sfe_window_get_active_pane(window) == pane)
        sfe_pane_visual_end(pane);
    return ok;
}

/* Command names: the ones above, then the actions not covered by them. */
static void add_command_names(GPtrArray* out)
{
    for (size_t i = 0; i < G_N_ELEMENTS(commands); i++) {
        const SfeCommandInfo* info = &commands[i].info;
        add(out, info->name, NULL, info->usage[0] ? info->usage : NULL, 0);
    }
    size_t n = 0;
    const SfeAction* table = sfe_actions_get(&n);
    for (size_t i = 0; i < n; i++) {
        g_autofree char* name = g_strdup(table[i].name);
        g_strdelimit(name, "-", '_');
        if (find_command(name) == NULL && strcmp(name, "copy") != 0) /* "copy" is the clipboard one */
            add(out, name, NULL, NULL, 0);
    }
    for (size_t i = 0; i < G_N_ELEMENTS(commands); i++) {
        if (commands[i].info.alias != NULL)
            add(out, commands[i].info.alias, NULL, commands[i].info.name, 1);
    }
}

GPtrArray* sfe_command_complete(SfeWindow* window, const char* line, int cursor, int* start, int* end)
{
    g_autoptr(GPtrArray) args = sfe_cmdline_tokenize(line);
    GPtrArray* out = g_ptr_array_new_with_free_func(sfe_candidate_free);
    g_autoptr(GPtrArray) before = g_ptr_array_new();
    const char* query = "";
    int argi = 0;

    /* The argument the cursor is in (or a new one at the cursor). */
    *start = *end = cursor;
    for (guint i = 0; i < args->len; i++) {
        SfeCmdArg* a = g_ptr_array_index(args, i);
        if (cursor >= a->start && cursor <= a->end) {
            *start = a->start;
            *end = a->end;
            query = a->value;
            break;
        }
        if (a->end < cursor) {
            g_ptr_array_add(before, a->value);
            argi++;
        }
    }
    g_ptr_array_add(before, NULL);
    char** argv = (char**)before->pdata;
    const char* rank = query;

    if (argi == 0) {
        add_command_names(out);
    } else {
        const Command* cmd = find_command(argv[0]);
        if (cmd != NULL && cmd->complete != NULL) {
            Ctx c = context(window);
            cmd->complete(&c, argi, argv, query, &rank, out);
        }
    }
    g_autofree char* rank_copy = g_strdup(rank); /* `rank` may point into the arguments */
    return sfe_cmdline_rank(out, rank_copy);
}
