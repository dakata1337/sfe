#include "sizecount.h"

#include <stdbool.h>

static guint64 measure(GFile* file, GCancellable* cancellable, bool* is_dir)
{
    g_autoptr(GFileInfo) info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_SIZE,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, NULL);
    *is_dir = info != NULL && g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
    if (info == NULL)
        return 0;
    if (!*is_dir)
        return (guint64)g_file_info_get_size(info);
    guint64 total = 0;
    g_autoptr(GFileEnumerator) en = g_file_enumerate_children(file, G_FILE_ATTRIBUTE_STANDARD_NAME,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, NULL);
    GFileInfo* child;
    while (en != NULL && (child = g_file_enumerator_next_file(en, cancellable, NULL)) != NULL) {
        g_autoptr(GFile) f = g_file_get_child(file, g_file_info_get_name(child));
        bool dir;
        total += measure(f, cancellable, &dir);
        g_object_unref(child);
    }
    return total;
}

void sfe_size_count(GList* files, GCancellable* cancellable, SfeSizeCount* count)
{
    *count = (SfeSizeCount) { 0 };
    for (GList* l = files; l != NULL && !g_cancellable_is_cancelled(cancellable); l = l->next) {
        bool dir;
        count->bytes += measure(l->data, cancellable, &dir);
        if (dir)
            count->folders++;
        else
            count->files++;
    }
}

static void count_thread(GTask* task, gpointer source, gpointer data, GCancellable* cancellable)
{
    (void)source;
    SfeSizeCount* count = g_new(SfeSizeCount, 1);
    sfe_size_count(data, cancellable, count);
    if (g_cancellable_is_cancelled(cancellable)) {
        g_free(count);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "cancelled");
        return;
    }
    g_task_return_pointer(task, count, g_free);
}

static void free_files(gpointer files)
{
    g_list_free_full(files, g_object_unref);
}

void sfe_size_count_async(GList* files, GCancellable* cancellable, GAsyncReadyCallback callback, gpointer data)
{
    GTask* task = g_task_new(NULL, cancellable, callback, data);
    g_task_set_task_data(task, g_list_copy_deep(files, (GCopyFunc)(void (*)(void))g_object_ref, NULL), free_files);
    g_task_run_in_thread(task, count_thread);
    g_object_unref(task);
}

gboolean sfe_size_count_finish(GAsyncResult* result, SfeSizeCount* count, GError** error)
{
    SfeSizeCount* found = g_task_propagate_pointer(G_TASK(result), error);
    if (found == NULL)
        return FALSE;
    *count = *found;
    g_free(found);
    return TRUE;
}

char* sfe_size_count_items(const SfeSizeCount* count)
{
    guint n = count->files + count->folders;
    if (count->folders == 0)
        return n == 1 ? g_strdup("1 file") : g_strdup_printf("%u files", n);
    if (count->files == 0)
        return n == 1 ? g_strdup("1 folder") : g_strdup_printf("%u folders", n);
    return g_strdup_printf("%u items", n);
}
