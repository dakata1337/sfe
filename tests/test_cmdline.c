#include "cmdline.h"

#include <string.h>

static void assert_split(const char* line, const char* expected)
{
    g_autoptr(GError) error = NULL;
    g_auto(GStrv) argv = sfe_cmdline_split(line, &error);
    g_assert_no_error(error);
    g_autofree char* joined = g_strjoinv("|", argv);
    g_assert_cmpstr(joined, ==, expected);
}

static void test_split(void)
{
    assert_split("", "");
    assert_split("   ", "");
    assert_split("copy a b", "copy|a|b");
    assert_split("  rename   old.txt  new.txt ", "rename|old.txt|new.txt");
    assert_split("cd 'My Files'", "cd|My Files");
    assert_split("cd \"My \\\"Files\\\"\"", "cd|My \"Files\"");
    assert_split("cd My\\ Files", "cd|My Files");
    assert_split("cd a'b c'd", "cd|ab cd");
    assert_split("touch \"\"", "touch|");
    assert_split("cd 'back\\slash'", "cd|back\\slash");

    g_autoptr(GError) error = NULL;
    g_assert_null(sfe_cmdline_split("cd 'open", &error));
    g_assert_nonnull(error);
}

static void test_tokenize_positions(void)
{
    const char* line = "copy 'a b' c";
    g_autoptr(GPtrArray) args = sfe_cmdline_tokenize(line);
    g_assert_cmpuint(args->len, ==, 3);
    SfeCmdArg* second = g_ptr_array_index(args, 1);
    g_assert_cmpstr(second->value, ==, "a b");
    g_assert_cmpint(second->start, ==, 5);
    g_assert_cmpint(second->end, ==, 10);

    /* While typing: an open quote runs to the end. */
    g_autoptr(GPtrArray) open = sfe_cmdline_tokenize("cd 'My Fi");
    SfeCmdArg* last = g_ptr_array_index(open, 1);
    g_assert_true(last->open_quote);
    g_assert_cmpstr(last->value, ==, "My Fi");
}

static void test_quote_round_trip(void)
{
    const char* names[] = { "plain.txt", "My Files", "it's", "a\"b", "back\\slash", "tab\there", "~/x" };
    for (size_t i = 0; i < G_N_ELEMENTS(names); i++) {
        g_autofree char* quoted = sfe_cmdline_quote(names[i]);
        g_autofree char* line = g_strconcat("cmd ", quoted, NULL);
        g_auto(GStrv) argv = sfe_cmdline_split(line, NULL);
        g_assert_cmpstr(argv[1], ==, names[i]);
    }
    g_autofree char* q = sfe_cmdline_quote("My Files");
    g_assert_cmpstr(q, ==, "My\\ Files");
    g_autofree char* tilde = sfe_cmdline_quote("~/Documents");
    g_assert_cmpstr(tilde, ==, "~/Documents"); /* left readable */
}

static GPtrArray* candidates(void)
{
    GPtrArray* c = g_ptr_array_new_with_free_func(sfe_candidate_free);
    /* Added out of order on purpose: groups decide first. */
    g_ptr_array_add(c, sfe_candidate_new("~/work/report.pdf", "report.pdf", "right-top", 1));
    g_ptr_array_add(c, sfe_candidate_new("notes.txt", NULL, "left", 0));
    g_ptr_array_add(c, sfe_candidate_new("report-old.pdf", NULL, "left", 0));
    g_ptr_array_add(c, sfe_candidate_new("report.pdf", NULL, "left", 0));
    g_ptr_array_add(c, sfe_candidate_new("~/music/repo", "repo", "right-bottom", 2));
    g_ptr_array_add(c, sfe_candidate_new("notes.txt", NULL, "right-top", 1)); /* duplicate */
    return c;
}

static const char* insert_at(GPtrArray* ranked, guint i)
{
    return ((SfeCandidate*)g_ptr_array_index(ranked, i))->insert;
}

static void test_rank(void)
{
    /* "report": active view first, exact name before prefix; other views
     * after it, in their group order. */
    g_autoptr(GPtrArray) ranked = sfe_cmdline_rank(candidates(), "report");
    g_assert_cmpuint(ranked->len, ==, 3);
    g_assert_cmpstr(insert_at(ranked, 0), ==, "report.pdf");
    g_assert_cmpstr(insert_at(ranked, 1), ==, "report-old.pdf");
    g_assert_cmpstr(insert_at(ranked, 2), ==, "~/work/report.pdf");

    /* Empty query: everything, by group, in the order added; the duplicate
     * goes. */
    g_autoptr(GPtrArray) all = sfe_cmdline_rank(candidates(), "");
    g_assert_cmpuint(all->len, ==, 5);
    g_assert_cmpstr(insert_at(all, 0), ==, "notes.txt");
    g_assert_cmpstr(insert_at(all, 1), ==, "report-old.pdf");
    g_assert_cmpstr(insert_at(all, 3), ==, "~/work/report.pdf");
    g_assert_cmpstr(insert_at(all, 4), ==, "~/music/repo");

    /* Fuzzy: "rpo" finds "repo" and the reports, "zzz" nothing. */
    g_autoptr(GPtrArray) fuzzy = sfe_cmdline_rank(candidates(), "rpo");
    g_assert_cmpuint(fuzzy->len, ==, 4);
    g_autoptr(GPtrArray) none = sfe_cmdline_rank(candidates(), "zzz");
    g_assert_cmpuint(none->len, ==, 0);
}

int main(int argc, char** argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/cmdline/split", test_split);
    g_test_add_func("/cmdline/tokenize", test_tokenize_positions);
    g_test_add_func("/cmdline/quote", test_quote_round_trip);
    g_test_add_func("/cmdline/rank", test_rank);
    return g_test_run();
}
