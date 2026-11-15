#include "fuzzy.h"

#include <limits.h>
#include <stdbool.h>

/* Each tier is worth more than any score inside the tier below. */
#define TIER_SCALE 100000

enum {
    TIER_FUZZY,
    TIER_SUBSTRING,
    TIER_WORD, /* substring starting at a word boundary */
    TIER_PREFIX,
    TIER_STEM, /* the name without its extension equals the query */
    TIER_EXACT,
};

/* Fuzzy (subsequence) scoring, in the spirit of fzf. */
#define SCORE_MATCH 16
#define BONUS_BOUNDARY 10 /* letter at a word start: "fb" in "foo_bar" */
#define BONUS_FIRST 8 /* letter at the very start of the name */
#define BONUS_CONSECUTIVE 12 /* letter right after the previous match */
#define PENALTY_GAP 1 /* per skipped character */
#define NEG (INT_MIN / 2)

static bool has_upper(const char* s)
{
    for (const char* p = s; *p; p = g_utf8_next_char(p)) {
        if (g_unichar_isupper(g_utf8_get_char(p)))
            return true;
    }
    return false;
}

/* UCS-4 copy, lowercased when `fold` (tolower keeps one char per char, so
 * positions stay aligned with the original). */
static gunichar* to_chars(const char* s, bool fold, glong* len)
{
    gunichar* chars = g_utf8_to_ucs4_fast(s, -1, len);
    if (fold) {
        for (glong i = 0; i < *len; i++)
            chars[i] = g_unichar_tolower(chars[i]);
    }
    return chars;
}

/* Word starts: the first char, a char after a separator, or camelCase. */
static bool is_boundary(const gunichar* orig, glong i)
{
    if (i == 0)
        return true;
    if (!g_unichar_isalnum(orig[i - 1]))
        return true;
    return g_unichar_islower(orig[i - 1]) && g_unichar_isupper(orig[i]);
}

static bool equal_at(const gunichar* text, glong at, const gunichar* query, glong qlen)
{
    for (glong i = 0; i < qlen; i++) {
        if (text[at + i] != query[i])
            return false;
    }
    return true;
}

/* Length of the name without its extension (".bashrc" has none). */
static glong stem_length(const gunichar* text, glong len)
{
    for (glong i = len - 1; i > 0; i--) {
        if (text[i] == '.')
            return i;
    }
    return len;
}

static int fuzzy_dp(const gunichar* text, const gunichar* orig, glong tlen, const gunichar* query, glong qlen)
{
    int* prev = g_new(int, tlen);
    int* cur = g_new(int, tlen);
    int best = NEG;

    for (glong j = 0; j < tlen; j++) {
        int bonus = (is_boundary(orig, j) ? BONUS_BOUNDARY : 0) + (j == 0 ? BONUS_FIRST : 0);
        prev[j] = text[j] == query[0] ? SCORE_MATCH + bonus - PENALTY_GAP * (int)j : NEG;
    }

    for (glong i = 1; i < qlen; i++) {
        int run = NEG; /* max over k <= j-2 of prev[k] + PENALTY_GAP * k */
        for (glong j = 0; j < tlen; j++) {
            if (j >= 2 && prev[j - 2] > NEG)
                run = MAX(run, prev[j - 2] + PENALTY_GAP * (int)(j - 2));
            cur[j] = NEG;
            if (j < i || text[j] != query[i])
                continue;

            int from = NEG;
            if (prev[j - 1] > NEG)
                from = prev[j - 1] + BONUS_CONSECUTIVE;
            if (run > NEG)
                from = MAX(from, run - PENALTY_GAP * (int)(j - 1));
            if (from > NEG)
                cur[j] = from + SCORE_MATCH + (is_boundary(orig, j) ? BONUS_BOUNDARY : 0);
        }
        int* tmp = prev;
        prev = cur;
        cur = tmp;
    }

    for (glong j = 0; j < tlen; j++)
        best = MAX(best, prev[j]);
    g_free(prev);
    g_free(cur);
    return best;
}

/* The fuzzy tier's letters: the same scoring as fuzzy_dp(), kept as a full
 * table so the best path can be followed back. Fills `at` (qlen indices). */
