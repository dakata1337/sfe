#include "config.h"

#include "keymap.h"
#include <errno.h>
#include <glib/gstdio.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <tomlc17.h>

G_DEFINE_QUARK(sfe-config-error-quark, sfe_config_error)

/* ------------------------------------------------------------------------ */
/* Option table: one row per scalar option.                                  */
/* ------------------------------------------------------------------------ */

static const char* const view_names[] = { "grid", "list", NULL };
static const char* const sort_names[] = { "name", "size", "modified", "type", NULL };
static const char* const order_names[] = { "ascending", "descending", NULL };
static const char* const file_icon_names[] = { "system", "tinted", "symbolic", NULL };
static const char* const device_names[] = { "all", "removable", NULL };

#define FIELD(f) offsetof(SfeConfig, f)

bool sfe_sort_key_from_name(const char* name, SfeSortKey* key)
{
    for (int i = 0; name != NULL && sort_names[i] != NULL; i++) {
        if (g_ascii_strcasecmp(name, sort_names[i]) == 0) {
            *key = (SfeSortKey)i;
            return true;
        }
    }
    return false;
}

bool sfe_sort_order_from_name(const char* name, SfeSortOrder* order)
{
    if (name == NULL)
        return false;
    if (g_ascii_strcasecmp(name, "ascending") == 0 || g_ascii_strcasecmp(name, "asc") == 0)
        *order = SFE_SORT_ASCENDING;
    else if (g_ascii_strcasecmp(name, "descending") == 0 || g_ascii_strcasecmp(name, "desc") == 0)
        *order = SFE_SORT_DESCENDING;
    else
        return false;
    return true;
}

const char* sfe_sort_key_name(SfeSortKey key)
{
    return key >= SFE_SORT_NAME && key <= SFE_SORT_TYPE ? sort_names[key] : "name";
}

const char* sfe_sort_order_name(SfeSortOrder order)
{
    return order == SFE_SORT_DESCENDING ? "descending" : "ascending";
}

