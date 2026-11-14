#include "fileops.h"

#include "archive.h"
#include "util.h"

#include <string.h>

#define REPORT_INTERVAL_US (100 * 1000)
#define NOFOLLOW G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS

struct _SfeFileOp {
    GObject parent;

    SfeOpKind kind;
    GList* sources; /* GFile* */
    GFile* dest_dir;
    GCancellable* cancel;
    GMainContext* context; /* where signals are emitted */

    GMutex lock;
    GCond cond;

    /* Pending question (guarded by lock). */
    bool waiting;
    bool answered;
    SfeOpAnswer answer;
    bool answer_for_all;
    SfeOpQuestion q_kind;
    GFile* q_source;
    GFile* q_dest;
    char* q_error;

    /* Progress (guarded by lock). */
    char* status;
    double fraction;
    gint64 last_report;
    bool report_pending;

    /* Worker thread only. */
    char* current; /* name of the item being processed */
    int apply_to_all[2]; /* per SfeOpQuestion: an SfeOpAnswer, or -1 */
    guint64 total_bytes, done_bytes;
    guint total_items, done_items;
};

G_DEFINE_TYPE(SfeFileOp, sfe_file_op, G_TYPE_OBJECT)

enum {
    SIGNAL_PROGRESS,
    SIGNAL_QUESTION,
    SIGNAL_FINISHED,
    N_SIGNALS,
};
static guint signals[N_SIGNALS];

/* ------------------------------------------------------------------------ */
/* Names                                                                     */
/* ------------------------------------------------------------------------ */

char* sfe_numbered_name(const char* name, bool is_dir, int n)
{
    g_autofree char* stem = NULL;
    g_autofree char* ext = NULL;
    sfe_split_extension(name, is_dir, &stem, &ext);

    /* "a (2)" becomes "a (3)", not "a (2) (2)". */
    size_t len = strlen(stem);
    if (len > 3 && stem[len - 1] == ')') {
        size_t i = len - 2;
        while (i > 0 && g_ascii_isdigit(stem[i]))
            i--;
        if (i + 1 < len - 1 && i >= 1 && stem[i] == '(' && stem[i - 1] == ' ')
            stem[i - 1] = '\0';
    }
    return g_strdup_printf("%s (%d)%s", stem, n, ext);
}

/* A sibling of `dest` that does not exist yet. */
static GFile* unused_name(GFile* dest, bool is_dir)
{
    g_autoptr(GFile) parent = g_file_get_parent(dest);
    g_autofree char* base = g_file_get_basename(dest);

    for (int n = 2;; n++) {
        g_autofree char* name = sfe_numbered_name(base, is_dir, n);
        GFile* candidate = g_file_get_child(parent, name);
        if (!g_file_query_exists(candidate, NULL))
            return candidate;
        g_object_unref(candidate);
    }
}

/* ------------------------------------------------------------------------ */
/* Talking to the main thread                                                */
/* ------------------------------------------------------------------------ */

static gboolean emit_progress(gpointer data)
{
    SfeFileOp* op = data;
    g_mutex_lock(&op->lock);
    op->report_pending = false;
    g_mutex_unlock(&op->lock);
    g_signal_emit(op, signals[SIGNAL_PROGRESS], 0);
    return G_SOURCE_REMOVE;
}

static void invoke(SfeFileOp* op, GSourceFunc func)
{
    g_main_context_invoke_full(op->context, G_PRIORITY_DEFAULT, func, g_object_ref(op), g_object_unref);
}

