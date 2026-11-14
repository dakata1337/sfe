#include "rename.h"

#include "util.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_TEMPLATE "{name}{ext}"

struct SfeRenamer {
    char* template_;
    GRegex* regex; /* NULL when nothing is replaced */
    char* replace;
    bool literal; /* plain-text find: the replacement has no \1 references */
    SfeCaseMode case_mode;
    int counter_start;
    int counter_step;
};

/* ------------------------------------------------------------------------ */
/* Building names                                                            */
/* ------------------------------------------------------------------------ */

SfeRenamer* sfe_renamer_new(const SfeRenameRule* rule, GError** error)
{
    g_autoptr(GRegex) regex = NULL;

    if (rule->find != NULL && *rule->find != '\0') {
        g_autofree char* pattern = rule->regex ? g_strdup(rule->find) : g_regex_escape_string(rule->find, -1);
        GRegexCompileFlags flags = rule->match_case ? 0 : G_REGEX_CASELESS;
        regex = g_regex_new(pattern, flags, 0, error);
        if (regex == NULL)
            return NULL;
        if (rule->regex && !g_regex_check_replacement(rule->replace ? rule->replace : "", NULL, error))
            return NULL;
    }

    SfeRenamer* r = g_new0(SfeRenamer, 1);
    r->template_ = g_strdup(rule->template_ && *rule->template_ ? rule->template_ : DEFAULT_TEMPLATE);
    r->regex = g_steal_pointer(&regex);
    r->replace = g_strdup(rule->replace ? rule->replace : "");
    r->literal = !rule->regex;
    r->case_mode = rule->case_mode;
    r->counter_start = rule->counter_start;
    r->counter_step = rule->counter_step;
    return r;
}

void sfe_renamer_free(SfeRenamer* r)
{
    if (r == NULL)
        return;
    g_free(r->template_);
    g_clear_pointer(&r->regex, g_regex_unref);
    g_free(r->replace);
    g_free(r);
}

/* "hello wORLD-foo" -> "Hello World-Foo" */
static char* title_case(const char* text)
{
    g_autofree char* lower = g_utf8_strdown(text, -1);
    GString* out = g_string_new(NULL);
    bool word_start = true;

    for (const char* p = lower; *p; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);
        g_string_append_unichar(out, word_start ? g_unichar_totitle(c) : c);
        word_start = !g_unichar_isalnum(c);
    }
    return g_string_free(out, FALSE);
}

static char* change_case(const char* text, SfeCaseMode mode)
{
    switch (mode) {
    case SFE_CASE_LOWER: return g_utf8_strdown(text, -1);
    case SFE_CASE_UPPER: return g_utf8_strup(text, -1);
    case SFE_CASE_TITLE: return title_case(text);
    case SFE_CASE_KEEP: break;
    }
    return g_strdup(text);
}

/* Expands {name}, {ext}, {n} and {n:WIDTH}; anything else is kept as is. */
static char* expand_template(const char* tpl, const char* stem, const char* ext, int counter)
{
    GString* out = g_string_new(NULL);

    for (const char* p = tpl; *p;) {
        const char* close = *p == '{' ? strchr(p, '}') : NULL;
        if (close == NULL) {
            g_string_append_c(out, *p++);
            continue;
        }
        g_autofree char* key = g_strndup(p + 1, close - p - 1);
        if (strcmp(key, "name") == 0)
            g_string_append(out, stem);
        else if (strcmp(key, "ext") == 0)
            g_string_append(out, ext);
        else if (strcmp(key, "n") == 0)
            g_string_append_printf(out, "%d", counter);
        else if (g_str_has_prefix(key, "n:") && key[2] != '\0' && strspn(key + 2, "0123456789") == strlen(key + 2))
            g_string_append_printf(out, "%0*d", MIN(atoi(key + 2), 32), counter);
        else
            g_string_append_len(out, p, close - p + 1);
        p = close + 1;
    }
    return g_string_free(out, FALSE);
}

char* sfe_renamer_apply(SfeRenamer* r, const char* name, bool is_dir, int index)
{
    g_autofree char* stem = NULL;
    g_autofree char* ext = NULL;
    sfe_split_extension(name, is_dir, &stem, &ext);

    if (r->regex != NULL) {
        char* replaced = r->literal
            ? g_regex_replace_literal(r->regex, stem, -1, 0, r->replace, 0, NULL)
            : g_regex_replace(r->regex, stem, -1, 0, r->replace, 0, NULL);
        if (replaced != NULL) {
            g_free(stem);
            stem = replaced;
        }
    }
    g_autofree char* cased = change_case(stem, r->case_mode);
    return expand_template(r->template_, cased, ext, r->counter_start + index * r->counter_step);
}

/* ------------------------------------------------------------------------ */
/* Checking                                                                  */
/* ------------------------------------------------------------------------ */

const char* sfe_rename_status_text(SfeRenameStatus status)
{
    switch (status) {
    case SFE_RENAME_OK: return "";
    case SFE_RENAME_UNCHANGED: return "unchanged";
    case SFE_RENAME_EMPTY: return "empty name";
    case SFE_RENAME_INVALID: return "invalid name";
    case SFE_RENAME_DUPLICATE: return "duplicate name";
    case SFE_RENAME_EXISTS: return "already exists";
    }
    return "";
}