static void fuzzy_path(const gunichar* text, const gunichar* orig, glong tlen, const gunichar* query, glong qlen,
    glong* at)
{
    int* score = g_new(int, qlen * tlen);
    glong* from = g_new(glong, qlen * tlen);

    for (glong j = 0; j < tlen; j++) {
        int bonus = (is_boundary(orig, j) ? BONUS_BOUNDARY : 0) + (j == 0 ? BONUS_FIRST : 0);
        score[j] = text[j] == query[0] ? SCORE_MATCH + bonus - PENALTY_GAP * (int)j : NEG;
        from[j] = -1;
    }
    for (glong i = 1; i < qlen; i++) {
        for (glong j = 0; j < tlen; j++) {
            int best = NEG;
            glong best_k = -1;
            for (glong k = 0; k < j && text[j] == query[i]; k++) {
                int prev = score[(i - 1) * tlen + k];
                if (prev <= NEG)
                    continue;
                int s = prev + (k == j - 1 ? BONUS_CONSECUTIVE : -PENALTY_GAP * (int)(j - k - 1));
                if (s > best) {
                    best = s;
                    best_k = k;
                }
            }
            score[i * tlen + j] = best > NEG ? best + SCORE_MATCH + (is_boundary(orig, j) ? BONUS_BOUNDARY : 0) : NEG;
            from[i * tlen + j] = best_k;
        }
    }
    glong end = 0;
    for (glong j = 1; j < tlen; j++) {
        if (score[(qlen - 1) * tlen + j] > score[(qlen - 1) * tlen + end])
            end = j;
    }
    for (glong i = qlen - 1; i >= 0; i--) {
        at[i] = end;
        end = from[i * tlen + end];
    }
    g_free(score);
    g_free(from);
}

/* Character indices -> byte offsets of `text`. */
static void add_positions(GArray* positions, const char* text, const glong* at, glong n)
{
    for (glong i = 0; positions != NULL && i < n; i++) {
        int byte = (int)(g_utf8_offset_to_pointer(text, at[i]) - text);
        g_array_append_val(positions, byte);
    }
}

static void add_run(GArray* positions, const char* text, glong start, glong n)
{
    g_autofree glong* at = g_new(glong, MAX(n, 1));
    for (glong i = 0; i < n; i++)
        at[i] = start + i;
    add_positions(positions, text, at, n);
}

int sfe_fuzzy_score(const char* query, const char* text)
{
    return sfe_fuzzy_match(query, text, NULL);
}

int sfe_fuzzy_match(const char* query, const char* text, GArray* positions)
{
    if (query == NULL || *query == '\0')
        return 0;

    bool fold = !has_upper(query);
    glong qlen = 0, tlen = 0, olen = 0;
    g_autofree gunichar* q = to_chars(query, fold, &qlen);
    g_autofree gunichar* t = to_chars(text, fold, &tlen);
    g_autofree gunichar* orig = to_chars(text, false, &olen);

    if (qlen == 0 || qlen > tlen)
        return qlen == 0 ? 0 : SFE_FUZZY_NO_MATCH;

    int shortness = 1000 - (int)tlen; /* shorter names rank higher */
    if (equal_at(t, 0, q, qlen)) {
        add_run(positions, text, 0, qlen);
        if (qlen == tlen)
            return TIER_EXACT * TIER_SCALE + 1000;
        if (qlen == stem_length(t, tlen))
            return TIER_STEM * TIER_SCALE + shortness;
        return TIER_PREFIX * TIER_SCALE + shortness;
    }

    glong first = -1, word = -1;
    for (glong i = 1; i + qlen <= tlen && word < 0; i++) {
        if (equal_at(t, i, q, qlen)) {
            if (first < 0)
                first = i;
            if (is_boundary(orig, i))
                word = i;
        }
    }
    if (word >= 0) {
        add_run(positions, text, word, qlen);
        return TIER_WORD * TIER_SCALE + shortness - (int)word;
    }
    if (first >= 0) {
        add_run(positions, text, first, qlen);
        return TIER_SUBSTRING * TIER_SCALE + shortness - (int)first;
    }

    int dp = fuzzy_dp(t, orig, tlen, q, qlen);
    if (dp <= NEG)
        return SFE_FUZZY_NO_MATCH;
    if (positions != NULL) {
        g_autofree glong* at = g_new(glong, qlen);
        fuzzy_path(t, orig, tlen, q, qlen, at);
        add_positions(positions, text, at, qlen);
    }
    return TIER_FUZZY * TIER_SCALE + CLAMP(TIER_SCALE / 2 + dp - (int)tlen / 4, 0, TIER_SCALE - 1);
}
