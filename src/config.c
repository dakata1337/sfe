#include "config.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <tomlc17.h>

G_DEFINE_QUARK(sfe-config-error-quark, sfe_config_error)

/* ------------------------------------------------------------------------ */
/* Option table: one row per scalar option.                                  */
/* ------------------------------------------------------------------------ */

typedef enum {
    OPT_BOOL,
    OPT_INT,
    OPT_DOUBLE,
    OPT_STRING,
    OPT_ENUM,
} OptType;

typedef struct {
    const char* section;
    const char* key;
    OptType type;
    size_t offset; /* offsetof(SfeConfig, field) */
    const char* help; /* written as a comment above the key */
    double min, max; /* OPT_INT, OPT_DOUBLE */
    const char* const* choices; /* OPT_ENUM: NULL-terminated, index == value */
} Option;

static const char* const view_names[] = { "grid", "list", NULL };
static const char* const sort_names[] = { "name", "size", "modified", "type", NULL };

#define FIELD(f) offsetof(SfeConfig, f)

static const Option options[] = {
    { "general", "start_path", OPT_STRING, FIELD(start_path), "Directory opened by new windows" },
    { "general", "show_hidden", OPT_BOOL, FIELD(show_hidden), "Show files whose name starts with a dot" },
    { "general", "ui_scale", OPT_DOUBLE, FIELD(ui_scale), "Scale factor for text and icons (0.5 - 3.0)", .min = 0.5, .max = 3.0 },
    { "general", "view", OPT_ENUM, FIELD(view), "Default view: grid | list", .choices = view_names },
    { "general", "icon_size", OPT_INT, FIELD(icon_size), "Icon size in the grid view, in pixels (16 - 256)", .min = 16, .max = 256 },
    { "general", "sort_by", OPT_ENUM, FIELD(sort_by), "Sort order: name | size | modified | type", .choices = sort_names },
    { "general", "dirs_first", OPT_BOOL, FIELD(dirs_first), "List directories before files" },
    { "general", "confirm_delete", OPT_BOOL, FIELD(confirm_delete), "Ask before deleting permanently" },
    { "general", "decorations", OPT_BOOL, FIELD(decorations), "Draw a window title bar (turn off on tiling compositors)" },
    { "preview", "thumbnails", OPT_BOOL, FIELD(thumbnails), "Show image and video thumbnails" },
    { "preview", "thumbnail_max_mb", OPT_INT, FIELD(thumbnail_max_mb), "Skip thumbnails for files larger than this (MiB)", .min = 1, .max = 1 << 20 },
    { "sidebar", "show_gtk_bookmarks", OPT_BOOL, FIELD(show_gtk_bookmarks), "Also list bookmarks from ~/.config/gtk-3.0/bookmarks" },
};

/* Order in which sections are written. */
static const char* const sections[] = { "general", "preview", "sidebar" };

static bool is_scalar_section(const char* name)
{
    for (size_t i = 0; i < G_N_ELEMENTS(sections); i++) {
        if (strcmp(sections[i], name) == 0)
            return true;
    }
    return false;
}

static void* field_ptr(const SfeConfig* cfg, const Option* opt)
{
    return (char*)cfg + opt->offset;
}

/* ------------------------------------------------------------------------ */
/* Construction                                                              */
/* ------------------------------------------------------------------------ */

static void user_action_free(gpointer p)
{
    SfeUserAction* a = p;
    g_free(a->name);
    g_free(a->command);
    g_free(a);
}

SfeConfig* sfe_config_new(void)
{
    SfeConfig* cfg = g_new0(SfeConfig, 1);

    cfg->start_path = g_strdup("~");
    cfg->show_hidden = false;
    cfg->ui_scale = 1.0;
    cfg->view = SFE_VIEW_GRID;
    cfg->icon_size = 64;
    cfg->sort_by = SFE_SORT_NAME;
    cfg->dirs_first = true;
    cfg->confirm_delete = true;
    cfg->decorations = true;

    cfg->thumbnails = true;
    cfg->thumbnail_max_mb = 50;

    cfg->bookmarks = g_ptr_array_new_with_free_func(g_free);
    cfg->show_gtk_bookmarks = true;

    cfg->open_with = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    cfg->actions = g_ptr_array_new_with_free_func(user_action_free);
    return cfg;
}

