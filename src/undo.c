#include "undo.h"

#include "rename.h"
#include "trash.h"
#include "util.h"
#include <string.h>

static GPtrArray* history; /* SfeUndoEntry*, oldest first */
static guint next_id = 1;

static void pair_free(gpointer p)
{
    SfeUndoPair* pair = p;
    g_clear_object(&pair->before);
    g_clear_object(&pair->after);
    g_free(pair);
}

static void entry_free(gpointer p)
{
    SfeUndoEntry* e = p;
    g_ptr_array_unref(e->pairs);
    g_date_time_unref(e->time);
    g_free(e);
}

GPtrArray* sfe_undo_entries(void)
{
    if (history == NULL)
        history = g_ptr_array_new_with_free_func(entry_free);
    return history;
}

SfeUndoPair* sfe_undo_pair_new(GFile* before, GFile* after)
{
    SfeUndoPair* pair = g_new0(SfeUndoPair, 1);
    pair->before = before ? g_object_ref(before) : NULL;
    pair->after = after ? g_object_ref(after) : NULL;
    return pair;
}

SfeUndoEntry* sfe_undo_record(SfeUndoKind kind, GPtrArray* pairs)
{
    if (pairs == NULL || pairs->len == 0) {
        if (pairs != NULL)
            g_ptr_array_unref(pairs);
        return NULL;
    }
    g_ptr_array_set_free_func(pairs, pair_free);
    SfeUndoEntry* e = g_new0(SfeUndoEntry, 1);
    e->id = next_id++;
    e->kind = kind;
    e->pairs = pairs;
    e->time = g_date_time_new_now_local();

    GPtrArray* all = sfe_undo_entries();
    g_ptr_array_add(all, e);
    if (all->len > SFE_UNDO_MAX)
        g_ptr_array_remove_index(all, 0);
    return e;
}

SfeUndoEntry* sfe_undo_record_renames(GFile** files, char** names, int n)
{
    GPtrArray* pairs = g_ptr_array_new_with_free_func(pair_free);
    for (int i = 0; i < n; i++) {
        g_autofree char* old = g_file_get_basename(files[i]);
        if (g_strcmp0(old, names[i]) == 0)
            continue; /* unchanged */
        g_autoptr(GFile) parent = g_file_get_parent(files[i]);
        g_autoptr(GFile) renamed = parent ? g_file_get_child(parent, names[i]) : NULL;
        if (renamed != NULL)
            g_ptr_array_add(pairs, sfe_undo_pair_new(files[i], renamed));
    }
    return sfe_undo_record(SFE_UNDO_RENAME, pairs);
}

SfeUndoEntry* sfe_undo_record_op(SfeFileOp* op)
{
    SfeUndoKind kind;
    switch (sfe_file_op_get_kind(op)) {
    case SFE_OP_COPY: kind = SFE_UNDO_COPY; break;
    case SFE_OP_MOVE: kind = SFE_UNDO_MOVE; break;
    case SFE_OP_TRASH: kind = SFE_UNDO_TRASH; break;
    case SFE_OP_EXTRACT: kind = SFE_UNDO_EXTRACT; break;
    case SFE_OP_COMPRESS: kind = SFE_UNDO_COMPRESS; break;
    default: return NULL; /* deleted for good */
    }
    GPtrArray* results = sfe_file_op_get_results(op);
    GPtrArray* pairs = g_ptr_array_new_with_free_func(pair_free);
    for (guint i = 0; i < results->len; i++) {
        const SfeOpResult* r = g_ptr_array_index(results, i);
        g_ptr_array_add(pairs, sfe_undo_pair_new(r->source, r->result));
    }
    return sfe_undo_record(kind, pairs);
}

