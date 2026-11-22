/* The ":" command line: tokenizing, splitting and quoting. Invariants:
 * argument offsets lie inside the line, and quoting any argument then
 * splitting gives that argument back (completion inserts quoted names). */
#include "cmdline.h"
#include "common.h"

int LLVMFuzzerInitialize(int* argc, char*** argv)
{
    (void)argc, (void)argv;
    fuzz_init();
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    char* line = fuzz_string(data, size);
    int len = (int)strlen(line);

    GPtrArray* args = sfe_cmdline_tokenize(line);
    for (guint i = 0; i < args->len; i++) {
        const SfeCmdArg* a = g_ptr_array_index(args, i);
        FUZZ_CHECK(0 <= a->start && a->start <= a->end && a->end <= len, "arg %u at %d-%d in a line of %d", i,
            a->start, a->end, len);
    }
    g_ptr_array_unref(args);

    char** argv = sfe_cmdline_split(line, NULL);
    for (char** a = argv; a != NULL && *a != NULL; a++) {
        char* quoted = sfe_cmdline_quote(*a);
        GError* error = NULL;
        char** back = sfe_cmdline_split(quoted, &error);
        FUZZ_CHECK(back != NULL && back[0] != NULL && back[1] == NULL && strcmp(back[0], *a) == 0,
            "quote/split round trip: [%s] -> [%s] -> [%s]%s", *a, quoted, back && back[0] ? back[0] : "(none)",
            back && back[0] && back[1] ? " and more" : "");
        g_strfreev(back);
        g_free(quoted);
    }
    g_strfreev(argv);

    /* Completion: the first line is the query, the others candidates. */
    char** lines = g_strsplit(line, "\n", 64);
    if (lines[0] != NULL && g_utf8_validate(line, -1, NULL)) { /* typed text, display names */
        GPtrArray* cands = g_ptr_array_new_with_free_func(sfe_candidate_free);
        for (int i = 1; lines[i] != NULL; i++)
            g_ptr_array_add(cands, sfe_candidate_new(lines[i], lines[i], NULL, i % 3));
        guint before = cands->len;
        GPtrArray* ranked = sfe_cmdline_rank(cands, lines[0]);
        FUZZ_CHECK(ranked->len <= before, "rank grew %u -> %u", before, ranked->len);
        g_ptr_array_unref(ranked);
    }
    g_strfreev(lines);
    g_free(line);
    return 0;
}
