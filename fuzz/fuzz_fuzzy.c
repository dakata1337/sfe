/* Fuzzy matching (search, completion). Input: query, a NUL byte, text.
 * Invariants: score and match agree, the positions are increasing byte
 * offsets inside the text, and an empty query matches with score 0. Both
 * strings are UTF-8, as from the callers (typed text, display names). */
#include "common.h"
#include "fuzzy.h"

int LLVMFuzzerInitialize(int* argc, char*** argv)
{
    (void)argc, (void)argv;
    fuzz_init();
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    const uint8_t* nul = memchr(data, 0, size);
    if (nul == NULL)
        return 0;
    char* query = fuzz_string(data, nul - data);
    char* text = fuzz_string(nul + 1, size - (nul - data) - 1);
    int len = (int)strlen(text);
    if (!g_utf8_validate(query, -1, NULL) || !g_utf8_validate(text, -1, NULL)) {
        g_free(text);
        g_free(query);
        return 0;
    }

    GArray* positions = g_array_new(FALSE, FALSE, sizeof(int));
    int score = sfe_fuzzy_score(query, text);
    int match = sfe_fuzzy_match(query, text, positions);
    FUZZ_CHECK(score == match, "score %d but match %d for [%s] in [%s]", score, match, query, text);
    if (*query == '\0')
        FUZZ_CHECK(score == 0, "empty query scored %d", score);
    if (match != SFE_FUZZY_NO_MATCH) {
        for (guint i = 0; i < positions->len; i++) {
            int p = g_array_index(positions, int, i);
            FUZZ_CHECK(p >= 0 && p < len, "position %d outside [%s] (%d bytes)", p, text, len);
            if (i > 0)
                FUZZ_CHECK(p > g_array_index(positions, int, i - 1), "positions not increasing at %u", i);
        }
    }
    g_array_unref(positions);
    g_free(text);
    g_free(query);
    return 0;
}
