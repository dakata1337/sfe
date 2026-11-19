#pragma once

/* Normal-mode key bindings: key sequences mapped to command lines.
 *
 *   "j"        -> "cursor down"
 *   "gg"       -> "cursor first"
 *   "<C-w>v"   -> "split vertical"
 *   "<C-H>"    -> "split move left"     (Ctrl+Shift+h)
 *
 * A sequence is a string of keys, vim style: a plain character stands for
 * itself; <...> holds modifiers C- (Ctrl), A- (Alt), S- (Shift) and a key,
 * which is a character or a name (Space, CR, Esc, BS, Tab, Up, Down, Left,
 * Right, Home, End, Del, F1..F12). Shift with a letter is the capital letter
 * ("<S-h>" is "H"); with other characters it is implied by the character.
 *
 * Internally every key is a canonical token ("j", "H", "C-w", "C-H",
 * "Space", "S-Tab"), the form the window produces from key events too.
 * GTK-free and unit-tested; config [keys] overrides the defaults. */

#include <glib.h>
#include <stdbool.h>

typedef struct SfeKeymap SfeKeymap;

/* A default binding, for the user guide and Preferences. */
typedef struct {
    const char* sequence;
    const char* command;
    const char* help;
} SfeKeyBinding;

/* The built-in bindings (n receives the count). */
const SfeKeyBinding* sfe_keymap_defaults(size_t* n);
/* The built-in visual-mode bindings, looked up before the ones above while
 * a view is in visual mode ("y" yanks at once there instead of "yy"). */
const SfeKeyBinding* sfe_keymap_visual_defaults(size_t* n);

/* An empty keymap, or one with the defaults. */
SfeKeymap* sfe_keymap_new(void);
SfeKeymap* sfe_keymap_new_default(void);
SfeKeymap* sfe_keymap_new_visual_default(void);
void sfe_keymap_free(SfeKeymap* map);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(SfeKeymap, sfe_keymap_free)

/* Binds `sequence` to `command`, replacing an earlier binding of the same
 * sequence; an empty command removes the binding. */
bool sfe_keymap_bind(SfeKeymap* map, const char* sequence, const char* command, GError** error);

/* Splits a sequence into canonical tokens (NULL-terminated); NULL with an
 * error when it cannot be parsed. */
char** sfe_keymap_parse(const char* sequence, GError** error);

/* The canonical token for one key: `name` is a character ("h", "H", ":")
 * or a key name ("Space", "Tab"); `ctrl` / `alt` / `shift` are the held
 * modifiers. Returns NULL for keys that are only modifiers. */
char* sfe_keymap_token(const char* name, bool ctrl, bool alt, bool shift);

/* Tokens back as a sequence for display: { "C-w", "v" } -> "<C-w>v". */
char* sfe_keymap_format(char* const* tokens, guint n);

typedef enum {
    SFE_KEYS_NONE, /* nothing starts with these keys */
    SFE_KEYS_PREFIX, /* keep reading keys */
    SFE_KEYS_MATCH, /* `command` is bound to exactly these keys */
} SfeKeysResult;

/* Looks up the keys typed so far. With MATCH, `prefix` says whether a longer
 * binding also starts with them (then the caller waits a moment for more
 * keys before running `command`). */
SfeKeysResult sfe_keymap_lookup(const SfeKeymap* map, char* const* tokens, guint n, const char** command,
    bool* prefix);

/* Two maps at once (visual mode: the visual keys, then the normal ones): an
 * exact binding of `first` wins over one of `then`, but a longer binding in
 * either still makes the caller wait, so a visual "d" does not swallow the
 * first key of the normal "dd". `first` may be NULL. */
SfeKeysResult sfe_keymap_lookup_layered(const SfeKeymap* first, const SfeKeymap* then, char* const* tokens,
    guint n, const char** command, bool* prefix);

/* A key that can follow a partly typed chord, for the suggestions list. */
typedef struct {
    char* keys; /* the rest of the sequence: "v", "<C-x>q" */
    char* command;
    const char* help; /* of the built-in binding with that command, or NULL */
} SfeKeyHint;

/* The bindings that continue `tokens` (SfeKeyHint*, sorted by keys) appended
 * to `hints` (a new array when NULL); keys already in `hints` are skipped,
 * so a map looked up first wins. */
GPtrArray* sfe_keymap_continuations(const SfeKeymap* map, char* const* tokens, guint n, GPtrArray* hints);
