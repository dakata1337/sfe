#include "archive.h"

#include <string.h>

typedef enum {
    FORMAT_NONE,
    FORMAT_TAR,
    FORMAT_ZIP,
    FORMAT_7Z,
    FORMAT_RAR,
    FORMAT_ISO,
} Format;

/* Longest extensions first, so ".tar.gz" wins over ".gz". */
static const struct {
    const char* ext;
    Format format;
} extensions[] = {
    { ".tar.gz", FORMAT_TAR },
    { ".tar.bz2", FORMAT_TAR },
    { ".tar.xz", FORMAT_TAR },
    { ".tar.zst", FORMAT_TAR },
    { ".tar.lz", FORMAT_TAR },
    { ".tar.lzma", FORMAT_TAR },
    { ".tgz", FORMAT_TAR },
    { ".tbz", FORMAT_TAR },
    { ".tbz2", FORMAT_TAR },
    { ".txz", FORMAT_TAR },
    { ".tzst", FORMAT_TAR },
    { ".tar", FORMAT_TAR },
    { ".zip", FORMAT_ZIP },
    { ".7z", FORMAT_7Z },
    { ".rar", FORMAT_RAR },
    { ".iso", FORMAT_ISO },
};

/* The tools for each format, in order of preference. */
typedef enum {
    TOOL_BSDTAR,
    TOOL_TAR,
    TOOL_UNZIP,
    TOOL_7Z,
    TOOL_7ZZ,
    TOOL_UNRAR,
} Tool;

static const char* const tool_names[] = { "bsdtar", "tar", "unzip", "7z", "7zz", "unrar" };

static const Tool tools_tar[] = { TOOL_BSDTAR, TOOL_TAR };
static const Tool tools_zip[] = { TOOL_BSDTAR, TOOL_UNZIP, TOOL_7Z, TOOL_7ZZ };
static const Tool tools_7z[] = { TOOL_7Z, TOOL_7ZZ, TOOL_BSDTAR };
static const Tool tools_rar[] = { TOOL_UNRAR, TOOL_7Z, TOOL_7ZZ, TOOL_BSDTAR };
static const Tool tools_iso[] = { TOOL_BSDTAR, TOOL_7Z, TOOL_7ZZ };

static Format format_of(const char* name, size_t* ext_len)
{
    g_autofree char* lower = g_utf8_strdown(name, -1);
    for (size_t i = 0; i < G_N_ELEMENTS(extensions); i++) {
        if (g_str_has_suffix(lower, extensions[i].ext) && strlen(lower) > strlen(extensions[i].ext)) {
            if (ext_len != NULL)
                *ext_len = strlen(extensions[i].ext);
            return extensions[i].format;
        }
    }
    return FORMAT_NONE;
}

bool sfe_archive_is_archive(const char* name)
{
    return format_of(name, NULL) != FORMAT_NONE;
}

char* sfe_archive_folder_name(const char* name)
{
    size_t ext_len = 0;
    if (format_of(name, &ext_len) == FORMAT_NONE)
        return g_strdup(name);
    return g_strndup(name, strlen(name) - ext_len);
}

static char** argv_for(Tool tool, const char* program, const char* archive, const char* dest)
{
    switch (tool) {
    case TOOL_BSDTAR:
    case TOOL_TAR:
        return g_strdupv((char*[]) { (char*)program, "-xf", (char*)archive, "-C", (char*)dest, NULL });
    case TOOL_UNZIP:
        return g_strdupv((char*[]) { (char*)program, "-q", (char*)archive, "-d", (char*)dest, NULL });
    case TOOL_7Z:
    case TOOL_7ZZ: {
        g_autofree char* out = g_strconcat("-o", dest, NULL);
        return g_strdupv((char*[]) { (char*)program, "x", "-y", out, (char*)archive, NULL });
    }
    case TOOL_UNRAR: {
        g_autofree char* out = g_strconcat(dest, "/", NULL); /* unrar wants the slash */
        return g_strdupv((char*[]) { (char*)program, "x", "-idq", "-o+", (char*)archive, out, NULL });
    }
    }
    return NULL;
}

char** sfe_archive_extract_argv(const char* archive, const char* dest, SfeFindProgram find)
{
    g_autofree char* base = g_path_get_basename(archive);
    const Tool* tools = NULL;
    size_t n = 0;

    switch (format_of(base, NULL)) {
    case FORMAT_TAR: tools = tools_tar; n = G_N_ELEMENTS(tools_tar); break;
    case FORMAT_ZIP: tools = tools_zip; n = G_N_ELEMENTS(tools_zip); break;
    case FORMAT_7Z: tools = tools_7z; n = G_N_ELEMENTS(tools_7z); break;
    case FORMAT_RAR: tools = tools_rar; n = G_N_ELEMENTS(tools_rar); break;
    case FORMAT_ISO: tools = tools_iso; n = G_N_ELEMENTS(tools_iso); break;
    case FORMAT_NONE: return NULL;
    }

    for (size_t i = 0; i < n; i++) {
        g_autofree char* program = find(tool_names[tools[i]]);
        if (program != NULL)
            return argv_for(tools[i], program, archive, dest);
    }
    return NULL;
}

bool sfe_archive_can_extract(const char* name)
{
    g_auto(GStrv) argv = sfe_archive_extract_argv(name, ".", g_find_program_in_path); /* never run */
    return argv != NULL;
}
