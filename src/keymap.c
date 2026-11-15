#include "keymap.h"

#include <string.h>

#define SFE_KEYMAP_ERROR (g_quark_from_static_string("sfe-keymap-error"))

/* The built-in normal-mode keys (PLAN.md section 7). */
static const SfeKeyBinding defaults[] = {
    { "j", "cursor down", "cursor down" },
    { "k", "cursor up", "cursor up" },
    { "h", "cd ..", "parent folder" },
    { "<BS>", "cd ..", "parent folder" },
    { "l", "open", "open the folder or file" },
    { "gg", "cursor first", "first item" },
    { "G", "cursor last", "last item" },
    { "gh", "cd ~", "home folder" },
    { "<C-o>", "back", "back in history" },
    { "<C-i>", "forward", "forward in history" },
    { "<Space>", "select toggle", "select / unselect the item and move down" },
    { "yy", "copy_clipboard", "copy the selection to the clipboard" },
    { "dd", "cut", "cut the selection" },
    { "p", "paste", "paste into this view" },
    { "x", "trash", "move to trash" },
    { "X", "delete", "delete permanently (asks)" },
    { "r", "rename", "rename" },
    { "R", "mass_rename", "rename with a pattern" },
    { "zh", "hidden toggle", "show / hide hidden files" },
    { "v", "visual", "visual mode (select chunks with j / k)" },
    { "/", "search", "fuzzy search" },
    { ":", "command_line", "command line" },
    { "<C-w>v", "split vertical", "split side by side" },
    { "<C-w>s", "split horizontal", "split top / bottom" },
    { "<C-w>q", "split close", "close the view (the window if it is the last)" },
    { "W", "split next", "next view: left, right-top, right-bottom" },
    { "H", "split left", "focus the view on the left" },
    { "J", "split down", "focus the view below" },
    { "K", "split up", "focus the view above" },
    { "L", "split right", "focus the view on the right" },
    { "<C-H>", "split move left", "swap with the view on the left" },
    { "<C-J>", "split move down", "swap with the view below" },
    { "<C-K>", "split move up", "swap with the view above" },
    { "<C-L>", "split move right", "swap with the view on the right" },
};

/* Visual mode: these first, then the keys above. */
static const SfeKeyBinding visual_defaults[] = {
    { "v", "visual", "end the chunk / start a new one at the cursor" },
    { "o", "visual swap", "jump to the other end of the chunk" },
    { "h", "cursor previous", "the item before (in a grid: to the left)" },
    { "l", "cursor next", "the item after (in a grid: to the right)" },
    { "<Esc>", "visual off", "leave visual mode (the selection stays)" },
    { "y", "copy_clipboard", "copy the selection to the clipboard" },
    { "d", "cut", "cut the selection" },
};

const SfeKeyBinding* sfe_keymap_defaults(size_t* n)
{
    *n = G_N_ELEMENTS(defaults);
    return defaults;
}

const SfeKeyBinding* sfe_keymap_visual_defaults(size_t* n)
{
    *n = G_N_ELEMENTS(visual_defaults);
    return visual_defaults;
}

/* ------------------------------------------------------------------------ */
/* Tokens                                                                    */
/* ------------------------------------------------------------------------ */

/* Key names as written in sequences (any case) -> canonical names. */
static const struct {
    const char* alias;
    const char* name;
} key_names[] = {
    { "space", "Space" },
    { "cr", "CR" },
    { "enter", "CR" },
    { "return", "CR" },
    { "esc", "Esc" },
    { "escape", "Esc" },
    { "bs", "BS" },
    { "backspace", "BS" },
    { "tab", "Tab" },
    { "up", "Up" },
    { "down", "Down" },
    { "left", "Left" },
    { "right", "Right" },
    { "home", "Home" },
    { "end", "End" },
    { "del", "Del" },
    { "delete", "Del" },
    { "pageup", "PageUp" },
    { "pagedown", "PageDown" },
    { "lt", "<" }, /* "<lt>" is a literal "<" */
};

static const char* canonical_name(const char* name)
{
    for (size_t i = 0; i < G_N_ELEMENTS(key_names); i++) {
        if (g_ascii_strcasecmp(name, key_names[i].alias) == 0)
            return key_names[i].name;
    }
    /* F1 .. F12 */
    if ((name[0] == 'f' || name[0] == 'F') && name[1] != '\0') {
        char* end = NULL;
        long n = strtol(name + 1, &end, 10);
        if (*end == '\0' && n >= 1 && n <= 24) {
            static char buf[8];
            g_snprintf(buf, sizeof buf, "F%ld", n);
            return g_intern_string(buf);
        }
    }
    return NULL;
}

static bool is_one_char(const char* s)
{
    return *s != '\0' && *g_utf8_next_char(s) == '\0';
}

char* sfe_keymap_token(const char* name, bool ctrl, bool alt, bool shift)
{
    g_autofree char* key = NULL;

    if (name == NULL || *name == '\0')
        return NULL;
    if (strcmp(name, " ") == 0)
        name = "Space";

    if (is_one_char(name)) {
        gunichar c = g_utf8_get_char(name);
        if (shift && g_unichar_islower(c))
            c = g_unichar_toupper(c);
        char buf[8] = { 0 };
        g_unichar_to_utf8(c, buf);
        key = g_strdup(buf);
        shift = false; /* implied by the character */
    } else {
        const char* canonical = canonical_name(name);
        if (canonical == NULL)
            return NULL; /* a modifier, or a key without a name here */
        key = g_strdup(canonical);
        if (is_one_char(key))
            shift = false; /* "<lt>" */
    }
    return g_strdup_printf("%s%s%s%s", ctrl ? "C-" : "", alt ? "A-" : "", shift ? "S-" : "", key);
}