void sfe_config_free(SfeConfig* cfg)
{
    if (cfg == NULL)
        return;
    for (size_t i = 0; i < G_N_ELEMENTS(options); i++) {
        if (options[i].type == OPT_STRING)
            g_free(*(char**)field_ptr(cfg, &options[i]));
    }
    g_ptr_array_unref(cfg->bookmarks);
    g_hash_table_unref(cfg->open_with);
    g_ptr_array_unref(cfg->actions);
    g_free(cfg);
}

SfeConfig* sfe_config_copy(const SfeConfig* cfg)
{
    /* A serialization round trip: always in sync with the option table. */
    g_autofree char* text = sfe_config_to_string(cfg);
    SfeConfig* copy = sfe_config_parse(text, strlen(text), NULL);
    g_assert(copy != NULL);
    return copy;
}

void sfe_config_add_user_action(SfeConfig* cfg, const char* name, const char* command)
{
    SfeUserAction* a = g_new0(SfeUserAction, 1);
    a->name = g_strdup(name);
    a->command = g_strdup(command);
    g_ptr_array_add(cfg->actions, a);
}

char* sfe_config_default_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "sfe", "config.toml", NULL);
}

/* ------------------------------------------------------------------------ */
/* Reading                                                                   */
/* ------------------------------------------------------------------------ */

static const Option* find_option(const char* section, const char* key, int key_len)
{
    for (size_t i = 0; i < G_N_ELEMENTS(options); i++) {
        const Option* o = &options[i];
        if (strcmp(o->section, section) == 0 && (int)strlen(o->key) == key_len
            && memcmp(o->key, key, key_len) == 0)
            return o;
    }
    return NULL;
}

static void read_option(SfeConfig* cfg, const Option* opt, toml_datum_t v)
{
    void* field = field_ptr(cfg, opt);

    switch (opt->type) {
    case OPT_BOOL:
        if (v.type == TOML_BOOLEAN) {
            *(bool*)field = v.u.boolean;
            return;
        }
        break;
    case OPT_INT:
        if (v.type == TOML_INT64 && v.u.int64 >= opt->min && v.u.int64 <= opt->max) {
            *(int*)field = (int)v.u.int64;
            return;
        }
        break;
    case OPT_DOUBLE: {
        double d = v.type == TOML_FP64 ? v.u.fp64
            : v.type == TOML_INT64     ? (double)v.u.int64
                                       : NAN;
        if (d >= opt->min && d <= opt->max) { /* false for NaN */
            *(double*)field = d;
            return;
        }
        break;
    }
    case OPT_STRING:
        if (v.type == TOML_STRING) {
            g_free(*(char**)field);
            *(char**)field = g_strdup(v.u.s);
            return;
        }
        break;
    case OPT_ENUM:
        if (v.type == TOML_STRING) {
            for (int i = 0; opt->choices[i] != NULL; i++) {
                if (g_ascii_strcasecmp(opt->choices[i], v.u.s) == 0) {
                    *(int*)field = i;
                    return;
                }
            }
        }
        break;
    }
    g_warning("config line %d: invalid value for %s.%s, using the default",
        v.lineno, opt->section, opt->key);
}

/* Stores extensions lowercase and without a leading dot. */
static char* normalize_extension(const char* ext, int len)
{
    while (len > 0 && *ext == '.') {
        ext++;
        len--;
    }
    return g_utf8_strdown(ext, len);
}

static void read_open_table(SfeConfig* cfg, toml_datum_t tab)
{
    for (int i = 0; i < tab.u.tab.size; i++) {
        toml_datum_t v = tab.u.tab.value[i];
        if (v.type != TOML_STRING) {
            g_warning("config line %d: open.%.*s must be a string", v.lineno,
                tab.u.tab.len[i], tab.u.tab.key[i]);
            continue;
        }
        g_hash_table_replace(cfg->open_with,
            normalize_extension(tab.u.tab.key[i], tab.u.tab.len[i]), g_strdup(v.u.s));
    }
}

static void read_bookmarks(SfeConfig* cfg, toml_datum_t arr)
{
    if (arr.type != TOML_ARRAY) {
        g_warning("config line %d: sidebar.bookmarks must be an array", arr.lineno);
        return;
    }
    for (int i = 0; i < arr.u.arr.size; i++) {
        toml_datum_t v = arr.u.arr.elem[i];
        if (v.type == TOML_STRING)
            g_ptr_array_add(cfg->bookmarks, g_strdup(v.u.s));
        else
            g_warning("config line %d: bookmarks must be strings", v.lineno);
    }
}

