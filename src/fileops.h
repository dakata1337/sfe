#pragma once

/* Copy / move / trash / delete jobs.
 *
 * A job runs in a worker thread with plain synchronous GIO calls, which keeps
 * the recursive logic readable. Everything it reports happens on the main
 * thread through signals:
 *
 *   "progress"                       status text / fraction changed
 *   "question" (SfeOpQuestion kind, GFile* source, GFile* dest, const char* error)
 *                                    the worker waits until sfe_file_op_answer()
 *   "finished" (GError* or NULL)     done; G_IO_ERROR_CANCELLED when cancelled
 *
 * The job holds a reference to itself while it runs. GTK-free, so it can be
 * tested without a display. */

#include <gio/gio.h>
#include <stdbool.h>

typedef enum {
    SFE_OP_COPY,
    SFE_OP_MOVE,
    SFE_OP_TRASH,
    SFE_OP_DELETE,
    SFE_OP_EXTRACT, /* archives -> a new folder each, inside dest_dir */
    SFE_OP_COMPRESS, /* sources (all in one folder) -> the archive dest_dir names */
} SfeOpKind;

typedef enum {
    SFE_QUESTION_CONFLICT, /* `dest` already exists */
    SFE_QUESTION_ERROR, /* an operation on `source` failed */
} SfeOpQuestion;

typedef enum {
    SFE_ANSWER_SKIP,
    SFE_ANSWER_REPLACE, /* conflict: overwrite */
    SFE_ANSWER_RENAME, /* conflict: keep both, "name (2).ext" */
    SFE_ANSWER_RETRY, /* error: try again */
    SFE_ANSWER_CANCEL, /* stop the whole job */
} SfeOpAnswer;

#define SFE_TYPE_FILE_OP (sfe_file_op_get_type())
G_DECLARE_FINAL_TYPE(SfeFileOp, sfe_file_op, SFE, FILE_OP, GObject)

/* `sources` is a list of GFile*; `dest_dir` is used by copy, move and
 * extract, and is the archive to create for compress. */
SfeFileOp* sfe_file_op_new(SfeOpKind kind, GList* sources, GFile* dest_dir);
void sfe_file_op_start(SfeFileOp* op);
void sfe_file_op_cancel(SfeFileOp* op);

/* Answers the pending "question". With `apply_to_all`, later questions of
 * the same kind get the same answer without asking. */
void sfe_file_op_answer(SfeFileOp* op, SfeOpAnswer answer, bool apply_to_all);

/* Copy / move: each source goes to the matching file of `dests` (GFile*)
 * instead of into dest_dir (undo moves things back this way). */
void sfe_file_op_set_destinations(SfeFileOp* op, GList* dests);

/* What the job did, for undo; complete once "finished" was emitted:
 *   copy, move   source -> the new file (after "keep both" renames; items
 *                merged into an existing folder are listed one by one)
 *   trash        source -> NULL
 *   extract      archive -> the new folder
 *   compress     NULL -> the archive */
typedef struct {
    GFile* source;
    GFile* result;
} SfeOpResult;
GPtrArray* sfe_file_op_get_results(SfeFileOp* op); /* SfeOpResult*, owned by op */

SfeOpKind sfe_file_op_get_kind(SfeFileOp* op);
char* sfe_file_op_get_status(SfeFileOp* op); /* newly allocated */
double sfe_file_op_get_fraction(SfeFileOp* op);

/* "name (n).ext" — the "keep both" name for the n-th copy. */
char* sfe_numbered_name(const char* name, bool is_dir, int n);
