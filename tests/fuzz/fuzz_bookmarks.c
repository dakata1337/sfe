/* The GTK bookmarks file (~/.config/gtk-3.0/bookmarks), which other
 * programs write too: "URI [label]" per line. Input: the file, then after a
 * NUL byte sfe's own bookmark entries (config.toml strings), one per line.
 * Invariants: every bookmark has a file and a non-empty UTF-8 label. */
#include "bookmarks.h"
#include "common.h"

static char* gtk_path;

int LLVMFuzzerInitialize(int* argc, char*** argv)
{
    (void)argc, (void)argv;
    fuzz_init();
    gtk_path = g_build_filename(fuzz_tmpdir(), "bookmarks", NULL);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    const uint8_t* nul = memchr(data, 0, size);
    size_t file_size = nul ? (size_t)(nul - data) : size;
    fuzz_write(gtk_path, data, file_size);

    SfeConfig* cfg = sfe_config_new();
    cfg->show_gtk_bookmarks = true;
    if (nul != NULL) {
        char* entries = fuzz_string(nul + 1, size - file_size - 1);
        char** lines = g_strsplit(entries, "\n", 16);
        for (char** l = lines; *l != NULL; l++)
            if (**l != '\0' && g_utf8_validate(*l, -1, NULL)) /* (config strings are UTF-8) */
                g_ptr_array_add(cfg->bookmarks, g_strdup(*l));
        g_strfreev(lines);
        g_free(entries);
    }

    GPtrArray* list = sfe_bookmarks_list(cfg, gtk_path);
    for (guint i = 0; i < list->len; i++) {
        const SfeBookmark* b = g_ptr_array_index(list, i);
        FUZZ_CHECK(b->file != NULL, "bookmark %u has no file", i);
        FUZZ_CHECK(b->label != NULL && *b->label != '\0' && g_utf8_validate(b->label, -1, NULL),
            "bookmark %u: bad label [%s]", i, b->label ? b->label : "(null)");
        g_autoptr(GFile) dir = g_object_ref(b->file);
        if (b->config_index >= 0)
            FUZZ_CHECK(sfe_bookmarks_find(cfg, dir) >= 0, "config bookmark %u not found again", i);
    }
    g_ptr_array_unref(list);
    sfe_config_free(cfg);
    return 0;
}