/* Updates the status line; signals at most every REPORT_INTERVAL_US. */
static void report(SfeFileOp* op, const char* verb, GFile* file, guint64 current_bytes)
{
    if (file != NULL) {
        g_free(op->current);
        op->current = g_file_get_basename(file);
    }
    const char* name = op->current;
    gint64 now = g_get_monotonic_time();
    double fraction = op->total_bytes > 0
        ? (double)(op->done_bytes + current_bytes) / (double)op->total_bytes
        : op->total_items > 0 ? (double)op->done_items / op->total_items
                              : 0.0;
    bool emit = false;

    g_mutex_lock(&op->lock);
    if (now - op->last_report >= REPORT_INTERVAL_US && !op->report_pending) {
        g_free(op->status);
        op->status = name == NULL       ? g_strdup(verb)
            : op->total_items <= 1 ? g_strdup_printf("%s “%s”", verb, name)
                                   : g_strdup_printf("%s “%s” — %u of %u", verb, name,
                                       MIN(op->done_items + 1, op->total_items), op->total_items);
        op->fraction = CLAMP(fraction, 0.0, 1.0);
        op->last_report = now;
        op->report_pending = emit = true;
    }
    g_mutex_unlock(&op->lock);
    if (emit)
        invoke(op, emit_progress);
}

static gboolean emit_question(gpointer data)
{
    SfeFileOp* op = data;

    if (!g_signal_has_handler_pending(op, signals[SIGNAL_QUESTION], 0, TRUE)) {
        sfe_file_op_answer(op, SFE_ANSWER_SKIP, false); /* nobody to ask */
        return G_SOURCE_REMOVE;
    }
    g_mutex_lock(&op->lock);
    SfeOpQuestion kind = op->q_kind;
    g_autoptr(GFile) source = op->q_source ? g_object_ref(op->q_source) : NULL;
    g_autoptr(GFile) dest = op->q_dest ? g_object_ref(op->q_dest) : NULL;
    g_autofree char* error = g_strdup(op->q_error);
    g_mutex_unlock(&op->lock);

    g_signal_emit(op, signals[SIGNAL_QUESTION], 0, kind, source, dest, error);
    return G_SOURCE_REMOVE;
}

/* Asks the main thread and blocks the worker until it answers. */
static SfeOpAnswer ask(SfeFileOp* op, SfeOpQuestion kind, GFile* source, GFile* dest, const char* error)
{
    if (op->apply_to_all[kind] >= 0)
        return op->apply_to_all[kind];

    g_mutex_lock(&op->lock);
    op->q_kind = kind;
    g_set_object(&op->q_source, source);
    g_set_object(&op->q_dest, dest);
    g_free(op->q_error);
    op->q_error = g_strdup(error);
    op->waiting = true;
    op->answered = g_cancellable_is_cancelled(op->cancel);
    op->answer = SFE_ANSWER_CANCEL;
    g_mutex_unlock(&op->lock);

    invoke(op, emit_question);

    g_mutex_lock(&op->lock);
    while (!op->answered)
        g_cond_wait(&op->cond, &op->lock);
    SfeOpAnswer answer = op->answer;
    if (op->answer_for_all)
        op->apply_to_all[kind] = answer;
    op->waiting = false;
    g_mutex_unlock(&op->lock);

    if (answer == SFE_ANSWER_CANCEL)
        g_cancellable_cancel(op->cancel);
    return answer;
}

/* Asks what to do about a failed step. Returns true to retry it. */
static bool retry_after(SfeFileOp* op, GFile* file, GError* error)
{
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return false;
    return ask(op, SFE_QUESTION_ERROR, file, NULL, error->message) == SFE_ANSWER_RETRY;
}

static bool cancelled(SfeFileOp* op)
{
    return g_cancellable_is_cancelled(op->cancel);
}

/* ------------------------------------------------------------------------ */
/* Recursive building blocks                                                 */
/* ------------------------------------------------------------------------ */

static GFileType file_type(GFile* file)
{
    return g_file_query_file_type(file, NOFOLLOW, NULL);
}

