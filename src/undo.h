#pragma once

/* The undo history: what was done to files, newest last, shared by every
 * window of the application.
 *
 * Copies, moves, trashing, renames, new files and folders, links,
 * extracting and compressing are recorded; deleting permanently is not
 * (there is nothing to bring back). Undoing an entry:
 *
 *   copy, new file / folder, link, extract, compress
 *                    the files made are moved to the trash (not deleted)
 *   move             the files go back where they were
 *   trash            the files come back from the trash
 *   rename           the files get their old names back
 *
 * GTK-free and unit-tested; the window runs the file jobs it returns. */

#include "fileops.h"
#include <gio/gio.h>
#include <stdbool.h>

typedef enum {
    SFE_UNDO_COPY,
    SFE_UNDO_MOVE,
    SFE_UNDO_TRASH,
    SFE_UNDO_RENAME,
    SFE_UNDO_CREATE, /* new file or folder */
    SFE_UNDO_LINK,
    SFE_UNDO_EXTRACT,
    SFE_UNDO_COMPRESS,
} SfeUndoKind;

/* One file: where it was and where it is now.
 *   copy     source -> copy          move     old -> new
 *   trash    original -> NULL        rename   old name -> new name
 *   create, link, compress   NULL -> the new file
 *   extract  archive -> the new folder */
typedef struct {
    GFile* before;
    GFile* after;
} SfeUndoPair;

typedef struct {
    guint id; /* 1, 2, 3 ... never reused */
    SfeUndoKind kind;
    GPtrArray* pairs; /* SfeUndoPair* */
    GDateTime* time;
} SfeUndoEntry;

/* At most this many entries are kept; the oldest go first. */
#define SFE_UNDO_MAX 100

/* A pair for sfe_undo_record() (either file may be NULL). */
SfeUndoPair* sfe_undo_pair_new(GFile* before, GFile* after);

/* Adds an entry and takes `pairs` (SfeUndoPair*); nothing is recorded for
 * an empty list (NULL is returned). */
SfeUndoEntry* sfe_undo_record(SfeUndoKind kind, GPtrArray* pairs);

/* Records a rename done with sfe_rename_files() (unchanged names skipped). */
SfeUndoEntry* sfe_undo_record_renames(GFile** files, char** names, int n);

/* Records what a finished copy / move / trash / extract / compress job did. */
SfeUndoEntry* sfe_undo_record_op(SfeFileOp* op);

/* The entries, oldest first (owned by the history). */
GPtrArray* sfe_undo_entries(void);

/* The entry `id`, or the newest for 0; NULL if there is none. */
SfeUndoEntry* sfe_undo_find(guint id);

/* "copy 3 items to ~/Backup", "rename notes.txt → todo.txt". */
char* sfe_undo_describe(const SfeUndoEntry* entry);

/* Undoes `entry` and drops it from the history. Renames and restores from
 * the trash happen right away; for the copy-like and move kinds `*job`
 * receives the file job to run (not started; its results are not to be
 * recorded again), else NULL. Files that are gone are skipped (`missing`
 * receives their count); false with `error` when nothing could be undone
 * (the entry is dropped anyway when none of its files are left). */
bool sfe_undo_apply(SfeUndoEntry* entry, SfeFileOp** job, guint* missing, GError** error);

/* Forgets everything (tests). */
void sfe_undo_clear(void);
