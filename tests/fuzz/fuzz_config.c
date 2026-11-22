/* config.toml: parsing must not crash, and whatever parses must survive
 * the round trip every Settings change makes (write it out, parse it back:
 * sfe_config_copy). That round trip aborted in decimal-comma locales. */
#include "common.h"
#include "config.h"

int LLVMFuzzerInitialize(int* argc, char*** argv)
{
    (void)argc, (void)argv;
    fuzz_init();
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    SfeConfig* cfg = sfe_config_parse((const char*)data, size, NULL);
    if (cfg == NULL)
        return 0;
    char* text = sfe_config_to_string(cfg);
    GError* error = NULL;
    SfeConfig* again = sfe_config_parse(text, strlen(text), &error);
    FUZZ_CHECK(again != NULL, "written config does not parse: %s\n--- written:\n%s", error->message, text);
    char* text2 = sfe_config_to_string(again);
    FUZZ_CHECK(strcmp(text, text2) == 0, "round trip changed the config\n--- first:\n%s\n--- second:\n%s", text, text2);
    g_free(text2);
    g_free(text);
    sfe_config_free(again);
    sfe_config_free(cfg);
    return 0;
}
