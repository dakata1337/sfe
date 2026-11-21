#pragma once

/* Shared by the fuzz targets (libFuzzer). Each target is one .c file with
 * LLVMFuzzerTestOneInput(); see fuzz/README.md. */

#include <glib.h>
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
