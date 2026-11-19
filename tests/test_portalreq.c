#include "filefilter.h"
#include "portalreq.h"

#include <glib/gstdio.h>
#include <string.h>

/* The value after `option` in the arguments, or NULL. */
static const char* arg(const SfePortalRequest* r, const char* option)
{
    for (guint i = 0; i + 1 < r->args->len; i++) {
        if (strcmp(g_ptr_array_index(r->args, i), option) == 0)
            return g_ptr_array_index(r->args, i + 1);
    }
    return NULL;
}

static bool has_arg(const SfePortalRequest* r, const char* option)
{
    for (guint i = 0; i < r->args->len; i++) {
        if (strcmp(g_ptr_array_index(r->args, i), option) == 0)
            return true;
    }
    return false;
}

/* The filters the dialog will show, read back like sfe --choose does. */
static GPtrArray* dialog_filters(const SfePortalRequest* r)
{
    const char* text = arg(r, "--filters");
    g_assert_nonnull(text);
    g_autoptr(GError) error = NULL;
    GPtrArray* filters = sfe_file_filters_parse(text, &error);
    g_assert_no_error(error);
    return filters;
}

static SfePortalRequest* request(const char* method, const char* options)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) v = g_variant_parse(G_VARIANT_TYPE_VARDICT, options, NULL, NULL, &error);
    g_assert_no_error(error);
    return sfe_portal_request_new(method, "wayland:abc", "File Upload", v);
}

/* Zen / Firefox (GTK 3) for <input accept=".pdf,image/png">: each
 * extension made case-insensitive with [xX] classes, a MIME type, and
 * "All Files", the first one current. */
static void test_firefox_open(void)
{
    g_autoptr(SfePortalRequest) r = request("OpenFile",
        "{'filters': <[('PDF', [(uint32 0, '*.[pP][dD][fF]')]), ('PNG', [(uint32 1, 'image/png')]),"
        " ('All Files', [(uint32 0, '*')])]>,"
        " 'current_filter': <('PDF', [(uint32 0, '*.[pP][dD][fF]')])>, 'modal': <true>}");
    g_assert_cmpstr(g_ptr_array_index(r->args, 0), ==, "--choose");
    g_assert_cmpstr(g_ptr_array_index(r->args, 1), ==, "open");
    g_assert_cmpstr(arg(r, "--title"), ==, "File Upload");
    g_assert_cmpstr(arg(r, "--parent"), ==, "wayland:abc");
    g_assert_cmpstr(arg(r, "--current-filter"), ==, "0");
    g_autoptr(GPtrArray) filters = dialog_filters(r);
    g_assert_cmpuint(filters->len, ==, 3);
    const SfeFileFilter* pdf = g_ptr_array_index(filters, 0);
    g_assert_true(sfe_file_filter_shows(pdf, "Report.PDF", "application/pdf", false));
    g_assert_true(sfe_file_filter_shows(pdf, "report.pdf", NULL, false));
    g_assert_false(sfe_file_filter_shows(pdf, "notes.txt", "text/plain", false));
    g_assert_true(sfe_file_filter_shows(pdf, "Downloads", "inode/directory", true)); /* to walk into */
    const SfeFileFilter* png = g_ptr_array_index(filters, 1);
    g_assert_true(sfe_file_filter_shows(png, "x.png", "image/png", false));
    g_assert_false(sfe_file_filter_shows(png, "x.jpg", "image/jpeg", false));
}

/* Firefox's image filter: one pattern per extension of the list. */
static void test_firefox_images(void)
{
    g_autoptr(SfePortalRequest) r = request("OpenFile",
        "{'filters': <[('Image Files', [(uint32 0, '*.[jJ][pP][gG]'), (uint32 0, '*.[jJ][pP][eE][gG]'),"
        " (uint32 0, '*.[pP][nN][gG]'), (uint32 0, '*.[wW][eE][bB][pP]')])]>, 'multiple': <true>}");
    g_assert_true(has_arg(r, "--multiple"));
    g_assert_null(arg(r, "--current-filter")); /* none asked: the dialog starts with the first */
    g_autoptr(GPtrArray) filters = dialog_filters(r);
    const SfeFileFilter* images = g_ptr_array_index(filters, 0);
    g_assert_true(sfe_file_filter_shows(images, "IMG_0001.JPG", NULL, false));
    g_assert_true(sfe_file_filter_shows(images, "a.jpeg", NULL, false));
    g_assert_true(sfe_file_filter_shows(images, "b.WebP", NULL, false));
    g_assert_false(sfe_file_filter_shows(images, "c.gif", NULL, false));
    g_assert_false(sfe_file_filter_shows(images, "png", NULL, false));
}

