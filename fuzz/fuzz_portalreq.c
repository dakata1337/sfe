/* File dialog requests from other programs, through the desktop portal
 * (D-Bus, so not trusted). Input: 1 byte picks the method, then the
 * options a{sv} as serialized GVariant data, as D-Bus delivers it; after
 * a NUL byte, what `sfe --choose` printed. */
#include "common.h"
#include "portalreq.h"

int LLVMFuzzerInitialize(int* argc, char*** argv)
{
    (void)argc, (void)argv;
    fuzz_init();
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size < 1)
        return 0;
    static const char* methods[] = { "OpenFile", "SaveFile", "SaveFiles" };
    const char* method = methods[data[0] % 3];
    data++, size--;
    const uint8_t* nul = memchr(data, 0xff, size); /* (0 is common in GVariant data) */
    size_t vsize = nul ? (size_t)(nul - data) : size;

    GBytes* bytes = g_bytes_new(data, vsize);
    GVariant* raw = g_variant_ref_sink(g_variant_new_from_bytes(G_VARIANT_TYPE("a{sv}"), bytes, FALSE));
    GVariant* options = g_variant_take_ref(g_variant_get_normal_form(raw)); /* what GDBus hands over */
    g_variant_unref(raw);
    g_bytes_unref(bytes);

    SfePortalRequest* req = sfe_portal_request_new(method, "", "Title", options);
    if (req != NULL) {
        char* output = nul ? fuzz_string(nul + 1, size - vsize - 1) : g_strdup("file:///tmp/a\n");
        GVariant* results = g_variant_ref_sink(sfe_portal_request_results(req, output));
        g_variant_unref(results);
        g_free(output);
        sfe_portal_request_free(req);
    }
    g_variant_unref(options);
    return 0;
}
