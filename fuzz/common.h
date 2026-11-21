#pragma once

/* Shared by the fuzz targets (libFuzzer). Each target is one .c file with
 * LLVMFuzzerTestOneInput(); see fuzz/README.md. */

#include <glib.h>
#include <glib/gstdio.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A failed invariant: report it the way libFuzzer understands (a crash). */
#define FUZZ_CHECK(cond, ...)                                       \
    do {                                                            \
        if (!(cond)) {                                              \
            fprintf(stderr, "invariant failed: %s\n  ", #cond);     \
            fprintf(stderr, __VA_ARGS__);                           \
            fprintf(stderr, "\n");                                  \
            abort();                                                \
        }                                                           \
    } while (0)

/* The input as a NUL-terminated string (embedded NULs end it early). */
static inline char* fuzz_string(const uint8_t* data, size_t size)
{
    return g_strndup((const char*)data, size);
}

/* Messages from the code under test: criticals (failed g_return_if_fail,
 * i.e. bugs) abort, the rest is dropped so the output stays readable. */
static GLogWriterOutput fuzz_log_writer(GLogLevelFlags level, const GLogField* fields, gsize n, gpointer data)
{
    (void)fields, (void)n, (void)data;
    if (level & (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL))
        abort();
    return G_LOG_WRITER_HANDLED;
}

static inline void fuzz_init(void)
{
    g_log_set_writer_func(fuzz_log_writer, NULL, NULL);
}

/* Deletes `path` and everything below it (no symlinks followed). */
static void fuzz_remove_tree(const char* path)
{
    GDir* dir = g_file_test(path, G_FILE_TEST_IS_SYMLINK) ? NULL : g_dir_open(path, 0, NULL);
    if (dir != NULL) {
        const char* name;
        while ((name = g_dir_read_name(dir)) != NULL) {
            char* child = g_build_filename(path, name, NULL);
            fuzz_remove_tree(child);
            g_free(child);
        }
        g_dir_close(dir);
    }
    g_remove(path);
}

static char* fuzz_tmp;
static void fuzz_remove_tmp(void)
{
    fuzz_remove_tree(fuzz_tmp);
}

/* A temporary folder for targets that read files, removed at exit (left
 * behind only after a crash). Also XDG_DATA_HOME and HOME, so nothing
 * touches the user's files; call it before GLib reads them. */
static inline const char* fuzz_tmpdir(void)
{
    if (fuzz_tmp == NULL) {
        if (g_file_test("/dev/shm", G_FILE_TEST_IS_DIR)) /* tmpfs: no disk in the loop */
            g_setenv("TMPDIR", "/dev/shm", TRUE);
        fuzz_tmp = g_dir_make_tmp("sfe-fuzz-XXXXXX", NULL);
        g_assert(fuzz_tmp != NULL);
        g_setenv("XDG_DATA_HOME", fuzz_tmp, TRUE);
        g_setenv("HOME", fuzz_tmp, TRUE);
        atexit(fuzz_remove_tmp);
    }
    return fuzz_tmp;
}

/* Writes the input to `path` (not g_file_set_contents: its fsync made the
 * file targets wait on the disk most of the time). */
static inline void fuzz_write(const char* path, const uint8_t* data, size_t size)
{
    FILE* f = fopen(path, "wb");
    g_assert(f != NULL);
    g_assert(fwrite(data, 1, size, f) == size);
    fclose(f);
}
