#pragma once

/* The total size of some files and folders (folders counted through),
 * measured in a thread. GTK-free. */

#include <gio/gio.h>

typedef struct {
    guint64 bytes; /* the files' sizes; folders themselves count 0 */
    guint files; /* among the given ones: not folders */
    guint folders; /* among the given ones */
} SfeSizeCount;

/* Starts counting `files` (GFile*, copied); symbolic links are not followed. */
void sfe_size_count_async(GList* files, GCancellable* cancellable, GAsyncReadyCallback callback, gpointer data);

/* FALSE when it was cancelled. */
gboolean sfe_size_count_finish(GAsyncResult* result, SfeSizeCount* count, GError** error);

/* The same, in the calling thread. */
void sfe_size_count(GList* files, GCancellable* cancellable, SfeSizeCount* count);

/* "3 files", "1 folder", "4 items" for a count's given files and folders. */
char* sfe_size_count_items(const SfeSizeCount* count);
