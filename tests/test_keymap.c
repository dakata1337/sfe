#include "keymap.h"

#include <string.h>

static void assert_tokens(const char* sequence, const char* expected)
{
    g_autoptr(GError) error = NULL;
    g_auto(GStrv) tokens = sfe_keymap_parse(sequence, &error);
    g_assert_no_error(error);
    g_autofree char* joined = g_strjoinv(" ", tokens);
    g_assert_cmpstr(joined, ==, expected);
}

static void test_parse(void)
{
    assert_tokens("j", "j");
    assert_tokens("gg", "g g");
    assert_tokens("<C-w>v", "C-w v");
    assert_tokens("<c-W>", "C-W");
    assert_tokens("<S-h>", "H"); /* shift + letter = capital */
    assert_tokens("<C-S-h>", "C-H");
    assert_tokens("<C-H>", "C-H");
    assert_tokens("<Space>", "Space");
    assert_tokens("<space>", "Space");
    assert_tokens("<CR><Esc><BS>", "CR Esc BS");
    assert_tokens("<S-Tab>", "S-Tab"); /* named keys keep shift */
    assert_tokens("<A-x>", "A-x");
    assert_tokens("<f5>", "F5");
    assert_tokens("<lt>", "<");
    assert_tokens("<S-:>", ":"); /* implied by the character */
    assert_tokens("<", "<"); /* no closing bracket: a plain character */
    assert_tokens("é", "é");

    g_autoptr(GError) error = NULL;
    g_assert_null(sfe_keymap_parse("<C-nope>", &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_null(sfe_keymap_parse("", &error));
}

static void test_token_and_format(void)
{
    /* What the window produces from key events. */
    g_autofree char* a = sfe_keymap_token("h", false, false, true);
    g_assert_cmpstr(a, ==, "H");
    g_autofree char* b = sfe_keymap_token("H", true, false, true); /* Ctrl+Shift+h */
    g_assert_cmpstr(b, ==, "C-H");
    g_autofree char* c = sfe_keymap_token(" ", false, false, false);
    g_assert_cmpstr(c, ==, "Space");
    g_autofree char* d = sfe_keymap_token("Tab", false, false, true);
    g_assert_cmpstr(d, ==, "S-Tab");
    g_assert_null(sfe_keymap_token("Shift_L", false, false, true));

    char* tokens[] = { "C-w", "v", "Space", "<" };
    g_autofree char* text = sfe_keymap_format(tokens, 4);
    g_assert_cmpstr(text, ==, "<C-w>v<Space><lt>");
}

static SfeKeysResult lookup(SfeKeymap* map, const char* sequence, const char** command, bool* prefix)
{
    g_auto(GStrv) tokens = sfe_keymap_parse(sequence, NULL);
    return sfe_keymap_lookup(map, tokens, g_strv_length(tokens), command, prefix);
}

static void test_lookup(void)
{
    g_autoptr(SfeKeymap) map = sfe_keymap_new_default();
    const char* command = NULL;
    bool prefix = false;

    g_assert_cmpint(lookup(map, "j", &command, &prefix), ==, SFE_KEYS_MATCH);
    g_assert_cmpstr(command, ==, "cursor down");
    g_assert_false(prefix);

    g_assert_cmpint(lookup(map, "g", &command, &prefix), ==, SFE_KEYS_PREFIX);
    g_assert_null(command);
    g_assert_cmpint(lookup(map, "gg", &command, NULL), ==, SFE_KEYS_MATCH);
    g_assert_cmpstr(command, ==, "cursor first");
    g_assert_cmpint(lookup(map, "<C-w>", NULL, NULL), ==, SFE_KEYS_PREFIX);
    g_assert_cmpint(lookup(map, "<C-w>v", &command, NULL), ==, SFE_KEYS_MATCH);
    g_assert_cmpstr(command, ==, "split vertical");
    g_assert_cmpint(lookup(map, "<C-w>x", NULL, NULL), ==, SFE_KEYS_NONE);
    g_assert_cmpint(lookup(map, "<S-h>", &command, NULL), ==, SFE_KEYS_MATCH);
    g_assert_cmpstr(command, ==, "split left");
    g_assert_cmpint(lookup(map, "<C-S-l>", &command, NULL), ==, SFE_KEYS_MATCH);
    g_assert_cmpstr(command, ==, "split move right");
    g_assert_cmpint(lookup(map, "q", NULL, NULL), ==, SFE_KEYS_NONE);
}

/* Config [keys]: rebind, unbind, and a binding that is also a prefix. */
static void test_overrides(void)
{
    g_autoptr(SfeKeymap) map = sfe_keymap_new_default();
    const char* command = NULL;
    bool prefix = false;

    g_assert_true(sfe_keymap_bind(map, "j", "cursor up", NULL));
    lookup(map, "j", &command, NULL);
    g_assert_cmpstr(command, ==, "cursor up");

    g_assert_true(sfe_keymap_bind(map, "x", "", NULL));
    g_assert_cmpint(lookup(map, "x", NULL, NULL), ==, SFE_KEYS_NONE);

    g_assert_true(sfe_keymap_bind(map, "g", "cd ~", NULL));
    g_assert_cmpint(lookup(map, "g", &command, &prefix), ==, SFE_KEYS_MATCH);
    g_assert_cmpstr(command, ==, "cd ~");
    g_assert_true(prefix); /* "gg" and "gh" still start with it */

    g_assert_false(sfe_keymap_bind(map, "<C-bogus>", "x", NULL));
}

/* Every default binding parses (the default keymap asserts on it). */
static void test_defaults(void)
{
    size_t n = 0;
    const SfeKeyBinding* d = sfe_keymap_defaults(&n);
    g_assert_cmpuint(n, >, 20);
    for (size_t i = 0; i < n; i++) {
        g_auto(GStrv) tokens = sfe_keymap_parse(d[i].sequence, NULL);
        g_assert_nonnull(tokens);
        g_assert_nonnull(d[i].help);
    }

    /* Visual mode: "y" yanks at once (no "yy" prefix there), Esc leaves. */
    g_autoptr(SfeKeymap) visual = sfe_keymap_new_visual_default();
    const char* command = NULL;
    bool prefix = true;
    char* y[] = { "y" };
    g_assert_cmpint(sfe_keymap_lookup(visual, y, 1, &command, &prefix), ==, SFE_KEYS_MATCH);
    g_assert_cmpstr(command, ==, "yank");
    g_assert_false(prefix);
    char* esc[] = { "Esc" };
    g_assert_cmpint(sfe_keymap_lookup(visual, esc, 1, &command, &prefix), ==, SFE_KEYS_MATCH);
    g_assert_cmpstr(command, ==, "visual off");
    char* g[] = { "g" }; /* "gg" comes from the normal keys */
    g_assert_cmpint(sfe_keymap_lookup(visual, g, 1, &command, &prefix), ==, SFE_KEYS_NONE);
    d = sfe_keymap_visual_defaults(&n);
    for (size_t i = 0; i < n; i++)
        g_assert_nonnull(d[i].help);
}

/* The suggestions after a partial chord: sorted, with help, visual first. */
static void test_continuations(void)
{
    g_autoptr(SfeKeymap) map = sfe_keymap_new_default();
    char* cw[] = { "C-w" };
    g_autoptr(GPtrArray) hints = sfe_keymap_continuations(map, cw, 1, NULL);
    g_assert_cmpuint(hints->len, ==, 3);
    const SfeKeyHint* q = g_ptr_array_index(hints, 0);
    const SfeKeyHint* v = g_ptr_array_index(hints, 2);
    g_assert_cmpstr(q->keys, ==, "q");
    g_assert_cmpstr(v->keys, ==, "v");
    g_assert_cmpstr(v->command, ==, "split vertical");
    g_assert_cmpstr(v->help, ==, "split side by side");

    /* A user binding without a built-in command has no help. */
    g_assert_true(sfe_keymap_bind(map, "<C-w>x", "cd /tmp", NULL));
    g_autoptr(GPtrArray) more = sfe_keymap_continuations(map, cw, 1, NULL);
    g_assert_cmpuint(more->len, ==, 4);
    g_assert_null(((SfeKeyHint*)g_ptr_array_index(more, 3))->help);

    /* Keys from an earlier map win. */
    g_autoptr(SfeKeymap) first = sfe_keymap_new();
    g_assert_true(sfe_keymap_bind(first, "<C-w>v", "open", NULL));
    g_autoptr(GPtrArray) both = sfe_keymap_continuations(first, cw, 1, NULL);
    sfe_keymap_continuations(map, cw, 1, both);
    g_assert_cmpuint(both->len, ==, 4);
    g_assert_cmpstr(((SfeKeyHint*)g_ptr_array_index(both, 2))->command, ==, "open");
}

/* Typing `sequence` key by key with `first` over `then`: no earlier key may
 * run a command on its own (MATCH without a longer binding), else the rest
 * of the sequence would leak into the next chord (the old visual "dd"). The
 * whole sequence must give `expected`. */
static void assert_reachable(const SfeKeymap* first, const SfeKeymap* then, const char* sequence,
    const char* expected)
{
    g_auto(GStrv) tokens = sfe_keymap_parse(sequence, NULL);
    guint n = g_strv_length(tokens);
    for (guint k = 1; k < n; k++) {
        bool prefix = false;
        SfeKeysResult r = sfe_keymap_lookup_layered(first, then, tokens, k, NULL, &prefix);
        if (r == SFE_KEYS_NONE || (r == SFE_KEYS_MATCH && !prefix))
            g_error("“%s” is cut short after %u key(s)%s", sequence, k, first ? " in visual mode" : "");
    }
    const char* command = NULL;
    g_assert_cmpint(sfe_keymap_lookup_layered(first, then, tokens, n, &command, NULL), ==, SFE_KEYS_MATCH);
    g_assert_cmpstr(command, ==, expected);
}

/* Every built-in key works in both modes: normal keys in visual mode too,
 * unless visual mode binds the very same keys. */
static void test_layered(void)
{
    g_autoptr(SfeKeymap) normal = sfe_keymap_new_default();
    g_autoptr(SfeKeymap) visual = sfe_keymap_new_visual_default();
    size_t n = 0, nv = 0;
    const SfeKeyBinding* d = sfe_keymap_defaults(&n);
    const SfeKeyBinding* v = sfe_keymap_visual_defaults(&nv);

    for (size_t i = 0; i < n; i++) {
        assert_reachable(NULL, normal, d[i].sequence, d[i].command);
        const char* own = NULL;
        g_auto(GStrv) tokens = sfe_keymap_parse(d[i].sequence, NULL);
        sfe_keymap_lookup(visual, tokens, g_strv_length(tokens), &own, NULL);
        assert_reachable(visual, normal, d[i].sequence, own ? own : d[i].command);
    }
    for (size_t i = 0; i < nv; i++)
        assert_reachable(visual, normal, v[i].sequence, v[i].command);

    /* The case that was broken: "d" waits (it is cut on its own after the
     * timeout), "dd" is the normal trash. */
    const char* command = NULL;
    bool prefix = false;
    char* d1[] = { "d", NULL };
    g_assert_cmpint(sfe_keymap_lookup_layered(visual, normal, d1, 1, &command, &prefix), ==, SFE_KEYS_MATCH);
    g_assert_cmpstr(command, ==, "cut");
    g_assert_true(prefix);
    assert_reachable(visual, normal, "dd", "trash");
    assert_reachable(visual, normal, "yy", "yank");
}

int main(int argc, char** argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/keymap/parse", test_parse);
    g_test_add_func("/keymap/token-format", test_token_and_format);
    g_test_add_func("/keymap/lookup", test_lookup);
    g_test_add_func("/keymap/overrides", test_overrides);
    g_test_add_func("/keymap/defaults", test_defaults);
    g_test_add_func("/keymap/continuations", test_continuations);
    g_test_add_func("/keymap/layered", test_layered);
    return g_test_run();
}
