/* File dialog filters (GVariant text a(sa(us)), from the portal / the
 * command line). Input: the filter text, a NUL byte, file names (one per
 * line) to test against every parsed filter. */
#include "common.h"
#include "filefilter.h"

int LLVMFuzzerInitialize(int* argc, char*** argv)
{
    (void)argc, (void)argv;
    fuzz_init();
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    const uint8_t* nul = memchr(data, 0, size);
    char* text = fuzz_string(data, nul ? (size_t)(nul - data) : size);
    char* names = nul ? fuzz_string(nul + 1, size - (nul - data) - 1) : g_strdup("a.png\nb.txt\nfolder");

    GPtrArray* filters = sfe_file_filters_parse(text, NULL);
    if (filters != NULL) {
        char** list = g_strsplit(names, "\n", 32);
        for (guint i = 0; i < filters->len; i++) {
            const SfeFileFilter* f = g_ptr_array_index(filters, i);
            for (char** n = list; *n != NULL; n++) {
                sfe_file_filter_matches(f, *n, NULL);
                sfe_file_filter_matches(f, *n, "image/png");
                sfe_file_filter_shows(f, *n, "inode/directory", true);
            }
        }
        g_strfreev(list);
        g_ptr_array_unref(filters);
    }
    g_free(names);
    g_free(text);
    return 0;
}
