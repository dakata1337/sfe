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
    TOOL_ZIP,
} Tool;

static const char* const tool_names[] = { "bsdtar", "tar", "unzip", "7z", "7zz", "unrar", "zip" };

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
    case TOOL_ZIP:
        break; /* creates only */
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

/* ------------------------------------------------------------------------ */
/* Creating archives                                                         */
/* ------------------------------------------------------------------------ */

static const Tool create_zip[] = { TOOL_BSDTAR, TOOL_ZIP, TOOL_7Z, TOOL_7ZZ };
static const Tool create_tar[] = { TOOL_BSDTAR, TOOL_TAR };
static const Tool create_7z[] = { TOOL_7Z, TOOL_7ZZ, TOOL_BSDTAR };

static const struct {
    SfeArchiveFormat format;
    const Tool* tools;
    size_t n_tools;
    const char* compressor; /* what GNU tar needs for it, or NULL */
} creatable[] = {
    { { ".zip", "zip" }, create_zip, G_N_ELEMENTS(create_zip), NULL },
    { { ".tar.gz", "tar.gz" }, create_tar, G_N_ELEMENTS(create_tar), "gzip" },
    { { ".tar.xz", "tar.xz" }, create_tar, G_N_ELEMENTS(create_tar), "xz" },
    { { ".tar.zst", "tar.zst" }, create_tar, G_N_ELEMENTS(create_tar), "zstd" },
    { { ".7z", "7z" }, create_7z, G_N_ELEMENTS(create_7z), NULL },
    { { ".tar", "tar" }, create_tar, G_N_ELEMENTS(create_tar), NULL },
};

static SfeArchiveFormat formats[G_N_ELEMENTS(creatable)];

const SfeArchiveFormat* sfe_archive_formats(size_t* n)
{
    for (size_t i = 0; i < G_N_ELEMENTS(creatable); i++)
        formats[i] = creatable[i].format;
    *n = G_N_ELEMENTS(creatable);
    return formats;
}

/* The row of `creatable` for an archive name, by its extension; -1 if none. */
static int creatable_for(const char* archive)
{
    g_autofree char* base = g_path_get_basename(archive);
    g_autofree char* lower = g_utf8_strdown(base, -1);
    int best = -1;
    for (size_t i = 0; i < G_N_ELEMENTS(creatable); i++) {
        const char* ext = creatable[i].format.ext;
        if (g_str_has_suffix(lower, ext) && strlen(lower) > strlen(ext)
            && (best < 0 || strlen(ext) > strlen(creatable[best].format.ext)))
            best = (int)i; /* the longest match: ".tar.gz", not ".gz" */
    }
    return best;
}

/* Names that a tool could read as an option ("-x") or a list file ("@x")
 * get a "./" in front. */
static void add_names(GPtrArray* argv, char* const* names)
{
    for (char* const* n = names; *n != NULL; n++)
        g_ptr_array_add(argv, (**n == '-' || **n == '@') ? g_strconcat("./", *n, NULL) : g_strdup(*n));
}

char** sfe_archive_compress_argv(const char* archive, char* const* names, SfeFindProgram find)
{
    int row = creatable_for(archive);
    if (row < 0)
        return NULL;

    for (size_t i = 0; i < creatable[row].n_tools; i++) {
        Tool tool = creatable[row].tools[i];
        g_autofree char* program = find(tool_names[tool]);
        if (program == NULL)
            continue;
        if (tool == TOOL_TAR && creatable[row].compressor != NULL) {
            g_autofree char* compressor = find(creatable[row].compressor);
            if (compressor == NULL)
                continue;
        }

        GPtrArray* argv = g_ptr_array_new();
        g_ptr_array_add(argv, g_strdup(program));
        switch (tool) {
        case TOOL_BSDTAR:
        case TOOL_TAR: /* -a: compression from the extension */
            g_ptr_array_add(argv, g_strdup("-acf"));
            g_ptr_array_add(argv, g_strdup(archive));
            break;
        case TOOL_ZIP: /* -y: keep symlinks, -nw: names are not patterns */
            g_ptr_array_add(argv, g_strdup("-qry"));
            g_ptr_array_add(argv, g_strdup("-nw"));
            g_ptr_array_add(argv, g_strdup(archive));
            break;
        case TOOL_7Z:
        case TOOL_7ZZ: /* -spd: names are not patterns */
            g_ptr_array_add(argv, g_strdup("a"));
            g_ptr_array_add(argv, g_strdup(g_str_has_suffix(creatable[row].format.ext, ".zip") ? "-tzip" : "-t7z"));
            g_ptr_array_add(argv, g_strdup("-y"));
            g_ptr_array_add(argv, g_strdup("-spd"));
            g_ptr_array_add(argv, g_strdup(archive));
            break;
        case TOOL_UNZIP:
        case TOOL_UNRAR:
            break; /* never listed for creating */
        }
        add_names(argv, names);
        g_ptr_array_add(argv, NULL);
        return (char**)g_ptr_array_free(argv, FALSE);
    }
    return NULL;
}

bool sfe_archive_can_compress(const char* ext)
{
    g_autofree char* name = g_strconcat("archive", ext, NULL);
    char* names[] = { "x", NULL };
    g_auto(GStrv) argv = sfe_archive_compress_argv(name, names, g_find_program_in_path); /* never run */
    return argv != NULL;
}