char** sfe_keymap_parse(const char* sequence, GError** error)
{
    GPtrArray* tokens = g_ptr_array_new_with_free_func(g_free);
    const char* p = sequence;

    if (sequence == NULL || *sequence == '\0') {
        g_set_error_literal(error, SFE_KEYMAP_ERROR, 0, "empty key sequence");
        goto fail;
    }
    while (*p != '\0') {
        char* token = NULL;
        if (*p == '<' && strchr(p, '>') != NULL && strchr(p, '>') > p + 1) {
            const char* end = strchr(p, '>');
            g_autofree char* inner = g_strndup(p + 1, end - p - 1);
            const char* key = inner;
            bool ctrl = false, alt = false, shift = false;
            /* Modifiers, then the key (which may itself be "-"). */
            while (key[0] != '\0' && key[1] == '-' && key[2] != '\0') {
                char m = g_ascii_toupper(key[0]);
                if (m == 'C')
                    ctrl = true;
                else if (m == 'A' || m == 'M')
                    alt = true;
                else if (m == 'S')
                    shift = true;
                else
                    break;
                key += 2;
            }
            token = sfe_keymap_token(key, ctrl, alt, shift);
            if (token == NULL) {
                g_set_error(error, SFE_KEYMAP_ERROR, 0, "unknown key <%s> in \"%s\"", inner, sequence);
                goto fail;
            }
            p = end + 1;
        } else {
            const char* next = g_utf8_next_char(p);
            g_autofree char* ch = g_strndup(p, next - p);
            token = sfe_keymap_token(ch, false, false, false);
            p = next;
        }
        g_ptr_array_add(tokens, token);
    }
    g_ptr_array_add(tokens, NULL);
    return (char**)g_ptr_array_free(tokens, FALSE);

fail:
    g_ptr_array_unref(tokens);
    return NULL;
}

char* sfe_keymap_format(char* const* tokens, guint n)
{
    GString* out = g_string_new(NULL);
    for (guint i = 0; i < n; i++) {
        const char* t = tokens[i];
        if (strcmp(t, "<") == 0)
            g_string_append(out, "<lt>");
        else if (is_one_char(t))
            g_string_append(out, t);
        else
            g_string_append_printf(out, "<%s>", t);
    }
    return g_string_free(out, FALSE);
}

/* ------------------------------------------------------------------------ */
/* The map                                                                   */
/* ------------------------------------------------------------------------ */

typedef struct {
    char** tokens;
    guint n;
    char* command;
} Binding;

struct SfeKeymap {
    GPtrArray* bindings; /* Binding* */
};

static void binding_free(gpointer p)
{
    Binding* b = p;
    g_strfreev(b->tokens);
    g_free(b->command);
    g_free(b);
}

SfeKeymap* sfe_keymap_new(void)
{
    SfeKeymap* map = g_new0(SfeKeymap, 1);
    map->bindings = g_ptr_array_new_with_free_func(binding_free);
    return map;
}

static SfeKeymap* new_from(const SfeKeyBinding* table, size_t n)
{
    SfeKeymap* map = sfe_keymap_new();
    for (size_t i = 0; i < n; i++) {
        bool ok = sfe_keymap_bind(map, table[i].sequence, table[i].command, NULL);
        g_assert(ok);
    }
    return map;
}

SfeKeymap* sfe_keymap_new_default(void)
{
    return new_from(defaults, G_N_ELEMENTS(defaults));
}

SfeKeymap* sfe_keymap_new_visual_default(void)
{
    return new_from(visual_defaults, G_N_ELEMENTS(visual_defaults));
}

void sfe_keymap_free(SfeKeymap* map)
{
    if (map == NULL)
        return;
    g_ptr_array_unref(map->bindings);
    g_free(map);
}

static bool same_tokens(char* const* a, guint na, char* const* b, guint nb)
{
    if (na != nb)
        return false;
    for (guint i = 0; i < na; i++) {
        if (strcmp(a[i], b[i]) != 0)
            return false;
    }
    return true;
}

bool sfe_keymap_bind(SfeKeymap* map, const char* sequence, const char* command, GError** error)
{
    char** tokens = sfe_keymap_parse(sequence, error);
    if (tokens == NULL)
        return false;
    guint n = g_strv_length(tokens);

    for (guint i = 0; i < map->bindings->len; i++) {
        Binding* b = g_ptr_array_index(map->bindings, i);
        if (same_tokens(b->tokens, b->n, tokens, n)) {
            g_ptr_array_remove_index(map->bindings, i);
            break;
        }
    }
    if (command == NULL || *command == '\0') {
        g_strfreev(tokens);
        return true;
    }
    Binding* b = g_new0(Binding, 1);
    b->tokens = tokens;
    b->n = n;
    b->command = g_strdup(command);
    g_ptr_array_add(map->bindings, b);
    return true;
}

SfeKeysResult sfe_keymap_lookup(const SfeKeymap* map, char* const* tokens, guint n, const char** command,
    bool* prefix)
{
    const char* exact = NULL;
    bool longer = false;

    for (guint i = 0; i < map->bindings->len; i++) {
        const Binding* b = g_ptr_array_index(map->bindings, i);
        if (b->n < n)
            continue;
        bool starts = true;
        for (guint k = 0; k < n && starts; k++)
            starts = strcmp(b->tokens[k], tokens[k]) == 0;
        if (!starts)
            continue;
        if (b->n == n)
            exact = b->command;
        else
            longer = true;
    }
    if (command != NULL)
        *command = exact;
    if (prefix != NULL)
        *prefix = longer;
    if (exact != NULL)
        return SFE_KEYS_MATCH;
    return longer ? SFE_KEYS_PREFIX : SFE_KEYS_NONE;
}
