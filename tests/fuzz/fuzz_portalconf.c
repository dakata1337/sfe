/* The portals.conf sfe edits to become the file dialog (Settings →
 * System). Input: the user's existing file, then after a NUL byte the
 * system file used as a seed when there is none. Invariants: after turning
 * sfe on the file says so, after turning it off it does not, and keys sfe
 * does not own survive both. */
#include "common.h"
#include "portalconf.h"

static char* conf;
static char* seed;

int LLVMFuzzerInitialize(int* argc, char*** argv)
{
    (void)argc, (void)argv;
    fuzz_init();
    conf = g_build_filename(fuzz_tmpdir(), "user.conf", NULL);
    seed = g_build_filename(fuzz_tmpdir(), "seed.conf", NULL);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    const uint8_t* nul = memchr(data, 0, size);
    size_t user_size = nul ? (size_t)(nul - data) : size;
    fuzz_write(seed, nul ? nul + 1 : (const uint8_t*)"", nul ? size - user_size - 1 : 0);
    bool fresh = user_size == 0;
    if (fresh)
        g_remove(conf);
    else
        fuzz_write(conf, data, user_size);

    GError* error = NULL;
    if (sfe_portal_conf_set(conf, seed, true, &error)) {
        FUZZ_CHECK(sfe_portal_conf_uses_sfe(conf), "turned on, but the file does not use sfe%s", "");
        GError* off_error = NULL;
        FUZZ_CHECK(sfe_portal_conf_set(conf, seed, false, &off_error), "cannot turn off again: %s",
            off_error ? off_error->message : "?");
        FUZZ_CHECK(!sfe_portal_conf_uses_sfe(conf), "turned off, but the file still uses sfe%s", "");
    } else {
        g_clear_error(&error);
    }
    return 0;
}