/* Counts items and bytes below `file` for the progress bar. */
static void scan(SfeFileOp* op, GFile* file)
{
    g_autoptr(GFileInfo) info = g_file_query_info(file, "standard::type,standard::size", NOFOLLOW, op->cancel, NULL);
    if (info == NULL)
        return;

    op->total_items++;
    if (g_file_info_get_file_type(info) != G_FILE_TYPE_DIRECTORY) {
        op->total_bytes += g_file_info_get_size(info);
        return;
    }
    g_autoptr(GFileEnumerator) en = g_file_enumerate_children(file, "standard::name", NOFOLLOW, op->cancel, NULL);
    GFileInfo* child;
    while (en != NULL && (child = g_file_enumerator_next_file(en, op->cancel, NULL)) != NULL) {
        g_autoptr(GFile) f = g_file_get_child(file, g_file_info_get_name(child));
        scan(op, f);
        g_object_unref(child);
    }
}

/* The children of `dir` as GFile*, or NULL if it cannot be read (skipped). */
static GPtrArray* list_children(SfeFileOp* op, GFile* dir)
{
    for (;;) {
        g_autoptr(GError) error = NULL;
        g_autoptr(GFileEnumerator) en = g_file_enumerate_children(dir, "standard::name", NOFOLLOW, op->cancel, &error);
        GPtrArray* children = g_ptr_array_new_with_free_func(g_object_unref);
        GFileInfo* info;

        while (en != NULL && (info = g_file_enumerator_next_file(en, op->cancel, &error)) != NULL) {
            g_ptr_array_add(children, g_file_get_child(dir, g_file_info_get_name(info)));
            g_object_unref(info);
        }
        if (error == NULL)
            return children;
        g_ptr_array_unref(children);
        if (!retry_after(op, dir, error))
            return NULL;
    }
}

static bool delete_recursive(SfeFileOp* op, GFile* file, bool count)
{
    bool ok = true;

    if (cancelled(op))
        return false;
    if (file_type(file) == G_FILE_TYPE_DIRECTORY) {
        g_autoptr(GPtrArray) children = list_children(op, file);
        if (children == NULL)
            return false;
        for (guint i = 0; i < children->len; i++)
            ok &= delete_recursive(op, g_ptr_array_index(children, i), count);
        if (!ok)
            return false; /* something inside was kept: keep the folder too */
    }
    if (count)
        report(op, "Deleting", file, 0);
    for (;;) {
        g_autoptr(GError) error = NULL;
        if (g_file_delete(file, op->cancel, &error))
            break;
        if (!retry_after(op, file, error))
            return false;
    }
    if (count)
        op->done_items++;
    return true;
}

typedef enum {
    TARGET_SKIP,
    TARGET_NEW, /* the (possibly renamed) target does not exist */
    TARGET_MERGE, /* both are directories: copy/move into the existing one */
} TargetMode;

/* Decides where `src` goes when `*dest` may already exist. May replace
 * *dest with a renamed sibling, or delete the existing one ("replace"). */
static TargetMode resolve_target(SfeFileOp* op, GFile* src, GFile** dest)
{
    GFileType src_type = file_type(src);
    bool src_dir = src_type == G_FILE_TYPE_DIRECTORY;

    if (!g_file_query_exists(*dest, NULL))
        return TARGET_NEW;
    if (g_file_equal(src, *dest)) { /* copy into its own folder: keep both */
        GFile* renamed = unused_name(*dest, src_dir);
        g_object_unref(*dest);
        *dest = renamed;
        return TARGET_NEW;
    }
    if (src_dir && file_type(*dest) == G_FILE_TYPE_DIRECTORY)
        return TARGET_MERGE;

    switch (ask(op, SFE_QUESTION_CONFLICT, src, *dest, NULL)) {
    case SFE_ANSWER_REPLACE:
        return delete_recursive(op, *dest, false) ? TARGET_NEW : TARGET_SKIP;
    case SFE_ANSWER_RENAME: {
        GFile* renamed = unused_name(*dest, src_dir);
        g_object_unref(*dest);
        *dest = renamed;
        return TARGET_NEW;
    }
    default:
        return TARGET_SKIP;
    }
}