/* <input webkitdirectory>: a folder dialog. Its file types (sent anyway)
 * must not reach sfe, and no file is listed. */
static void test_folder(void)
{
    g_autoptr(SfePortalRequest) r = request("OpenFile",
        "{'directory': <true>, 'multiple': <true>,"
        " 'filters': <[('All Files', [(uint32 0, '*')])]>, 'current_filter': <('All Files', [(uint32 0, '*')])>}");
    g_assert_cmpstr(g_ptr_array_index(r->args, 1), ==, "folder");
    g_assert_false(has_arg(r, "--filters"));
    g_assert_false(has_arg(r, "--multiple"));
    g_assert_null(r->filters);

    g_autoptr(SfeFileFilter) folders = sfe_file_filter_new_folders_only();
    g_assert_true(sfe_file_filter_shows(folders, "Pictures", "inode/directory", true));
    g_assert_false(sfe_file_filter_shows(folders, "notes.txt", "text/plain", false));
    g_assert_false(sfe_file_filter_shows(folders, "*", NULL, false));
}

/* current_filter without a list: the program wants only those files. */
static void test_current_filter_alone(void)
{
    g_autoptr(SfePortalRequest) r = request("OpenFile", "{'current_filter': <('Text', [(uint32 1, 'text/plain')])>}");
    g_assert_cmpstr(arg(r, "--current-filter"), ==, "0");
    g_autoptr(GPtrArray) filters = dialog_filters(r);
    g_assert_cmpuint(filters->len, ==, 1);
    g_assert_true(sfe_file_filter_shows(g_ptr_array_index(filters, 0), "a.txt", "text/plain", false));
    g_assert_false(sfe_file_filter_shows(g_ptr_array_index(filters, 0), "a.png", "image/png", false));

    g_autoptr(GVariant) results = sfe_portal_request_results(r, "#filter 0\nfile:///tmp/a.txt\n");
    g_autoptr(GVariant) current = g_variant_lookup_value(results, "current_filter", NULL);
    g_assert_nonnull(current);
    g_autofree char* text = g_variant_print(current, FALSE);
    g_assert_cmpstr(text, ==, "('Text', [(1, 'text/plain')])");
}

/* A current_filter missing from the list is added to its end. */
static void test_current_filter_added(void)
{
    g_autoptr(SfePortalRequest) r = request("OpenFile",
        "{'filters': <[('Images', [(uint32 1, 'image/*')])]>, 'current_filter': <('PDF', [(uint32 0, '*.pdf')])>}");
    g_assert_cmpstr(arg(r, "--current-filter"), ==, "1");
    g_autoptr(GPtrArray) filters = dialog_filters(r);
    g_assert_cmpuint(filters->len, ==, 2);
    g_assert_cmpstr(((SfeFileFilter*)g_ptr_array_index(filters, 1))->name, ==, "PDF");
}

static void test_save(void)
{
    g_autoptr(SfePortalRequest) r = request("SaveFile",
        "{'current_name': <'page.html'>, 'current_folder': <b'/home/alex/Downloads'>,"
        " 'filters': <[('HTML', [(uint32 0, '*.[hH][tT][mM][lL]')])]>}");
    g_assert_cmpstr(g_ptr_array_index(r->args, 1), ==, "save");
    g_assert_cmpstr(arg(r, "--name"), ==, "page.html");
    g_assert_cmpstr(arg(r, "--folder"), ==, "/home/alex/Downloads");
    g_assert_nonnull(arg(r, "--filters"));

    /* "Save as" over an existing file starts there */
    g_autoptr(SfePortalRequest) again = request("SaveFile", "{'current_file': <b'/home/alex/notes.txt'>}");
    g_assert_cmpstr(arg(again, "--folder"), ==, "/home/alex");
    g_assert_cmpstr(arg(again, "--name"), ==, "notes.txt");
}

