#include "cmdline.h"

#include "fuzzy.h"
#include <string.h>

#define SFE_CMDLINE_ERROR (g_quark_from_static_string("sfe-cmdline-error"))

static void arg_free(gpointer p)
{
    SfeCmdArg* a = p;
    g_free(a->value);
    g_free(a);
}

GPtrArray* sfe_cmdline_tokenize(const char* line)
{
    GPtrArray* args = g_ptr_array_new_with_free_func(arg_free);
    const char* p = line ? line : "";

    for (;;) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0')
            break;

        SfeCmdArg* a = g_new0(SfeCmdArg, 1);
        GString* value = g_string_new(NULL);
        char quote = 0;

        a->start = (int)(p - line);
        while (*p != '\0' && (quote != 0 || (*p != ' ' && *p != '\t'))) {
            if (quote == '\'') {
                if (*p == '\'')
                    quote = 0;
                else
                    g_string_append_c(value, *p);
                p++;
            } else if (quote == '"') {
                if (*p == '"') {
                    quote = 0;
                    p++;
                } else if (*p == '\\' && (p[1] == '"' || p[1] == '\\')) {
                    g_string_append_c(value, p[1]);
                    p += 2;
                } else {
                    g_string_append_c(value, *p++);
                }
            } else if (*p == '\'' || *p == '"') {
                quote = *p++;
            } else if (*p == '\\' && p[1] != '\0') {
                g_string_append_c(value, p[1]);
                p += 2;
            } else {
                g_string_append_c(value, *p++);
            }
        }
        a->end = (int)(p - line);
        a->open_quote = quote != 0;
        a->value = g_string_free(value, FALSE);
        g_ptr_array_add(args, a);
    }
    return args;
}

char** sfe_cmdline_split(const char* line, GError** error)
{
    g_autoptr(GPtrArray) args = sfe_cmdline_tokenize(line);
    GPtrArray* argv = g_ptr_array_new_with_free_func(g_free);

    for (guint i = 0; i < args->len; i++) {
        SfeCmdArg* a = g_ptr_array_index(args, i);
        if (a->open_quote) {
            g_set_error_literal(error, SFE_CMDLINE_ERROR, 0, "unfinished quote");
            g_ptr_array_free(argv, TRUE);
            return NULL;
        }
        g_ptr_array_add(argv, g_strdup(a->value));
    }
    g_ptr_array_add(argv, NULL);
    return (char**)g_ptr_array_free(argv, FALSE);
}

char* sfe_cmdline_quote(const char* arg)
{
    GString* out = g_string_new(NULL);
    for (const char* p = arg; *p != '\0'; p++) {
        if (strchr(" \t'\"\\", *p) != NULL)
            g_string_append_c(out, '\\');
        g_string_append_c(out, *p);
    }
    return g_string_free(out, FALSE);
}

/* ------------------------------------------------------------------------ */
/* Completion                                                                */
/* ------------------------------------------------------------------------ */

SfeCandidate* sfe_candidate_new(const char* insert, const char* match, const char* tag, int group)
{
    SfeCandidate* c = g_new0(SfeCandidate, 1);
    c->insert = g_strdup(insert);
    c->match = g_strdup(match ? match : insert);
    c->tag = g_strdup(tag);
    c->group = group;
    return c;
}

SfeCandidate* sfe_candidate_new_names(const char* const* names, const char* tag, int group)
{
    SfeCandidate* c = sfe_candidate_new(names[0], NULL, tag, group);
    c->names = g_strdupv((char**)names);
    c->label = g_strjoinv(" | ", c->names);
    return c;
}

void sfe_candidate_free(gpointer candidate)
{
    SfeCandidate* c = candidate;
    if (c == NULL)
        return;
    g_free(c->insert);
    g_free(c->match);
    g_free(c->tag);
    g_free(c->label);
    g_free(c->args);
    g_strfreev(c->names);
    g_free(c);
}

static int compare_candidates(gconstpointer a, gconstpointer b)
{
    const SfeCandidate* x = *(SfeCandidate* const*)a;
    const SfeCandidate* y = *(SfeCandidate* const*)b;
    if (x->group != y->group)
        return x->group < y->group ? -1 : 1;
    if (x->score != y->score)
        return x->score > y->score ? -1 : 1;
    return x->order < y->order ? -1 : x->order > y->order;
}

GPtrArray* sfe_cmdline_rank(GPtrArray* candidates, const char* query)
{
    GPtrArray* out = g_ptr_array_new_with_free_func(sfe_candidate_free);
    g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);

    for (guint i = 0; i < candidates->len; i++) {
        SfeCandidate* c = g_ptr_array_index(candidates, i);
        c->order = (int)i;
        c->score = sfe_fuzzy_score(query ? query : "", c->match);
        for (int k = 1; c->names != NULL && c->names[k] != NULL; k++) { /* the best name wins */
            int score = sfe_fuzzy_score(query ? query : "", c->names[k]);
            if (score > c->score) {
                c->score = score;
                g_free(c->insert);
                g_free(c->match);
                c->insert = g_strdup(c->names[k]);
                c->match = g_strdup(c->names[k]);
            }
        }
    }
    /* Taken over one by one: sorted first so that a duplicate keeps its
     * best place, then the matching ones move to `out`. */
    g_ptr_array_set_free_func(candidates, NULL);
    g_ptr_array_sort(candidates, compare_candidates);
    for (guint i = 0; i < candidates->len; i++) {
        SfeCandidate* c = g_ptr_array_index(candidates, i);
        if (c->score == SFE_FUZZY_NO_MATCH || g_hash_table_contains(seen, c->insert)) {
            sfe_candidate_free(c);
            continue;
        }
        g_hash_table_add(seen, c->insert);
        g_ptr_array_add(out, c);
    }
    g_ptr_array_unref(candidates);
    return out;
}