/* Refuses to put a folder inside itself. */
static bool inside_itself(SfeFileOp* op, GFile* src, GFile* dest)
{
    if (!g_file_has_prefix(dest, src))
        return false;
    ask(op, SFE_QUESTION_ERROR, src, NULL, "A folder cannot be copied or moved into itself.");
    return true;
}

static void on_copy_progress(goffset current, goffset total, gpointer data)
{
    (void)total;
    SfeFileOp* op = data;
    report(op, op->kind == SFE_OP_MOVE ? "Moving" : "Copying", NULL, current);
}

static bool copy_item(SfeFileOp* op, GFile* src, GFile* dest_in)
{
    g_autoptr(GFile) dest = g_object_ref(dest_in);
    const char* verb = op->kind == SFE_OP_MOVE ? "Moving" : "Copying";

    if (cancelled(op) || inside_itself(op, src, dest))
        return false;
    TargetMode mode = resolve_target(op, src, &dest);
    if (mode == TARGET_SKIP)
        return false;

    report(op, verb, src, 0);
    if (file_type(src) != G_FILE_TYPE_DIRECTORY) {
        g_autoptr(GFileInfo) info = g_file_query_info(src, "standard::size", NOFOLLOW, NULL, NULL);
        for (;;) {
            g_autoptr(GError) error = NULL;
            if (g_file_copy(src, dest, G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_ALL_METADATA,
                    op->cancel, on_copy_progress, op, &error))
                break;
            if (!retry_after(op, src, error))
                return false;
        }
        op->done_bytes += info ? g_file_info_get_size(info) : 0;
        op->done_items++;
        return true;
    }

    if (mode == TARGET_NEW) {
        for (;;) {
            g_autoptr(GError) error = NULL;
            if (g_file_make_directory(dest, op->cancel, &error))
                break;
            if (!retry_after(op, dest, error))
                return false;
        }
    }
    g_autoptr(GPtrArray) children = list_children(op, src);
    if (children == NULL)
        return false;
    bool ok = true;
    for (guint i = 0; i < children->len; i++) {
        GFile* child = g_ptr_array_index(children, i);
        g_autofree char* name = g_file_get_basename(child);
        g_autoptr(GFile) target = g_file_get_child(dest, name);
        ok &= copy_item(op, child, target);
    }
    /* Permissions and times last, so a read-only folder can be filled. */
    g_file_copy_attributes(src, dest, G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_ALL_METADATA, NULL, NULL);
    op->done_items++;
    return ok;
}

static bool move_item(SfeFileOp* op, GFile* src, GFile* dest_in)
{
    g_autoptr(GFile) dest = g_object_ref(dest_in);

    if (cancelled(op) || g_file_equal(src, dest)) /* already there */
        return !cancelled(op);
    if (inside_itself(op, src, dest))
        return false;
    TargetMode mode = resolve_target(op, src, &dest);
    if (mode == TARGET_SKIP)
        return false;

    if (mode == TARGET_MERGE) { /* move the contents, then drop the empty folder */
        g_autoptr(GPtrArray) children = list_children(op, src);
        bool ok = children != NULL;
        for (guint i = 0; ok && i < children->len; i++) {
            GFile* child = g_ptr_array_index(children, i);
            g_autofree char* name = g_file_get_basename(child);
            g_autoptr(GFile) target = g_file_get_child(dest, name);
            ok &= move_item(op, child, target);
        }
        if (ok)
            g_file_delete(src, NULL, NULL);
        return ok;
    }

    report(op, "Moving", src, 0);
    for (;;) {
        g_autoptr(GError) error = NULL;
        /* A rename when possible; across filesystems GIO reports
         * NOT_SUPPORTED / WOULD_RECURSE and we copy, then delete. */
        if (g_file_move(src, dest, G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_NO_FALLBACK_FOR_MOVE | G_FILE_COPY_ALL_METADATA,
                op->cancel, NULL, NULL, &error)) {
            op->done_items++;
            return true;
        }
        if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED)
            || g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE))
        {
            scan(op, src); /* now there are bytes to show progress for */
            return copy_item(op, src, dest) && delete_recursive(op, src, false);
        }
        if (!retry_after(op, src, error))
            return false;
    }
}