/* "parent-path/name", to compare names within the same folder. */
static char* key_for(GFile* file, const char* name)
{
    g_autoptr(GFile) parent = g_file_get_parent(file);
    g_autofree char* dir = parent ? g_file_get_path(parent) : g_strdup("");
    return g_strconcat(dir, "/", name, NULL);
}

static bool same_ignoring_case(const char* a, const char* b)
{
    g_autofree char* fa = g_utf8_casefold(a, -1);
    g_autofree char* fb = g_utf8_casefold(b, -1);
    return strcmp(fa, fb) == 0;
}

int sfe_rename_check(GFile** files, char** names, int n, SfeRenameStatus* status)
{
    g_autoptr(GHashTable) final_count = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_autoptr(GHashTable) vacated = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    int problems = 0;

    /* Basic validity, and which names the set leaves free. */
    for (int i = 0; i < n; i++) {
        g_autofree char* old = g_file_get_basename(files[i]);
        const char* name = names[i];

        if (*name == '\0')
            status[i] = SFE_RENAME_EMPTY;
        else if (strchr(name, '/') != NULL || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            status[i] = SFE_RENAME_INVALID;
        else if (strcmp(old, name) == 0)
            status[i] = SFE_RENAME_UNCHANGED;
        else
            status[i] = SFE_RENAME_OK;

        if (status[i] == SFE_RENAME_OK)
            g_hash_table_add(vacated, key_for(files[i], old));
    }

    /* Every file ends up with exactly one name: count them. */
    for (int i = 0; i < n; i++) {
        g_autofree char* old = g_file_get_basename(files[i]);
        bool renamed = status[i] == SFE_RENAME_OK;
        char* key = key_for(files[i], renamed ? names[i] : old);
        int count = GPOINTER_TO_INT(g_hash_table_lookup(final_count, key));
        g_hash_table_replace(final_count, key, GINT_TO_POINTER(count + 1));
    }

    for (int i = 0; i < n; i++) {
        if (status[i] != SFE_RENAME_OK)
            continue;
        g_autofree char* old = g_file_get_basename(files[i]);
        g_autofree char* key = key_for(files[i], names[i]);
        g_autoptr(GFile) parent = g_file_get_parent(files[i]);
        g_autoptr(GFile) target = g_file_get_child(parent, names[i]);

        if (GPOINTER_TO_INT(g_hash_table_lookup(final_count, key)) > 1)
            status[i] = SFE_RENAME_DUPLICATE;
        else if (!g_hash_table_contains(vacated, key) && !same_ignoring_case(old, names[i])
            && g_file_query_exists(target, NULL))
            status[i] = SFE_RENAME_EXISTS;
    }

    for (int i = 0; i < n; i++)
        problems += status[i] != SFE_RENAME_OK && status[i] != SFE_RENAME_UNCHANGED;
    return problems;
}

/* ------------------------------------------------------------------------ */
/* Renaming                                                                  */
/* ------------------------------------------------------------------------ */

static GFile* rename_to(GFile* file, const char* name, GError** error)
{
    return g_file_set_display_name(file, name, NULL, error);
}

/* Puts every file back under its original name after a failure. `current`
 * holds where each file is now (original, temporary or final name). */
static void roll_back(GFile** files, GFile** current, GFile** temps, int n)
{
    /* Finals first go back to their temporary names, so no original name is
     * taken when the temporaries are restored. */
    for (int i = 0; i < n; i++) {
        if (current[i] != NULL && temps[i] != NULL && !g_file_equal(current[i], temps[i])
            && !g_file_equal(current[i], files[i])) {
            g_autofree char* tmp_name = g_file_get_basename(temps[i]);
            GFile* back = rename_to(current[i], tmp_name, NULL);
            if (back != NULL) {
                g_object_unref(current[i]);
                current[i] = back;
            }
        }
    }
    for (int i = 0; i < n; i++) {
        if (current[i] != NULL && !g_file_equal(current[i], files[i])) {
            g_autofree char* orig = g_file_get_basename(files[i]);
            GFile* back = rename_to(current[i], orig, NULL);
            g_clear_object(&back);
        }
    }
}

bool sfe_rename_files(GFile** files, char** names, int n, GError** error)
{
    GFile** current = g_new0(GFile*, n);
    GFile** temps = g_new0(GFile*, n);
    bool ok = true;

    /* Pass 1: every renamed file moves to a unique temporary name. */
    for (int i = 0; i < n && ok; i++) {
        g_autofree char* old = g_file_get_basename(files[i]);
        current[i] = g_object_ref(files[i]);
        if (strcmp(old, names[i]) == 0)
            continue;
        g_autofree char* tmp = g_strdup_printf(".sfe-rename-%d-%d-%08x", (int)getpid(), i, g_random_int());
        temps[i] = rename_to(files[i], tmp, error);
        if (temps[i] == NULL)
            ok = false;
        else
            g_set_object(&current[i], temps[i]);
    }

    /* Pass 2: temporary names become the final ones. */
    for (int i = 0; i < n && ok; i++) {
        if (temps[i] == NULL)
            continue;
        GFile* final = rename_to(temps[i], names[i], error);
        if (final == NULL) {
            ok = false;
        } else {
            g_object_unref(current[i]);
            current[i] = final;
        }
    }

    if (!ok)
        roll_back(files, current, temps, n);
    for (int i = 0; i < n; i++) {
        g_clear_object(&current[i]);
        g_clear_object(&temps[i]);
    }
    g_free(current);
    g_free(temps);
    return ok;
}