static void read_actions(SfeConfig* cfg, toml_datum_t arr)
{
    if (arr.type != TOML_ARRAY) {
        g_warning("config line %d: use [[actions]] for custom actions", arr.lineno);
        return;
    }
    for (int i = 0; i < arr.u.arr.size; i++) {
        toml_datum_t a = arr.u.arr.elem[i];
        toml_datum_t name = toml_get(a, "name");
        toml_datum_t cmd = toml_get(a, "command");
        if (name.type != TOML_STRING || cmd.type != TOML_STRING) {
            g_warning("config line %d: an action needs a name and a command", a.lineno);
            continue;
        }
        sfe_config_add_user_action(cfg, name.u.s, cmd.u.s);
    }
}

/* Reads one scalar section, warning about keys that are not in the table. */
static void read_section(SfeConfig* cfg, const char* section, toml_datum_t tab)
{
    for (int i = 0; i < tab.u.tab.size; i++) {
        const char* key = tab.u.tab.key[i];
        int len = tab.u.tab.len[i];
        toml_datum_t v = tab.u.tab.value[i];
        const Option* opt = find_option(section, key, len);

        if (opt != NULL)
            read_option(cfg, opt, v);
        else if (strcmp(section, "sidebar") == 0 && len == 9 && memcmp(key, "bookmarks", 9) == 0)
            read_bookmarks(cfg, v);
        else
            g_warning("config line %d: unknown option %s.%.*s", v.lineno, section, len, key);
    }
}

SfeConfig* sfe_config_parse(const char* text, gsize len, GError** error)
{
    if (len > INT_MAX) {
        g_set_error(error, SFE_CONFIG_ERROR, 0, "config file is too large");
        return NULL;
    }

    toml_result_t res = toml_parse(text, (int)len);
    if (!res.ok) {
        g_set_error(error, SFE_CONFIG_ERROR, 0, "%s", res.errmsg);
        toml_free(res);
        return NULL;
    }

    SfeConfig* cfg = sfe_config_new();
    toml_datum_t top = res.toptab;

    for (int i = 0; i < top.u.tab.size; i++) {
        g_autofree char* name = g_strndup(top.u.tab.key[i], top.u.tab.len[i]);
        toml_datum_t v = top.u.tab.value[i];

        if (strcmp(name, "open") == 0 && v.type == TOML_TABLE)
            read_open_table(cfg, v);
        else if (strcmp(name, "actions") == 0)
            read_actions(cfg, v);
        else if (is_scalar_section(name) && v.type == TOML_TABLE)
            read_section(cfg, name, v);
        else
            g_warning("config line %d: unknown section or key '%s'", v.lineno, name);
    }

    toml_free(res);
    return cfg;
}

SfeConfig* sfe_config_load(const char* path, GError** error)
{
    g_autofree char* text = NULL;
    gsize len = 0;
    GError* local = NULL;

    if (!g_file_get_contents(path, &text, &len, &local)) {
        if (g_error_matches(local, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
            g_error_free(local);
            return sfe_config_new();
        }
        g_propagate_error(error, local);
        return NULL;
    }
    return sfe_config_parse(text, len, error);
}

/* ------------------------------------------------------------------------ */
/* Writing                                                                   */
/* ------------------------------------------------------------------------ */

static void append_string(GString* out, const char* s)
{
    g_string_append_c(out, '"');
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        switch (*p) {
        case '"': g_string_append(out, "\\\""); break;
        case '\\': g_string_append(out, "\\\\"); break;
        case '\n': g_string_append(out, "\\n"); break;
        case '\t': g_string_append(out, "\\t"); break;
        case '\r': g_string_append(out, "\\r"); break;
        default:
            if (*p < 0x20 || *p == 0x7f)
                g_string_append_printf(out, "\\u%04X", *p);
            else
                g_string_append_c(out, (char)*p);
        }
    }
    g_string_append_c(out, '"');
}

static void append_key(GString* out, const char* key)
{
    bool bare = *key != '\0';
    for (const char* p = key; *p; p++) {
        if (!g_ascii_isalnum(*p) && *p != '_' && *p != '-')
            bare = false;
    }
    if (bare)
        g_string_append(out, key);
    else
        append_string(out, key);
}