static void trash_item(SfeFileOp* op, GFile* file)
{
    report(op, "Moving to the trash", file, 0);
    for (;;) {
        g_autoptr(GError) error = NULL;
        if (g_file_trash(file, op->cancel, &error))
            break;
        if (!retry_after(op, file, error))
            return;
    }
    op->done_items++;
}

/* Removes a folder tree without asking (cleanup after a failed extraction). */
static void remove_tree(GFile* file)
{
    if (g_file_query_file_type(file, NOFOLLOW, NULL) == G_FILE_TYPE_DIRECTORY) {
        g_autoptr(GFileEnumerator) en = g_file_enumerate_children(file, "standard::name", NOFOLLOW, NULL, NULL);
        GFileInfo* info;
        while (en != NULL && (info = g_file_enumerator_next_file(en, NULL, NULL)) != NULL) {
            g_autoptr(GFile) child = g_file_get_child(file, g_file_info_get_name(info));
            remove_tree(child);
            g_object_unref(info);
        }
    }
    g_file_delete(file, NULL, NULL);
}

/* Runs an archive tool in `cwd` (or here); on failure returns false with
 * the tool's own message for the user. */
static bool run_tool(SfeFileOp* op, char** argv, const char* cwd, char** message)
{
    g_autoptr(GError) error = NULL;
    g_autofree char* err_text = NULL;
    g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(
        G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_PIPE);

    if (cwd != NULL)
        g_subprocess_launcher_set_cwd(launcher, cwd);
    g_autoptr(GSubprocess) proc = g_subprocess_launcher_spawnv(launcher, (const char* const*)argv, &error);
    if (proc == NULL) {
        *message = g_strdup(error->message);
        return false;
    }
    bool ok = g_subprocess_communicate_utf8(proc, NULL, op->cancel, NULL, &err_text, &error)
        && g_subprocess_get_successful(proc);
    if (cancelled(op)) {
        g_subprocess_force_exit(proc);
        g_subprocess_wait(proc, NULL, NULL); /* gone before its output is removed */
        return false;
    }
    if (!ok) {
        g_strstrip(err_text ? err_text : (err_text = g_strdup("")));
        *message = g_strdup(*err_text ? err_text : error ? error->message : "The archive tool failed.");
    }
    return ok;
}

static bool run_extractor(SfeFileOp* op, const char* archive, const char* dest, char** message)
{
    g_auto(GStrv) argv = sfe_archive_extract_argv(archive, dest, g_find_program_in_path);
    if (argv == NULL) {
        *message = g_strdup("No program that can extract this archive is installed "
                            "(bsdtar, tar, unzip, 7z or unrar).");
        return false;
    }
    return run_tool(op, argv, NULL, message);
}

/* "photos.tar.gz" -> a new folder "photos" (or "photos (2)") in dest_dir. */
static void extract_item(SfeFileOp* op, GFile* archive)
{
    g_autofree char* archive_path = g_file_get_path(archive);
    g_autofree char* base = g_file_get_basename(archive);
    g_autofree char* folder_name = sfe_archive_folder_name(base);
    g_autoptr(GFile) folder = g_file_get_child(op->dest_dir, folder_name);

    if (g_file_query_exists(folder, NULL)) {
        GFile* renamed = unused_name(folder, true);
        g_object_unref(folder);
        folder = renamed;
    }
    report(op, "Extracting", archive, 0);

    for (;;) {
        g_autoptr(GError) error = NULL;
        g_autofree char* dest = g_file_get_path(folder);
        g_autofree char* message = NULL;

        if (!g_file_make_directory(folder, op->cancel, &error)) {
            if (!retry_after(op, archive, error))
                return;
            continue;
        }
        if (archive_path != NULL && run_extractor(op, archive_path, dest, &message)) {
            op->done_items++;
            return;
        }
        remove_tree(folder); /* no half-extracted folders */
        if (cancelled(op))
            return;
        g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_FAILED,
            message ? message : "Only local archives can be extracted.");
        if (!retry_after(op, archive, error))
            return;
    }
}

