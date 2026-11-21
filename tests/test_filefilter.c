#include "filefilter.h"

static void test_parse_and_match(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GPtrArray) filters = sfe_file_filters_parse(
        "[('Images', [(0, '*.png'), (0, '*.JPG'), (1, 'image/webp')]), ('Text', [(1, 'text/*')]), ('All', [(0, '*')])]",
        &error);
    g_assert_no_error(error);
    g_assert_cmpuint(filters->len, ==, 3);
    const SfeFileFilter* images = g_ptr_array_index(filters, 0);
    const SfeFileFilter* text = g_ptr_array_index(filters, 1);
    const SfeFileFilter* all = g_ptr_array_index(filters, 2);
    g_assert_cmpstr(images->name, ==, "Images");

    g_assert_true(sfe_file_filter_matches(images, "photo.PNG", NULL)); /* globs ignore case */
    g_assert_true(sfe_file_filter_matches(images, "photo.jpg", NULL));
    g_assert_true(sfe_file_filter_matches(images, "x", "image/webp"));
    g_assert_false(sfe_file_filter_matches(images, "notes.txt", "text/plain"));
    g_assert_true(sfe_file_filter_matches(text, "notes", "text/plain"));
    g_assert_true(sfe_file_filter_matches(text, "page", "text/html"));
    g_assert_false(sfe_file_filter_matches(text, "a.png", "image/png"));
    g_assert_true(sfe_file_filter_matches(all, "anything", NULL));
    g_assert_true(sfe_file_filter_matches(all, ".hidden", NULL));
}

/* Character classes, as Chromium and the portal spec write any-case globs. */
static void test_classes(void)
{
    g_autoptr(GPtrArray) filters = sfe_file_filters_parse(
        "[('PNG', [(0, '*.[pP][nN][gG]')]), ('Logs', [(0, 'log-[0-9]*.txt')])]", NULL);
    const SfeFileFilter* png = g_ptr_array_index(filters, 0);
    const SfeFileFilter* logs = g_ptr_array_index(filters, 1);
    g_assert_true(sfe_file_filter_matches(png, "a.png", NULL));
    g_assert_true(sfe_file_filter_matches(png, "a.PnG", NULL));
    g_assert_false(sfe_file_filter_matches(png, "a.pngx", NULL));
    g_assert_false(sfe_file_filter_matches(png, "a.jpg", NULL));
    g_assert_true(sfe_file_filter_matches(logs, "log-3.txt", NULL));
    g_assert_false(sfe_file_filter_matches(logs, "log-x.txt", NULL));
}

static void test_errors(void)
{
    g_autoptr(GError) error = NULL;
    g_assert_null(sfe_file_filters_parse("not a variant", &error));
    g_assert_nonnull(error);
    g_autoptr(GPtrArray) none = sfe_file_filters_parse("@a(sa(us)) []", NULL);
    g_assert_cmpuint(none->len, ==, 0);

    /* Found by fuzzing: invalid UTF-8 in a pattern made GLib log a critical. */
    g_autoptr(GError) utf8_error = NULL;
    g_assert_null(sfe_file_filters_parse("[('Images', [(uint32 0, '*.\333\321g')])]", &utf8_error));
    g_assert_nonnull(utf8_error);
}

int main(int argc, char** argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/filefilter/parse-match", test_parse_and_match);
    g_test_add_func("/filefilter/classes", test_classes);
    g_test_add_func("/filefilter/errors", test_errors);
    return g_test_run();
}