static const SfeOption options[] = {
    { "general", "start_path", SFE_OPT_STRING, FIELD(start_path), "Directory opened by new windows", .label = "Start folder" },
    { "general", "show_hidden", SFE_OPT_BOOL, FIELD(show_hidden), "Show files whose name starts with a dot", .label = "Show hidden files" },
    { "general", "ui_scale", SFE_OPT_DOUBLE, FIELD(ui_scale), "Scale factor for text and icons (0.5 - 3.0)", .min = 0.5, .max = 3.0, .label = "Scale" },
    { "general", "view", SFE_OPT_ENUM, FIELD(view), "Default view: grid | list", .choices = view_names, .label = "Default view" },
    { "general", "icon_size", SFE_OPT_INT, FIELD(icon_size), "Icon size in the grid view, in pixels (16 - 256)", .min = 16, .max = 256, .label = "Icon size" },
    { "general", "file_icons", SFE_OPT_ENUM, FIELD(file_icons), "File icons: system (the icon theme as it is) | tinted (folders in the theme's accent colour) | symbolic (one-colour icons in the theme's colours)", .choices = file_icon_names, .label = "File icons" },
    { "general", "sort_by", SFE_OPT_ENUM, FIELD(sort_by), "Sort by: name | size | modified | type", .choices = sort_names, .label = "Sort by" },
    { "general", "sort_order", SFE_OPT_ENUM, FIELD(sort_order), "Sort direction: ascending (A-Z, smallest, oldest first) | descending", .choices = order_names, .label = "Sort order" },
    { "general", "dirs_first", SFE_OPT_BOOL, FIELD(dirs_first), "List directories before files", .label = "Folders first" },
    { "general", "confirm_delete", SFE_OPT_BOOL, FIELD(confirm_delete), "Ask before deleting permanently", .label = "Confirm permanent delete" },
    { "general", "decorations", SFE_OPT_BOOL, FIELD(decorations), "Draw a window title bar (turn off on tiling compositors)", .label = "Window title bar" },
    { "preview", "thumbnails", SFE_OPT_BOOL, FIELD(thumbnails), "Show image and video thumbnails", .label = "Thumbnails" },
    { "preview", "thumbnail_max_mb", SFE_OPT_INT, FIELD(thumbnail_max_mb), "Skip thumbnails for files larger than this (MiB)", .min = 1, .max = 1 << 20, .label = "Thumbnail size limit (MiB)" },
    { "sidebar", "devices", SFE_OPT_ENUM, FIELD(devices), "Devices listed: all (removable drives and disks) | removable (only what can be unplugged: flash drives, SD cards)", .choices = device_names, .label = "Devices" },
    { "sidebar", "show_gtk_bookmarks", SFE_OPT_BOOL, FIELD(show_gtk_bookmarks), "Also list bookmarks from ~/.config/gtk-3.0/bookmarks", .label = "Show GTK bookmarks" },
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

static void* field_ptr(const SfeConfig* cfg, const SfeOption* opt)
{
    return (char*)cfg + opt->offset;
}

const SfeOption* sfe_config_options(size_t* n)
{
    *n = G_N_ELEMENTS(options);
    return options;
}

void* sfe_config_field(const SfeConfig* cfg, const SfeOption* opt)
{
    return field_ptr(cfg, opt);
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

static void key_override_free(gpointer p)
{
    SfeKeyOverride* k = p;
    g_free(k->sequence);
    g_free(k->command);
    g_free(k);
}

void sfe_config_add_key(SfeConfig* cfg, const char* sequence, const char* command, bool visual)
{
    for (guint i = 0; i < cfg->keys->len; i++) { /* the last one wins */
        SfeKeyOverride* k = g_ptr_array_index(cfg->keys, i);
        if (strcmp(k->sequence, sequence) == 0 && k->visual == visual) {
            g_ptr_array_remove_index(cfg->keys, i);
            break;
        }
    }
    SfeKeyOverride* k = g_new0(SfeKeyOverride, 1);
    k->sequence = g_strdup(sequence);
    k->command = g_strdup(command);
    k->visual = visual;
    g_ptr_array_add(cfg->keys, k);
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
    cfg->sort_order = SFE_SORT_ASCENDING;
    cfg->dirs_first = true;
    cfg->confirm_delete = true;
    cfg->decorations = true;

    cfg->thumbnails = true;
    cfg->thumbnail_max_mb = 50;

    cfg->bookmarks = g_ptr_array_new_with_free_func(g_free);
    cfg->show_gtk_bookmarks = true;

    cfg->open_with = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    cfg->actions = g_ptr_array_new_with_free_func(user_action_free);
    cfg->keys = g_ptr_array_new_with_free_func(key_override_free);
    cfg->problems = g_ptr_array_new_with_free_func(g_free);
    return cfg;
}

void sfe_config_free(SfeConfig* cfg)
{
    if (cfg == NULL)
        return;
    for (size_t i = 0; i < G_N_ELEMENTS(options); i++) {
        if (options[i].type == SFE_OPT_STRING)
            g_free(*(char**)field_ptr(cfg, &options[i]));
    }
    g_ptr_array_unref(cfg->bookmarks);
    g_hash_table_unref(cfg->open_with);
    g_ptr_array_unref(cfg->actions);
    g_ptr_array_unref(cfg->keys);
    g_ptr_array_unref(cfg->problems);
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

void sfe_config_add_user_action(SfeConfig* cfg, const char* name, const char* command, unsigned applies_to)
{
    SfeUserAction* a = g_new0(SfeUserAction, 1);
    a->name = g_strdup(name);
    a->command = g_strdup(command);
    a->applies_to = applies_to;
    g_ptr_array_add(cfg->actions, a);
}

char* sfe_config_default_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "sfe", "config.toml", NULL);
}

/* ------------------------------------------------------------------------ */
/* Reading                                                                   */
/* ------------------------------------------------------------------------ */

static const SfeOption* find_option(const char* section, const char* key, int key_len)
{
    for (size_t i = 0; i < G_N_ELEMENTS(options); i++) {
        const SfeOption* o = &options[i];
        if (strcmp(o->section, section) == 0 && (int)strlen(o->key) == key_len
            && memcmp(o->key, key, key_len) == 0)
            return o;
    }
    return NULL;
}

/* Records something the parser skipped (and logs it). */
G_GNUC_PRINTF(2, 3)
static void problem(SfeConfig* cfg, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    char* text = g_strdup_vprintf(format, args);
    va_end(args);
    g_warning("config %s", text);
    g_ptr_array_add(cfg->problems, text);
}

static void read_option(SfeConfig* cfg, const SfeOption* opt, toml_datum_t v)
{
    void* field = field_ptr(cfg, opt);

    switch (opt->type) {
    case SFE_OPT_BOOL:
        if (v.type == TOML_BOOLEAN) {
            *(bool*)field = v.u.boolean;
            return;
        }
        break;
    case SFE_OPT_INT:
        if (v.type == TOML_INT64 && v.u.int64 >= opt->min && v.u.int64 <= opt->max) {
            *(int*)field = (int)v.u.int64;
            return;
        }
        break;
    case SFE_OPT_DOUBLE: {
        double d = v.type == TOML_FP64 ? v.u.fp64
            : v.type == TOML_INT64     ? (double)v.u.int64
                                       : NAN;
        if (d >= opt->min && d <= opt->max) { /* false for NaN */
            *(double*)field = d;
            return;
        }
        break;
    }
    case SFE_OPT_STRING:
        if (v.type == TOML_STRING) {
            g_free(*(char**)field);
            *(char**)field = g_strdup(v.u.s);
            return;
        }
        break;
    case SFE_OPT_ENUM:
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
    problem(cfg, "line %d: invalid value for %s.%s, using the default",
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
            problem(cfg, "line %d: open.%.*s must be a string", v.lineno,
                tab.u.tab.len[i], tab.u.tab.key[i]);
            continue;
        }
        g_hash_table_replace(cfg->open_with,
            normalize_extension(tab.u.tab.key[i], tab.u.tab.len[i]), g_strdup(v.u.s));
    }
}

/* [keys], and [keys.visual] inside it (`visual`). */
static void read_keys(SfeConfig* cfg, toml_datum_t tab, bool visual)
{
    const char* section = visual ? "keys.visual" : "keys";
    for (int i = 0; i < tab.u.tab.size; i++) {
        toml_datum_t v = tab.u.tab.value[i];
        g_autofree char* sequence = g_strndup(tab.u.tab.key[i], tab.u.tab.len[i]);
        if (!visual && v.type == TOML_TABLE && strcmp(sequence, "visual") == 0) {
            read_keys(cfg, v, true);
            continue;
        }
        if (v.type != TOML_STRING) {
            problem(cfg, "line %d: %s.\"%s\" must be a command (a string)", v.lineno, section, sequence);
            continue;
        }
        g_autoptr(GError) error = NULL;
        g_auto(GStrv) tokens = sfe_keymap_parse(sequence, &error);
        if (tokens == NULL) {
            problem(cfg, "line %d: %s: %s", v.lineno, section, error->message);
            continue;
        }
        sfe_config_add_key(cfg, sequence, v.u.s, visual);
    }
}

static void read_bookmarks(SfeConfig* cfg, toml_datum_t arr)
{
    if (arr.type != TOML_ARRAY) {
        problem(cfg, "line %d: sidebar.bookmarks must be an array", arr.lineno);
        return;
    }
    for (int i = 0; i < arr.u.arr.size; i++) {
        toml_datum_t v = arr.u.arr.elem[i];
        if (v.type == TOML_STRING)
            g_ptr_array_add(cfg->bookmarks, g_strdup(v.u.s));
        else
            problem(cfg, "line %d: bookmarks must be strings", v.lineno);
    }
}

static void read_action_list(SfeConfig* cfg, toml_datum_t arr, unsigned applies_to)
{
    if (arr.type != TOML_ARRAY) {
        problem(cfg, "line %d: use [[actions.files]] / [[actions.directories]]", arr.lineno);
        return;
    }
    for (int i = 0; i < arr.u.arr.size; i++) {
        toml_datum_t a = arr.u.arr.elem[i];
        toml_datum_t name = toml_get(a, "name");
        toml_datum_t cmd = toml_get(a, "command");
        if (name.type != TOML_STRING || cmd.type != TOML_STRING) {
            problem(cfg, "line %d: an action needs a name and a command", a.lineno);
            continue;
        }
        sfe_config_add_user_action(cfg, name.u.s, cmd.u.s, applies_to);
    }
}

/* [[actions.files]] and [[actions.directories]]; the older [[actions]]
 * (a plain array) applies to both. */
static void read_actions(SfeConfig* cfg, toml_datum_t value)
{
    if (value.type == TOML_ARRAY) {
        read_action_list(cfg, value, SFE_ACTION_ANY);
        return;
    }
    if (value.type != TOML_TABLE) {
        problem(cfg, "line %d: use [[actions.files]] / [[actions.directories]]", value.lineno);
        return;
    }
    for (int i = 0; i < value.u.tab.size; i++) {
        g_autofree char* key = g_strndup(value.u.tab.key[i], value.u.tab.len[i]);
        if (strcmp(key, "files") == 0)
            read_action_list(cfg, value.u.tab.value[i], SFE_ACTION_FILES);
        else if (strcmp(key, "directories") == 0)
            read_action_list(cfg, value.u.tab.value[i], SFE_ACTION_DIRECTORIES);
        else
            problem(cfg, "line %d: unknown action group actions.%s", value.u.tab.value[i].lineno, key);
    }
}

/* Reads one scalar section, warning about keys that are not in the table. */
static void read_section(SfeConfig* cfg, const char* section, toml_datum_t tab)
{
    for (int i = 0; i < tab.u.tab.size; i++) {
        const char* key = tab.u.tab.key[i];
        int len = tab.u.tab.len[i];
        toml_datum_t v = tab.u.tab.value[i];
        const SfeOption* opt = find_option(section, key, len);

        if (opt != NULL)
            read_option(cfg, opt, v);
        else if (strcmp(section, "sidebar") == 0 && len == 9 && memcmp(key, "bookmarks", 9) == 0)
            read_bookmarks(cfg, v);
        else
            problem(cfg, "line %d: unknown option %s.%.*s", v.lineno, section, len, key);
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
        else if (strcmp(name, "keys") == 0 && v.type == TOML_TABLE)
            read_keys(cfg, v, false);
        else if (is_scalar_section(name) && v.type == TOML_TABLE)
            read_section(cfg, name, v);
        else
            problem(cfg, "line %d: unknown section or key '%s'", v.lineno, name);
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

static void append_option(GString* out, const SfeConfig* cfg, const SfeOption* opt)
{
    const void* field = field_ptr(cfg, opt);

    g_string_append_printf(out, "# %s\n%s = ", opt->help, opt->key);
    switch (opt->type) {
    case SFE_OPT_BOOL: g_string_append(out, *(const bool*)field ? "true" : "false"); break;
    case SFE_OPT_INT: g_string_append_printf(out, "%d", *(const int*)field); break;
    case SFE_OPT_DOUBLE: append_double(out, *(const double*)field); break;
    case SFE_OPT_STRING: append_string(out, *(char* const*)field ? *(char* const*)field : ""); break;
    case SFE_OPT_ENUM: append_string(out, opt->choices[*(const int*)field]); break;
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

static void append_keys(GString* out, const SfeConfig* cfg)
{
    g_string_append(out,
        "[keys]\n"
        "# Normal-mode keys (see the user guide): a key sequence = a command line.\n"
        "# An empty command removes a default binding.\n"
        "# \"<C-w>v\" = \"split vertical\"\n"
        "# \"x\" = \"\"\n");
    bool any_visual = false;
    for (int visual = 0; visual <= 1; visual++) {
        if (visual && any_visual)
            g_string_append(out, "\n[keys.visual]\n# Visual mode, looked up before [keys] while selecting with v.\n");
        for (guint i = 0; i < cfg->keys->len; i++) {
            const SfeKeyOverride* k = g_ptr_array_index(cfg->keys, i);
            any_visual |= k->visual;
            if (k->visual != visual)
                continue;
            append_string(out, k->sequence);
            g_string_append(out, " = ");
            append_string(out, k->command);
            g_string_append_c(out, '\n');
        }
    }
    g_string_append_c(out, '\n');
}

static void append_action_group(GString* out, const SfeConfig* cfg, unsigned kind, const char* table)
{
    for (guint i = 0; i < cfg->actions->len; i++) {
        const SfeUserAction* a = g_ptr_array_index(cfg->actions, i);
        if (!(a->applies_to & kind))
            continue;
        g_string_append_printf(out, "\n[[actions.%s]]\nname = ", table);
        append_string(out, a->name);
        g_string_append(out, "\ncommand = ");
        append_string(out, a->command);
        g_string_append_c(out, '\n');
    }
}

static void append_actions(GString* out, const SfeConfig* cfg)
{
    g_string_append(out,
        "# Custom context menu commands. %f = the selected items (the current\n"
        "# folder when nothing is selected), %d = the current folder. The same\n"
        "# name can have one command for files and another for directories.\n"
        "# [[actions.directories]]\n"
        "# name = \"Open terminal here\"\n"
        "# command = \"foot -D %f\"\n");
    /* Old-style entries (both kinds) are written to both groups. */
    append_action_group(out, cfg, SFE_ACTION_FILES, "files");
    append_action_group(out, cfg, SFE_ACTION_DIRECTORIES, "directories");
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
    append_keys(out, cfg);
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
