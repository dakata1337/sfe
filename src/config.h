#pragma once

/* User configuration, stored as TOML in ~/.config/sfe/config.toml.
 *
 * Adding a new scalar option:
 *   1. add a field to SfeConfig,
 *   2. set its default in sfe_config_new(),
 *   3. add one row to the option table in config.c.
 * Loading, saving and validation are then handled automatically. */

#include <glib.h>
#include <stdbool.h>

typedef enum {
    SFE_VIEW_GRID,
    SFE_VIEW_LIST,
} SfeViewMode;

typedef enum {
    SFE_SORT_NAME,
    SFE_SORT_SIZE,
    SFE_SORT_MODIFIED,
    SFE_SORT_TYPE,
} SfeSortKey;

typedef enum {
    SFE_SORT_ASCENDING, /* A → Z, smallest, oldest first */
    SFE_SORT_DESCENDING,
} SfeSortOrder;

/* Sort keys and orders by name, as in the config and `:sort`: "name",
 * "size", "modified", "type"; "ascending" / "descending" (or "asc" /
 * "desc"). Case-insensitive; false for anything else. */
bool sfe_sort_key_from_name(const char* name, SfeSortKey* key);
bool sfe_sort_order_from_name(const char* name, SfeSortOrder* order);
const char* sfe_sort_key_name(SfeSortKey key);
const char* sfe_sort_order_name(SfeSortOrder order);

/* What a custom command applies to. */
typedef enum {
    SFE_ACTION_FILES = 1 << 0, /* [[actions.files]] */
    SFE_ACTION_DIRECTORIES = 1 << 1, /* [[actions.directories]]; also "nothing selected" */
    SFE_ACTION_ANY = SFE_ACTION_FILES | SFE_ACTION_DIRECTORIES, /* old-style [[actions]] */
} SfeActionKind;

/* A normal-mode key binding from [keys] ("<C-w>v" = "split vertical"); an
 * empty command removes a default binding. */
typedef struct {
    char* sequence;
    char* command;
    bool visual; /* from [keys.visual] */
} SfeKeyOverride;

/* A user-defined command shown in the context menu. The same name may be
 * defined once for files and once for directories with different commands. */
typedef struct {
    char* name;
    char* command; /* %f = selected files, %d = current directory */
    unsigned applies_to; /* SFE_ACTION_* */
} SfeUserAction;

/* Which devices the sidebar lists ([sidebar] devices). */
typedef enum {
    SFE_DEVICES_ALL, /* removable drives and internal disks */
    SFE_DEVICES_REMOVABLE, /* only what can be unplugged */
} SfeDeviceFilter;

/* How file icons are drawn ([general] file_icons). */
typedef enum {
    SFE_FILE_ICONS_SYSTEM, /* the icon theme's own colours */
    SFE_FILE_ICONS_TINTED, /* folders recoloured to the theme's accent */
    SFE_FILE_ICONS_SYMBOLIC, /* one-colour icons: text colour, folders accent */
} SfeFileIcons;

typedef struct {
    /* [general] */
    char* start_path; /* may start with "~" */
    bool show_hidden;
    double ui_scale;
    int view; /* SfeViewMode */
    int file_icons; /* SfeFileIcons */
    int icon_size;
    int sort_by; /* SfeSortKey */
    int sort_order; /* SfeSortOrder */
    bool dirs_first;
    bool confirm_delete;
    bool decorations;

    /* [preview] */
    bool thumbnails;
    int thumbnail_max_mb;
    bool folder_covers; /* a folder's cover.jpg etc. instead of its icon */

    /* [sidebar] */
    GPtrArray* bookmarks; /* char*, may start with "~" */
    bool show_gtk_bookmarks;
    int devices; /* SfeDeviceFilter */

    /* [open]: extension (lowercase, no dot) -> command with %f */
    GHashTable* open_with;

    /* [[actions]] */
    GPtrArray* actions; /* SfeUserAction* */

    /* [keys], in file order */
    GPtrArray* keys; /* SfeKeyOverride* */

    /* What parsing skipped, "line 3: unknown option general.foo" (char*). */
    GPtrArray* problems;
} SfeConfig;

/* ------------------------------------------------------------------------ */
/* The option table (scalar options), for generic code such as Preferences */

typedef enum {
    SFE_OPT_BOOL, /* bool */
    SFE_OPT_INT, /* int, between min and max */
    SFE_OPT_DOUBLE, /* double, between min and max */
    SFE_OPT_STRING, /* char* */
    SFE_OPT_ENUM, /* int: index into choices */
} SfeOptionType;

typedef struct {
    const char* section;
    const char* key;
    SfeOptionType type;
    size_t offset; /* offsetof(SfeConfig, field) */
    const char* help; /* written as a comment above the key; tooltip in Preferences */
    const char* label; /* short name in Preferences */
    double min, max; /* SFE_OPT_INT, SFE_OPT_DOUBLE */
    const char* const* choices; /* SFE_OPT_ENUM: NULL-terminated, index == value */
    const char* group; /* a heading in Preferences over this and the next options, or NULL */
} SfeOption;

/* Every scalar option (n receives the count), in file order. */
const SfeOption* sfe_config_options(size_t* n);

/* Where `opt` lives in `cfg` (cast to bool*, int*, double* or char**). */
void* sfe_config_field(const SfeConfig* cfg, const SfeOption* opt);

#define SFE_CONFIG_ERROR (sfe_config_error_quark())
GQuark sfe_config_error_quark(void);

/* A config with every option at its default value. */
SfeConfig* sfe_config_new(void);
void sfe_config_free(SfeConfig* cfg);
SfeConfig* sfe_config_copy(const SfeConfig* cfg);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(SfeConfig, sfe_config_free)

/* Default location: $XDG_CONFIG_HOME/sfe/config.toml. Newly allocated. */
char* sfe_config_default_path(void);

/* Parses TOML text. Unknown keys and invalid values are reported with
 * g_warning() and fall back to the default; only a syntax error fails. */
SfeConfig* sfe_config_parse(const char* text, gsize len, GError** error);

/* Loads `path`. A missing file yields the defaults (not an error). */
SfeConfig* sfe_config_load(const char* path, GError** error);

/* Serializes to TOML text (newly allocated). */
char* sfe_config_to_string(const SfeConfig* cfg);

/* Writes atomically, creating parent directories as needed. */
bool sfe_config_save(const SfeConfig* cfg, const char* path, GError** error);

void sfe_config_add_key(SfeConfig* cfg, const char* sequence, const char* command, bool visual);

void sfe_config_add_user_action(SfeConfig* cfg, const char* name, const char* command, unsigned applies_to);
