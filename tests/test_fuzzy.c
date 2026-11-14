#include "fuzzy.h"

#include <stdlib.h>
#include <string.h>

static int score(const char* q, const char* t)
{
    return sfe_fuzzy_score(q, t);
}

/* Asserts that `query` ranks the names in exactly the given order. */
static void assert_order(const char* query, const char* const* names, int n)
{
    for (int i = 0; i + 1 < n; i++) {
        int a = score(query, names[i]);
        int b = score(query, names[i + 1]);
        if (!(a > b))
            g_error("query “%s”: “%s” (%d) should rank above “%s” (%d)", query, names[i], a, names[i + 1], b);
    }
}

static void test_no_match(void)
{
    g_assert_cmpint(score("xyz", "readme.md"), ==, SFE_FUZZY_NO_MATCH);
    g_assert_cmpint(score("mdr", "readme.md"), ==, SFE_FUZZY_NO_MATCH); /* letters out of order */
    g_assert_cmpint(score("toolongquery", "short"), ==, SFE_FUZZY_NO_MATCH);
    g_assert_cmpint(score("", "anything"), ==, 0);
}

static void test_tiers(void)
{
    const char* const names[] = {
        "readme", /* exact */
        "readme.md", /* exact without extension */
        "readmes", /* prefix */
        "old_readme.txt", /* substring at a word start */
        "notreadme", /* substring */
        "r_e_a_d_m_e", /* fuzzy */
    };
    assert_order("readme", names, G_N_ELEMENTS(names));
}

static void test_within_tier(void)
{
    /* Shorter prefix matches first. */
    const char* const prefix[] = { "doc", "docs", "documents" };
    assert_order("do", prefix, G_N_ELEMENTS(prefix));

    /* Fuzzy: word starts and consecutive letters beat scattered letters. */
    const char* const fuzzy[] = { "foo_bar.c", "fxxxxbxxxx.c" };
    assert_order("fb", fuzzy, G_N_ELEMENTS(fuzzy));

    const char* const camel[] = { "myFileBrowser", "myfilebrowser" };
    assert_order("mfb", camel, G_N_ELEMENTS(camel));

    /* Consecutive letters beat gaps. */
    const char* const runs[] = { "xconfig", "cxoxnxfxixg" };
    assert_order("config", runs, G_N_ELEMENTS(runs));
}

static void test_smart_case(void)
{
    g_assert_cmpint(score("readme", "README.md"), >, 0); /* lowercase query ignores case */
    g_assert_cmpint(score("README", "readme.md"), ==, SFE_FUZZY_NO_MATCH); /* uppercase query is exact */
    g_assert_cmpint(score("README", "README.md"), >, 0);
}

static void test_unicode(void)
{
    g_assert_cmpint(score("снимка", "Снимка_001.jpg"), >, score("снимка", "моята снимка.jpg"));
    g_assert_cmpint(score("ü", "Über.txt"), >, 0);
}

/* A realistic folder: the perfect match must come first. */
static void test_realistic_sort(void)
{
    const char* names[] = { "Makefile", "main.c", "main.h", "maintainers.md", "meson.build", "manual.pdf", "main" };
    const char* query = "main";
    int n = G_N_ELEMENTS(names);
    int scores[G_N_ELEMENTS(names)];

    for (int i = 0; i < n; i++)
        scores[i] = score(query, names[i]);
    int best = 0;
    for (int i = 1; i < n; i++) {
        if (scores[i] > scores[best])
            best = i;
    }
    g_assert_cmpstr(names[best], ==, "main");
    g_assert_cmpint(score(query, "main.c"), >, score(query, "maintainers.md"));
    g_assert_cmpint(score(query, "meson.build"), ==, SFE_FUZZY_NO_MATCH);
}

int main(int argc, char** argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/fuzzy/no-match", test_no_match);
    g_test_add_func("/fuzzy/tiers", test_tiers);
    g_test_add_func("/fuzzy/within-tier", test_within_tier);
    g_test_add_func("/fuzzy/smart-case", test_smart_case);
    g_test_add_func("/fuzzy/unicode", test_unicode);
    g_test_add_func("/fuzzy/realistic", test_realistic_sort);
    return g_test_run();
}
