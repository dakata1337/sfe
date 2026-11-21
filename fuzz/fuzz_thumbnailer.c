/* .thumbnailer files (keyfiles in <data dir>/thumbnailers) and the command
 * line built from them for a file. Input: the .thumbnailer file, a NUL
 * byte, the file's path (file names come from anywhere: downloads,
 * archives). Invariant, for safety: the path is never split or merged into
 * other arguments; an Exec word that is exactly %i gives exactly the path. */
#include "common.h"
#include "thumbs.h"

static char* dir;
static char* file;

int LLVMFuzzerInitialize(int* argc, char*** argv)
{
    (void)argc, (void)argv;
    fuzz_init();
    dir = g_strdup(fuzz_tmpdir());
    char* sub = g_build_filename(dir, "thumbnailers", NULL);
    g_mkdir_with_parents(sub, 0700);
    file = g_build_filename(sub, "fuzz.thumbnailer", NULL);
    g_free(sub);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    const uint8_t* nul = memchr(data, 0, size);
    size_t file_size = nul ? (size_t)(nul - data) : size;
    fuzz_write(file, data, file_size);
    char* path = nul ? fuzz_string(nul + 1, size - file_size - 1) : g_strdup("/tmp/My Photo's \"best\".png");

    const char* dirs[] = { dir, NULL };
    GPtrArray* thumbnailers = sfe_thumbnailers_load(dirs);
    for (guint i = 0; i < thumbnailers->len; i++) {
        const SfeThumbnailer* t = g_ptr_array_index(thumbnailers, i);
        for (char** m = t->mime_types; m != NULL && *m != NULL; m++)
            FUZZ_CHECK(sfe_thumbnailers_find(thumbnailers, *m) != NULL, "type %s not found", *m);
        char** argv = sfe_thumbnailer_argv(t, path, "file:///x", "/tmp/out.png", 128, NULL);
        char** words = NULL;
        if (argv != NULL && g_shell_parse_argv(t->exec, NULL, &words, NULL)) {
            FUZZ_CHECK(g_strv_length(argv) == g_strv_length(words), "argv has %u words, Exec %u",
                g_strv_length(argv), g_strv_length(words));
            for (int w = 0; words[w] != NULL; w++)
                if (strcmp(words[w], "%i") == 0)
                    FUZZ_CHECK(strcmp(argv[w], path) == 0, "word %d is [%s], not the path [%s]", w, argv[w], path);
        }
        g_strfreev(words);
        g_strfreev(argv);
    }
    g_ptr_array_unref(thumbnailers);
    g_free(path);
    return 0;
}