static void test_save_files(void)
{
    g_autoptr(SfePortalRequest) r = request("SaveFiles", "{'files': <[b'/tmp/one.png', b'two.png']>}");
    g_assert_cmpstr(g_ptr_array_index(r->args, 1), ==, "folder");
    g_autoptr(GVariant) results = sfe_portal_request_results(r, "file:///home/alex/Pictures\n");
    const char** uris = NULL;
    g_assert_true(g_variant_lookup(results, "uris", "^a&s", &uris));
    g_assert_cmpuint(g_strv_length((char**)uris), ==, 2);
    g_assert_cmpstr(uris[0], ==, "file:///home/alex/Pictures/one.png");
    g_assert_cmpstr(uris[1], ==, "file:///home/alex/Pictures/two.png");
    g_free(uris);
}

/* Files the program would write over get free names instead. */
static void test_save_files_keep_existing(void)
{
    g_autofree char* dir = g_dir_make_tmp("sfe-portal-XXXXXX", NULL);
    g_autofree char* existing = g_build_filename(dir, "a.txt", NULL);
    g_assert_true(g_file_set_contents(existing, "mine", -1, NULL));
    g_autoptr(SfePortalRequest) r = request("SaveFiles", "{'files': <[b'a.txt', b'/x/a.txt', b'b']>}");
    g_autoptr(GFile) folder = g_file_new_for_path(dir);
    g_autofree char* folder_uri = g_file_get_uri(folder);
    g_autofree char* output = g_strconcat(folder_uri, "\n", NULL);
    g_autoptr(GVariant) results = sfe_portal_request_results(r, output);
    const char** uris = NULL;
    g_assert_true(g_variant_lookup(results, "uris", "^a&s", &uris));
    g_assert_cmpuint(g_strv_length((char**)uris), ==, 3);
    const char* expected[] = { "a (2).txt", "a (3).txt", "b" }; /* existing, then the first one's name */
    for (int i = 0; i < 3; i++) {
        g_autoptr(GFile) f = g_file_new_for_uri(uris[i]);
        g_autofree char* base = g_file_get_basename(f);
        g_assert_cmpstr(base, ==, expected[i]);
    }
    g_free(uris);
    g_remove(existing);
    g_rmdir(dir);
}

static void test_results(void)
{
    g_autoptr(SfePortalRequest) r = request("OpenFile",
        "{'filters': <[('A', [(uint32 0, '*.a')]), ('B', [(uint32 0, '*.b')])]>,"
        " 'choices': <[('enc', 'Encoding', [('utf8', 'UTF-8')], 'utf8')]>}");
    g_assert_nonnull(arg(r, "--choices"));
    g_autoptr(GVariant) results
        = sfe_portal_request_results(r, "#filter 1\n#choice enc utf8\n#choice note two words\n#other\nfile:///x.b\n");
    const char** uris = NULL;
    g_assert_true(g_variant_lookup(results, "uris", "^a&s", &uris));
    g_assert_cmpuint(g_strv_length((char**)uris), ==, 1); /* "#" lines are not files */
    g_assert_cmpstr(uris[0], ==, "file:///x.b");
    g_free(uris);
    g_autoptr(GVariant) current = g_variant_lookup_value(results, "current_filter", NULL);
    g_autofree char* current_text = g_variant_print(current, FALSE);
    g_assert_cmpstr(current_text, ==, "('B', [(0, '*.b')])");
    g_autoptr(GVariant) choices = g_variant_lookup_value(results, "choices", G_VARIANT_TYPE("a(ss)"));
    g_autofree char* choices_text = g_variant_print(choices, FALSE);
    g_assert_cmpstr(choices_text, ==, "[('enc', 'utf8'), ('note', 'two words')]");

    /* cancelled filter index, no choices: neither is answered */
    g_autoptr(GVariant) plain = sfe_portal_request_results(r, "#filter 7\nfile:///y\n");
    g_assert_null(g_variant_lookup_value(plain, "current_filter", NULL));
    g_assert_null(g_variant_lookup_value(plain, "choices", NULL));
}

int main(int argc, char** argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/portalreq/firefox-open", test_firefox_open);
    g_test_add_func("/portalreq/firefox-images", test_firefox_images);
    g_test_add_func("/portalreq/folder", test_folder);
    g_test_add_func("/portalreq/current-filter-alone", test_current_filter_alone);
    g_test_add_func("/portalreq/current-filter-added", test_current_filter_added);
    g_test_add_func("/portalreq/save", test_save);
    g_test_add_func("/portalreq/save-files", test_save_files);
    g_test_add_func("/portalreq/save-files-keep-existing", test_save_files_keep_existing);
    g_test_add_func("/portalreq/results", test_results);
    return g_test_run();
}