/* All the sources (in one folder) -> one new archive, dest_dir. An
 * existing archive is never added to: "photos (2).zip" instead. */
static void compress_all(SfeFileOp* op)
{
    g_autoptr(GFile) archive = g_object_ref(op->dest_dir);
    g_autoptr(GFile) parent = op->sources ? g_file_get_parent(op->sources->data) : NULL;
    g_autofree char* cwd = parent ? g_file_get_path(parent) : NULL;
    g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);

    for (GList* l = op->sources; l != NULL; l = l->next)
        g_ptr_array_add(names, g_file_get_basename(l->data));
    g_ptr_array_add(names, NULL);

    if (g_file_query_exists(archive, NULL)) {
        GFile* renamed = unused_name(archive, false);
        g_object_unref(archive);
        archive = renamed;
    }
    report(op, "Creating", archive, 0);

    for (;;) {
        g_autoptr(GError) error = NULL;
        g_autofree char* path = g_file_get_path(archive);
        g_autofree char* message = NULL;
        g_auto(GStrv) argv = path ? sfe_archive_compress_argv(path, (char* const*)names->pdata, g_find_program_in_path) : NULL;

        if (argv == NULL)
            message = g_strdup(path == NULL || cwd == NULL ? "Only local files can be compressed."
                                                           : "No program that can create this kind of archive is installed "
                                                             "(bsdtar, tar, zip or 7z).");
        else if (cwd != NULL && run_tool(op, argv, cwd, &message)) {
            op->done_items = op->total_items;
            return;
        }
        g_file_delete(archive, NULL, NULL); /* no half-written archives */
        if (cancelled(op))
            return;
        g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_FAILED, message);
        if (!retry_after(op, op->sources->data, error))
            return;
    }
}

/* ------------------------------------------------------------------------ */
/* The job                                                                   */
/* ------------------------------------------------------------------------ */

static void run_job(GTask* task, gpointer source, gpointer data, GCancellable* cancel)
{
    (void)data, (void)cancel;
    SfeFileOp* op = SFE_FILE_OP(source);

    report(op, "Preparing…", NULL, 0);
    if (op->kind == SFE_OP_COPY || op->kind == SFE_OP_DELETE) {
        for (GList* l = op->sources; l != NULL; l = l->next)
            scan(op, l->data);
    } else {
        op->total_items = op->kind == SFE_OP_COMPRESS ? 1 : g_list_length(op->sources);
    }

    if (op->kind == SFE_OP_COMPRESS)
        compress_all(op);
    for (GList* l = op->sources; op->kind != SFE_OP_COMPRESS && l != NULL && !cancelled(op); l = l->next) {
        GFile* src = l->data;
        g_autofree char* name = g_file_get_basename(src);
        g_autoptr(GFile) dest = op->dest_dir ? g_file_get_child(op->dest_dir, name) : NULL;

        switch (op->kind) {
        case SFE_OP_COPY: copy_item(op, src, dest); break;
        case SFE_OP_MOVE: move_item(op, src, dest); break;
        case SFE_OP_TRASH: trash_item(op, src); break;
        case SFE_OP_DELETE: delete_recursive(op, src, true); break;
        case SFE_OP_EXTRACT: extract_item(op, src); break;
        case SFE_OP_COMPRESS: break; /* above, in one go */
        }
    }

    if (cancelled(op))
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
    else
        g_task_return_boolean(task, TRUE);
}

static void on_job_done(GObject* source, GAsyncResult* res, gpointer data)
{
    (void)data;
    SfeFileOp* op = SFE_FILE_OP(source);
    g_autoptr(GError) error = NULL;

    g_task_propagate_boolean(G_TASK(res), &error);
    g_mutex_lock(&op->lock);
    op->fraction = 1.0;
    g_mutex_unlock(&op->lock);
    g_signal_emit(op, signals[SIGNAL_FINISHED], 0, error);
    g_object_unref(op); /* the reference taken in start() */
}