static void append_double(GString* out, double d)
{
    char buf[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(buf, sizeof(buf), "%g", d);
    g_string_append(out, buf);
    if (strpbrk(buf, ".eE") == NULL)
        g_string_append(out, ".0");
}

static void append_option(GString* out, const SfeConfig* cfg, const Option* opt)
{
    const void* field = field_ptr(cfg, opt);

    g_string_append_printf(out, "# %s\n%s = ", opt->help, opt->key);
    switch (opt->type) {
    case OPT_BOOL: g_string_append(out, *(const bool*)field ? "true" : "false"); break;
    case OPT_INT: g_string_append_printf(out, "%d", *(const int*)field); break;
    case OPT_DOUBLE: append_double(out, *(const double*)field); break;
    case OPT_STRING: append_string(out, *(char* const*)field ? *(char* const*)field : ""); break;
    case OPT_ENUM: append_string(out, opt->choices[*(const int*)field]); break;
    }
    g_string_append(out, "\n\n");
}

static void append_bookmarks(GString* out, const SfeConfig* cfg)
{
    g_string_append(out, "# Directories pinned to the sidebar\nbookmarks = [");
    for (guint i = 0; i < cfg->bookmarks->len; i++) {
        g_string_append(out, "\n    ");
        append_string(out, g_ptr_array_index(cfg->bookmarks, i));
        g_string_append_c(out, ',');
    }
    g_string_append(out, cfg->bookmarks->len ? "\n]\n\n" : "]\n\n");
}

static int compare_strings(const void* a, const void* b)
{
    return strcmp(*(char* const*)a, *(char* const*)b);
}

static void append_open_table(GString* out, const SfeConfig* cfg)
{
    g_string_append(out,
        "[open]\n"
        "# Per-extension overrides of xdg-open; %f is replaced by the file path.\n"
        "# mp4 = \"mpv %f\"\n");

    guint n = 0;
    g_autofree gpointer* keys = g_hash_table_get_keys_as_array(cfg->open_with, &n);
    qsort(keys, n, sizeof(char*), compare_strings);
    for (guint i = 0; i < n; i++) {
        append_key(out, keys[i]);
        g_string_append(out, " = ");
        append_string(out, g_hash_table_lookup(cfg->open_with, keys[i]));
        g_string_append_c(out, '\n');
    }
    g_string_append_c(out, '\n');
}

static void append_actions(GString* out, const SfeConfig* cfg)
{
    g_string_append(out,
        "# Custom context menu commands; %f = selected files, %d = current directory.\n"
        "# [[actions]]\n"
        "# name = \"Open terminal here\"\n"
        "# command = \"foot -D %d\"\n");

    for (guint i = 0; i < cfg->actions->len; i++) {
        const SfeUserAction* a = g_ptr_array_index(cfg->actions, i);
        g_string_append(out, "\n[[actions]]\nname = ");
        append_string(out, a->name);
        g_string_append(out, "\ncommand = ");
        append_string(out, a->command);
        g_string_append_c(out, '\n');
    }
}

char* sfe_config_to_string(const SfeConfig* cfg)
{
    GString* out = g_string_new("# sfe configuration. Changes are applied while sfe is running.\n\n");

    for (size_t s = 0; s < G_N_ELEMENTS(sections); s++) {
        g_string_append_printf(out, "[%s]\n", sections[s]);
        for (size_t i = 0; i < G_N_ELEMENTS(options); i++) {
            if (strcmp(options[i].section, sections[s]) == 0)
                append_option(out, cfg, &options[i]);
        }
        if (strcmp(sections[s], "sidebar") == 0)
            append_bookmarks(out, cfg);
    }
    append_open_table(out, cfg);
    append_actions(out, cfg);
    return g_string_free(out, FALSE);
}

bool sfe_config_save(const SfeConfig* cfg, const char* path, GError** error)
{
    g_autofree char* dir = g_path_get_dirname(path);
    g_autofree char* text = sfe_config_to_string(cfg);

    if (g_mkdir_with_parents(dir, 0700) != 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
            "cannot create %s: %s", dir, g_strerror(errno));
        return false;
    }
    return g_file_set_contents_full(path, text, -1,
        G_FILE_SET_CONTENTS_CONSISTENT, 0644, error);
}
