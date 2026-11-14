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

/* What a custom command applies to. */
typedef enum {
    SFE_ACTION_FILES = 1 << 0, /* [[actions.files]] */
    SFE_ACTION_DIRECTORIES = 1 << 1, /* [[actions.directories]]; also "nothing selected" */
    SFE_ACTION_ANY = SFE_ACTION_FILES | SFE_ACTION_DIRECTORIES, /* old-style [[actions]] */
} SfeActionKind;

/* A user-defined command shown in the context menu. The same name may be
 * defined once for files and once for directories with different commands. */
typedef struct {
    char* name;
    char* command; /* %f = selected files, %d = current directory */
    unsigned applies_to; /* SFE_ACTION_* */
} SfeUserAction;

typedef struct {
    /* [general] */
    char* start_path; /* may start with "~" */
    bool show_hidden;
    double ui_scale;
    int view; /* SfeViewMode */
    int icon_size;
    int sort_by; /* SfeSortKey */
    bool dirs_first;
    bool confirm_delete;
    bool decorations;

    /* [preview] */
    bool thumbnails;
    int thumbnail_max_mb;

    /* [sidebar] */
    GPtrArray* bookmarks; /* char*, may start with "~" */
    bool show_gtk_bookmarks;

    /* [open]: extension (lowercase, no dot) -> command with %f */
    GHashTable* open_with;

    /* [[actions]] */
    GPtrArray* actions; /* SfeUserAction* */
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

void sfe_config_add_user_action(SfeConfig* cfg, const char* name, const char* command, unsigned applies_to);