void sfe_file_op_start(SfeFileOp* op)
{
    g_object_ref(op); /* alive until finished */
    g_autoptr(GTask) task = g_task_new(op, op->cancel, on_job_done, NULL);
    g_task_run_in_thread(task, run_job);
}

void sfe_file_op_cancel(SfeFileOp* op)
{
    g_cancellable_cancel(op->cancel);
    g_mutex_lock(&op->lock);
    if (op->waiting && !op->answered) {
        op->answer = SFE_ANSWER_CANCEL;
        op->answered = true;
        g_cond_signal(&op->cond);
    }
    g_mutex_unlock(&op->lock);
}

void sfe_file_op_answer(SfeFileOp* op, SfeOpAnswer answer, bool apply_to_all)
{
    g_mutex_lock(&op->lock);
    if (op->waiting && !op->answered) {
        op->answer = answer;
        op->answer_for_all = apply_to_all;
        op->answered = true;
        g_cond_signal(&op->cond);
    }
    g_mutex_unlock(&op->lock);
}

SfeOpKind sfe_file_op_get_kind(SfeFileOp* op)
{
    return op->kind;
}

char* sfe_file_op_get_status(SfeFileOp* op)
{
    g_mutex_lock(&op->lock);
    char* status = g_strdup(op->status ? op->status : "");
    g_mutex_unlock(&op->lock);
    return status;
}

double sfe_file_op_get_fraction(SfeFileOp* op)
{
    g_mutex_lock(&op->lock);
    double fraction = op->fraction;
    g_mutex_unlock(&op->lock);
    return fraction;
}

/* ------------------------------------------------------------------------ */
/* GObject                                                                   */
/* ------------------------------------------------------------------------ */

static void sfe_file_op_finalize(GObject* object)
{
    SfeFileOp* op = SFE_FILE_OP(object);

    g_list_free_full(op->sources, g_object_unref);
    g_clear_object(&op->dest_dir);
    g_clear_object(&op->cancel);
    g_main_context_unref(op->context);
    g_clear_object(&op->q_source);
    g_clear_object(&op->q_dest);
    g_free(op->q_error);
    g_free(op->status);
    g_free(op->current);
    g_mutex_clear(&op->lock);
    g_cond_clear(&op->cond);
    G_OBJECT_CLASS(sfe_file_op_parent_class)->finalize(object);
}

static void sfe_file_op_class_init(SfeFileOpClass* klass)
{
    G_OBJECT_CLASS(klass)->finalize = sfe_file_op_finalize;

    signals[SIGNAL_PROGRESS] = g_signal_new("progress", G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    signals[SIGNAL_QUESTION] = g_signal_new("question", G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 4,
        G_TYPE_INT, G_TYPE_FILE, G_TYPE_FILE, G_TYPE_STRING);
    signals[SIGNAL_FINISHED] = g_signal_new("finished", G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_ERROR);
}

static void sfe_file_op_init(SfeFileOp* op)
{
    g_mutex_init(&op->lock);
    g_cond_init(&op->cond);
    op->cancel = g_cancellable_new();
    op->context = g_main_context_ref_thread_default();
    op->apply_to_all[0] = op->apply_to_all[1] = -1;
}

static gpointer ref_copy(gconstpointer object, gpointer data)
{
    (void)data;
    return g_object_ref((gpointer)object);
}

SfeFileOp* sfe_file_op_new(SfeOpKind kind, GList* sources, GFile* dest_dir)
{
    SfeFileOp* op = g_object_new(SFE_TYPE_FILE_OP, NULL);

    op->kind = kind;
    op->sources = g_list_copy_deep(sources, ref_copy, NULL);
    op->dest_dir = dest_dir ? g_object_ref(dest_dir) : NULL;
    return op;
}