SfeUndoEntry* sfe_undo_find(guint id)
{
    GPtrArray* all = sfe_undo_entries();
    if (id == 0)
        return all->len > 0 ? g_ptr_array_index(all, all->len - 1) : NULL;
    for (guint i = 0; i < all->len; i++) {
        SfeUndoEntry* e = g_ptr_array_index(all, i);
        if (e->id == id)
            return e;
    }
    return NULL;
}

static void forget(SfeUndoEntry* entry)
{
    g_ptr_array_remove(sfe_undo_entries(), entry);
}

void sfe_undo_clear(void)
{
    g_ptr_array_set_size(sfe_undo_entries(), 0);
}

/* ------------------------------------------------------------------------ */
/* Descriptions                                                              */
/* ------------------------------------------------------------------------ */

static char* display(GFile* file)
{
    if (file == NULL)
        return g_strdup("?");
    g_autofree char* path = g_file_get_path(file);
    return path ? sfe_contract_home(path) : g_file_get_uri(file);
}

static char* name_of(GFile* file)
{
    g_autofree char* base = file ? g_file_get_basename(file) : NULL;
    return base ? g_filename_display_name(base) : g_strdup("?");
}

/* "notes.txt" for one file, "3 items" for more. */
static char* items(const SfeUndoEntry* e, bool after)
{
    if (e->pairs->len != 1)
        return g_strdup_printf("%u items", e->pairs->len);
    const SfeUndoPair* p = g_ptr_array_index(e->pairs, 0);
    return name_of(after ? p->after : p->before);
}

/* The folder the first file ended up in. */
static char* destination(const SfeUndoEntry* e)
{
    const SfeUndoPair* p = g_ptr_array_index(e->pairs, 0);
    g_autoptr(GFile) parent = p->after ? g_file_get_parent(p->after) : NULL;
    return display(parent);
}

char* sfe_undo_describe(const SfeUndoEntry* e)
{
    g_autofree char* what = items(e, false);
    g_autofree char* made = items(e, true);
    g_autofree char* where = destination(e);

    switch (e->kind) {
    case SFE_UNDO_COPY: return g_strdup_printf("copy %s to %s", what, where);
    case SFE_UNDO_MOVE: return g_strdup_printf("move %s to %s", what, where);
    case SFE_UNDO_TRASH: return g_strdup_printf("trash %s", what);
    case SFE_UNDO_RENAME:
        if (e->pairs->len == 1)
            return g_strdup_printf("rename %s → %s", what, made);
        return g_strdup_printf("rename %s", what);
    case SFE_UNDO_CREATE: return g_strdup_printf("create %s", made);
    case SFE_UNDO_LINK: return g_strdup_printf("link %s", made);
    case SFE_UNDO_EXTRACT: return g_strdup_printf("extract %s", what);
    case SFE_UNDO_COMPRESS: return g_strdup_printf("compress into %s", made);
    }
    return g_strdup("?");
}

/* ------------------------------------------------------------------------ */
/* Undoing                                                                   */
/* ------------------------------------------------------------------------ */

/* Whether `file` is there (a link counts even if its target is gone). */
static bool present(GFile* file)
{
    return file != NULL && g_file_query_file_type(file, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL) != G_FILE_TYPE_UNKNOWN;
}

static bool nothing_left(GError** error)
{
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "nothing to undo: the files are gone");
    return false;
}

/* The files made go to the trash. */
static bool undo_by_trashing(SfeUndoEntry* e, SfeFileOp** job, guint* missing, GError** error)
{
    GList* files = NULL;
    for (guint i = 0; i < e->pairs->len; i++) {
        const SfeUndoPair* p = g_ptr_array_index(e->pairs, i);
        if (present(p->after))
            files = g_list_prepend(files, g_object_ref(p->after));
        else
            (*missing)++;
    }
    forget(e);
    if (files == NULL)
        return nothing_left(error);
    files = g_list_reverse(files);
    *job = sfe_file_op_new(SFE_OP_TRASH, files, NULL);
    g_list_free_full(files, g_object_unref);
    return true;
}

