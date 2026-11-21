/* Mass rename rules. Input: 8 bytes of settings (regex, case, counter
 * start and step), then template, find, replace and file name separated by
 * newlines, all UTF-8 (the dialog passes display names and entry text).
 * Invariant: applying a compiled rule never crashes and gives UTF-8. */
#include "common.h"
#include "rename.h"

int LLVMFuzzerInitialize(int* argc, char*** argv)
{
    (void)argc, (void)argv;
    fuzz_init();
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size < 8)
        return 0;
    int16_t start, step;
    memcpy(&start, data + 2, sizeof(start));
    memcpy(&step, data + 4, sizeof(step));
    char* rest = fuzz_string(data + 8, size - 8);
    if (!g_utf8_validate(rest, -1, NULL)) {
        g_free(rest);
        return 0;
    }
    char** parts = g_strsplit(rest, "\n", 4);
    const char* part[4] = { NULL, NULL, NULL, NULL };
    for (int i = 0; i < 4 && parts[i] != NULL; i++)
        part[i] = parts[i];

    SfeRenameRule rule = {
        .template_ = part[0],
        .find = part[1],
        .replace = part[2] ? part[2] : "",
        .regex = data[0] & 1,
        .match_case = data[0] & 2,
        .case_mode = (SfeCaseMode)(data[1] % 4),
        .counter_start = start,
        .counter_step = step,
    };
    const char* name = part[3] ? part[3] : "photo.jpg";
    SfeRenamer* r = sfe_renamer_new(&rule, NULL);
    if (r != NULL) {
        int index = data[6] | (data[7] << 8);
        char* out = sfe_renamer_apply(r, name, data[0] & 4, index);
        if (out != NULL)
            FUZZ_CHECK(g_utf8_validate(out, -1, NULL), "invalid UTF-8 from [%s]", name);
        g_free(out);
        sfe_renamer_free(r);
    }
    g_strfreev(parts);
    g_free(rest);
    return 0;
}
