/* Key sequences from the [keys] section ("<C-w>v", "gg"). Invariants: a
 * parsed sequence formats and parses back to the same tokens, and binding
 * then looking it up finds the command. */
#include "common.h"
#include "keymap.h"

int LLVMFuzzerInitialize(int* argc, char*** argv)
{
    (void)argc, (void)argv;
    fuzz_init();
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    char* seq = fuzz_string(data, size);
    char** tokens = sfe_keymap_parse(seq, NULL);
    if (tokens != NULL) {
        guint n = g_strv_length(tokens);
        char* text = sfe_keymap_format(tokens, n);
        GError* error = NULL;
        char** again = sfe_keymap_parse(text, &error);
        FUZZ_CHECK(again != NULL, "[%s] formats as [%s], which does not parse: %s", seq, text, error->message);
        FUZZ_CHECK(g_strv_equal((const char* const*)tokens, (const char* const*)again),
            "[%s] -> [%s] parses to other tokens", seq, text);
        g_strfreev(again);
        g_free(text);

        SfeKeymap* map = sfe_keymap_new();
        if (n > 0 && sfe_keymap_bind(map, seq, "quit", NULL)) {
            const char* command = NULL;
            SfeKeysResult r = sfe_keymap_lookup(map, tokens, n, &command, NULL);
            FUZZ_CHECK(r == SFE_KEYS_MATCH && g_strcmp0(command, "quit") == 0, "bound [%s] but lookup gave %d", seq,
                (int)r);
        }
        sfe_keymap_free(map);
        g_strfreev(tokens);
    }
    g_free(seq);
    return 0;
}