/* Every file goes back to where it was. */
static bool undo_move(SfeUndoEntry* e, SfeFileOp** job, guint* missing, GError** error)
{
    GList* from = NULL;
    GList* to = NULL;
    for (guint i = 0; i < e->pairs->len; i++) {
        const SfeUndoPair* p = g_ptr_array_index(e->pairs, i);
        if (present(p->after) && p->before != NULL) {
            from = g_list_prepend(from, g_object_ref(p->after));
            to = g_list_prepend(to, g_object_ref(p->before));
        } else {
            (*missing)++;
        }
    }
    forget(e);
    if (from == NULL)
        return nothing_left(error);
    from = g_list_reverse(from);
    to = g_list_reverse(to);
    *job = sfe_file_op_new(SFE_OP_MOVE, from, NULL);
    sfe_file_op_set_destinations(*job, to);
    g_list_free_full(from, g_object_unref);
    g_list_free_full(to, g_object_unref);
    return true;
}

static bool undo_trash(SfeUndoEntry* e, guint* missing, GError** error)
{
    GError* first = NULL;
    guint restored = 0;
    GList* dirs = sfe_trash_dirs();
    g_autoptr(GHashTable) index = sfe_trash_index(dirs); /* once: not per file */
    g_list_free_full(dirs, g_object_unref);
    for (guint i = 0; i < e->pairs->len; i++) {
        const SfeUndoPair* p = g_ptr_array_index(e->pairs, i);
        g_autofree char* path = p->before ? g_file_get_path(p->before) : NULL;
        GFile* item = path ? g_hash_table_lookup(index, path) : NULL;
        if (item == NULL) {
            (*missing)++;
            continue;
        }
        GError* local = NULL;
        if (sfe_trash_restore(item, &local))
            restored++;
        else if (first == NULL)
            first = local;
        else
            g_error_free(local);
    }
    if (first != NULL && restored == 0) {
        g_propagate_error(error, first);
        return false; /* kept, to try again */
    }
    g_clear_error(&first);
    forget(e);
    return restored > 0 || nothing_left(error);
}

static bool undo_rename(SfeUndoEntry* e, guint* missing, GError** error)
{
    g_autoptr(GPtrArray) files = g_ptr_array_new_with_free_func(g_object_unref);
    g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < e->pairs->len; i++) {
        const SfeUndoPair* p = g_ptr_array_index(e->pairs, i);
        if (!present(p->after) || p->before == NULL) {
            (*missing)++;
            continue;
        }
        g_ptr_array_add(files, g_object_ref(p->after));
        g_ptr_array_add(names, g_file_get_basename(p->before));
    }
    if (files->len == 0) {
        forget(e);
        return nothing_left(error);
    }
    /* sfe_rename_files() handles swaps (a -> b, b -> a) and rolls back on
     * failure, so a failed undo leaves everything as it was. */
    if (!sfe_rename_files((GFile**)files->pdata, (char**)names->pdata, (int)files->len, error))
        return false;
    forget(e);
    return true;
}

bool sfe_undo_apply(SfeUndoEntry* entry, SfeFileOp** job, guint* missing, GError** error)
{
    guint gone = 0;
    bool ok = false;
    *job = NULL;

    switch (entry->kind) {
    case SFE_UNDO_COPY:
    case SFE_UNDO_CREATE:
    case SFE_UNDO_LINK:
    case SFE_UNDO_EXTRACT:
    case SFE_UNDO_COMPRESS: ok = undo_by_trashing(entry, job, &gone, error); break;
    case SFE_UNDO_MOVE: ok = undo_move(entry, job, &gone, error); break;
    case SFE_UNDO_TRASH: ok = undo_trash(entry, &gone, error); break;
    case SFE_UNDO_RENAME: ok = undo_rename(entry, &gone, error); break;
    }
    if (missing != NULL)
        *missing = gone;
    return ok;
}
