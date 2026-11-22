#include "thumbs.h"

#include <glib/gstdio.h>
#include <string.h>

static char* root; /* temporary; also XDG_CACHE_HOME */

static char* in_root(const char* rel)
{
    return g_build_filename(root, rel, NULL);
}

static void write_text(const char* rel, const char* text)
{
    g_autofree char* path = in_root(rel);
    g_autofree char* dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_assert_true(g_file_set_contents(path, text, -1, NULL));
}

/* A `w` x `h` PNG at `rel`. */
static GFile* write_png(const char* rel, int w, int h)
{
    g_autofree char* path = in_root(rel);
    g_autofree char* dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_autoptr(GdkPixbuf) pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, w, h);
    gdk_pixbuf_fill(pixbuf, 0x3366ccff);
    g_assert_true(gdk_pixbuf_save(pixbuf, path, "png", NULL, NULL));
    return g_file_new_for_path(path);
}

static gint64 mtime_of(GFile* file)
{
    g_autoptr(GFileInfo) info = g_file_query_info(file, G_FILE_ATTRIBUTE_TIME_MODIFIED, 0, NULL, NULL);
    return (gint64)g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
}

/* The spec's own example. */
static void test_paths(void)
{
    g_autofree char* path = sfe_thumb_path("file:///home/jens/photos/me.png", SFE_THUMB_NORMAL);
    g_autofree char* expect = g_build_filename(root, "thumbnails", "normal", "c6ee772d9e49320e97ec29a7eb5b1697.png", NULL);
    g_assert_cmpstr(path, ==, expect);
    g_autofree char* fail = sfe_thumb_fail_path("file:///home/jens/photos/me.png");
    g_assert_true(strstr(fail, "/thumbnails/fail/sfe/c6ee772d") != NULL);

    g_assert_cmpint(sfe_thumb_size_for(48), ==, SFE_THUMB_NORMAL);
    g_assert_cmpint(sfe_thumb_size_for(128), ==, SFE_THUMB_NORMAL);
    g_assert_cmpint(sfe_thumb_size_for(200), ==, SFE_THUMB_LARGE);
    g_assert_cmpint(sfe_thumb_size_for(5000), ==, SFE_THUMB_XXLARGE);
    g_assert_cmpint(sfe_thumb_size_pixels(SFE_THUMB_LARGE), ==, 256);

    g_autofree char* cached = g_build_filename(root, "thumbnails", "large", "x.png", NULL);
    g_autoptr(GFile) inside = g_file_new_for_path(cached);
    g_autoptr(GFile) outside = g_file_new_for_path("/home/jens/photos/me.png");
    g_assert_true(sfe_thumb_in_cache_dir(inside));
    g_assert_false(sfe_thumb_in_cache_dir(outside));
}

/* An image: scaled to fit (never enlarged), cached with the spec's keys,
 * stale once the file changes. */
static void test_image(void)
{
    g_autoptr(GFile) file = write_png("pics/wide.png", 600, 300);
    g_autofree char* uri = g_file_get_uri(file);
    gint64 mtime = mtime_of(file);
    g_autoptr(GError) error = NULL;

    g_assert_true(sfe_thumb_supported(NULL, "image/png"));
    g_autoptr(GdkPixbuf) thumb = sfe_thumb_generate(NULL, file, "image/png", mtime, SFE_THUMB_NORMAL, NULL, &error);
    g_assert_no_error(error);
    g_assert_cmpint(gdk_pixbuf_get_width(thumb), ==, 128);
    g_assert_cmpint(gdk_pixbuf_get_height(thumb), ==, 64);

    g_autoptr(GdkPixbuf) cached = sfe_thumb_load_cached(uri, mtime, SFE_THUMB_NORMAL);
    g_assert_nonnull(cached);
    g_assert_cmpstr(gdk_pixbuf_get_option(cached, "tEXt::Thumb::URI"), ==, uri);
    g_autoptr(GdkPixbuf) stale = sfe_thumb_load_cached(uri, mtime + 1, SFE_THUMB_NORMAL);
    g_assert_null(stale);
    g_autofree char* path = sfe_thumb_path(uri, SFE_THUMB_NORMAL);
    GStatBuf st;
    g_assert_cmpint(g_stat(path, &st), ==, 0);
    g_assert_cmpint(st.st_mode & 0777, ==, 0600);

    /* Small images keep their size. */
    g_autoptr(GFile) small = write_png("pics/small.png", 40, 20);
    g_autoptr(GdkPixbuf) same = sfe_thumb_generate(NULL, small, "image/png", mtime_of(small), SFE_THUMB_LARGE, NULL, NULL);
    g_assert_cmpint(gdk_pixbuf_get_width(same), ==, 40);
}

/* A broken file fails once and is not tried again until it changes. */
static void test_failure(void)
{
    write_text("pics/broken.png", "not a png");
    g_autofree char* path = in_root("pics/broken.png");
    g_autoptr(GFile) file = g_file_new_for_path(path);
    g_autofree char* uri = g_file_get_uri(file);
    gint64 mtime = mtime_of(file);
    g_autoptr(GError) error = NULL;

    g_assert_false(sfe_thumb_has_failed(uri, mtime));
    g_autoptr(GdkPixbuf) none = sfe_thumb_generate(NULL, file, "image/png", mtime, SFE_THUMB_NORMAL, NULL, &error);
    g_assert_null(none);
    g_assert_nonnull(error);
    g_assert_true(sfe_thumb_has_failed(uri, mtime));
    g_assert_false(sfe_thumb_has_failed(uri, mtime + 10));
}

/* .thumbnailer files: TryExec decides, the first one for a type wins, the
 * Exec codes are expanded inside arguments. */
static void test_thumbnailers(void)
{
    write_text("data/thumbnailers/a-copy.thumbnailer",
        "[Thumbnailer Entry]\nTryExec=cp\nExec=cp %i %o\nMimeType=text/x-sfe-test;\n");
    write_text("data/thumbnailers/b-other.thumbnailer",
        "[Thumbnailer Entry]\nExec=cp --size=%s %u %o\nMimeType=text/x-sfe-test;text/x-sfe-other;\n");
    write_text("data/thumbnailers/c-missing.thumbnailer",
        "[Thumbnailer Entry]\nTryExec=/nonexistent/sfe-thumbnailer\nExec=x %i %o\nMimeType=text/x-sfe-missing;\n");
    g_autofree char* data = in_root("data");
    const char* dirs[] = { data, NULL };
    g_autoptr(GPtrArray) list = sfe_thumbnailers_load(dirs);
    g_assert_cmpuint(list->len, ==, 2);

    const SfeThumbnailer* t = sfe_thumbnailers_find(list, "text/x-sfe-test");
    g_assert_nonnull(t);
    g_assert_cmpstr(t->exec, ==, "cp %i %o");
    g_assert_nonnull(sfe_thumbnailers_find(list, "text/x-sfe-other"));
    g_assert_null(sfe_thumbnailers_find(list, "text/x-sfe-missing"));
    g_assert_null(sfe_thumbnailers_find(list, "text/plain"));

    SfeThumbnailer fancy = { .exec = "thumb -s %s --uri=%u %i %o 100%%" };
    g_auto(GStrv) argv = sfe_thumbnailer_argv(&fancy, "/a b/c.mkv", "file:///a%20b/c.mkv", "/tmp/o.png", 256, NULL);
    g_autofree char* joined = g_strjoinv("|", argv);
    g_assert_cmpstr(joined, ==, "thumb|-s|256|--uri=file:///a%20b/c.mkv|/a b/c.mkv|/tmp/o.png|100%");

    /* An external one makes the thumbnail ("cp" copies a PNG into place). */
    g_autoptr(GFile) file = write_png("docs/page.sfetest", 300, 300);
    g_autoptr(GdkPixbuf) thumb = sfe_thumb_generate(list, file, "text/x-sfe-test", mtime_of(file), SFE_THUMB_NORMAL,
        NULL, NULL);
    g_assert_nonnull(thumb);
    g_assert_cmpint(gdk_pixbuf_get_width(thumb), ==, 128);
    g_assert_true(sfe_thumb_supported(list, "text/x-sfe-test"));
    g_assert_false(sfe_thumb_supported(list, "text/x-sfe-missing"));
}

/* A thumbnailer runs in the sandbox (when bubblewrap works here): it can
 * write its output, and nothing else. */
static void test_thumbnailer_sandbox(void)
{
    char* cmd[] = { "cp", "a", "b", NULL };
    g_auto(GStrv) wrapped = sfe_thumb_sandboxed(cmd, "/tmp/in.pdf", "/tmp/sfe-thumb-x");
    g_autofree char* joined = g_strjoinv(" ", wrapped);
    g_assert_true(g_str_has_prefix(joined, "bwrap --ro-bind / / "));
    g_assert_nonnull(strstr(joined, "--tmpfs /tmp --ro-bind /tmp/in.pdf /tmp/in.pdf --bind /tmp/sfe-thumb-x /tmp/sfe-thumb-x --unshare-all"));
    g_assert_true(g_str_has_suffix(joined, " -- cp a b"));

    if (!sfe_thumb_sandbox_works()) {
        g_test_skip("bubblewrap does not work here");
        return;
    }
    write_text("sandbox/thumbnailers/escape.thumbnailer",
        "[Thumbnailer Entry]\nExec=sh -c \"cp %i %o; echo x > %i.escaped\"\nMimeType=text/x-sfe-escape;\n");
    g_autofree char* data = in_root("sandbox");
    const char* dirs[] = { data, NULL };
    g_autoptr(GPtrArray) list = sfe_thumbnailers_load(dirs);
    g_autoptr(GFile) file = write_png("sandbox/files/page.sfeescape", 300, 300);
    g_autoptr(GdkPixbuf) thumb = sfe_thumb_generate(list, file, "text/x-sfe-escape", mtime_of(file), SFE_THUMB_NORMAL,
        NULL, NULL);
    g_assert_nonnull(thumb); /* its output came through */
    g_autofree char* escaped = in_root("sandbox/files/page.sfeescape.escaped");
    g_assert_false(g_file_test(escaped, G_FILE_TEST_EXISTS)); /* nothing else was written */
}

typedef struct {
    int done;
    int with_pixbuf;
    GMainLoop* loop;
} Count;

static void on_done(GFile* file, GdkPixbuf* pixbuf, gpointer data)
{
    (void)file;
    Count* c = data;
    c->done++;
    c->with_pixbuf += pixbuf != NULL;
    if (c->done == 2)
        g_main_loop_quit(c->loop);
}

static void on_never(GFile* file, GdkPixbuf* pixbuf, gpointer data)
{
    (void)file, (void)pixbuf, (void)data;
    g_assert_not_reached();
}

/* The pool: results on the main thread; one job per file even when asked
 * twice; cancelled requests are never answered. */
static void test_queue(void)
{
    g_autoptr(GPtrArray) none = g_ptr_array_new();
    SfeThumbQueue* q = sfe_thumb_queue_new(2, none);
    g_autoptr(GFile) a = write_png("queue/a.png", 500, 500);
    g_autoptr(GFile) b = write_png("queue/b.png", 500, 500);
    Count count = { .loop = g_main_loop_new(NULL, FALSE) };
    g_autoptr(GCancellable) gone = g_cancellable_new();
    g_cancellable_cancel(gone);

    sfe_thumb_queue_request(q, a, "image/png", mtime_of(a), SFE_THUMB_NORMAL, gone, on_never, NULL);
    sfe_thumb_queue_request(q, a, "image/png", mtime_of(a), SFE_THUMB_NORMAL, NULL, on_done, &count);
    sfe_thumb_queue_request(q, b, "image/png", mtime_of(b), SFE_THUMB_NORMAL, NULL, on_done, &count);
    g_main_loop_run(count.loop);
    g_assert_cmpint(count.done, ==, 2);
    g_assert_cmpint(count.with_pixbuf, ==, 2);
    g_main_loop_unref(count.loop);
}

static char* cover_name(const char* dir_rel)
{
    g_autofree char* path = in_root(dir_rel);
    g_autoptr(GFile) dir = g_file_new_for_path(path);
    g_autoptr(GFile) cover = sfe_folder_cover_find(dir, NULL);
    return cover ? g_file_get_basename(cover) : NULL;
}

/* The best-named picture wins; other pictures and non-pictures do not count. */
static void test_folder_cover(void)
{
    write_text("album/01 Song.flac", "x");
    write_text("album/cover.txt", "not a picture");
    g_autofree char* none = cover_name("album");
    g_assert_null(none);

    g_object_unref(write_png("album/AlbumArt_{1234}_Large.jpg", 8, 8)); /* (a PNG named .jpg: names only) */
    g_autofree char* wmp = cover_name("album");
    g_assert_cmpstr(wmp, ==, "AlbumArt_{1234}_Large.jpg");
    g_object_unref(write_png("album/Folder.PNG", 8, 8));
    g_autofree char* folder = cover_name("album");
    g_assert_cmpstr(folder, ==, "Folder.PNG");
    g_object_unref(write_png("album/cover.webp", 8, 8));
    g_autofree char* cover = cover_name("album");
    g_assert_cmpstr(cover, ==, "cover.webp");

    g_object_unref(write_png("photos/holiday.png", 8, 8)); /* any other picture is not a cover */
    g_autofree char* other = cover_name("photos");
    g_assert_null(other);
    g_autofree char* sub = in_root("nested/cover.jpg");
    g_mkdir_with_parents(sub, 0755); /* a folder named like a cover */
    g_autofree char* dir = cover_name("nested");
    g_assert_null(dir);
}

/* A folder request delivers the cover's thumbnail as the folder's. */
static void on_folder_done(GFile* file, GdkPixbuf* pixbuf, gpointer data)
{
    Count* c = data;
    g_autofree char* name = g_file_get_basename(file);
    g_assert_true(strcmp(name, "music") == 0 || strcmp(name, "empty") == 0);
    c->done++;
    c->with_pixbuf += pixbuf != NULL;
    if (c->done == 2)
        g_main_loop_quit(c->loop);
}

static void test_folder_queue(void)
{
    g_autoptr(GPtrArray) none = g_ptr_array_new();
    SfeThumbQueue* q = sfe_thumb_queue_new(1, none);
    g_autoptr(GFile) art = write_png("music/front.png", 300, 300);
    write_text("empty/notes.txt", "x");
    g_autofree char* music_path = in_root("music");
    g_autofree char* empty_path = in_root("empty");
    g_autoptr(GFile) music = g_file_new_for_path(music_path);
    g_autoptr(GFile) empty = g_file_new_for_path(empty_path);
    Count count = { .loop = g_main_loop_new(NULL, FALSE) };

    sfe_thumb_queue_request_folder(q, music, mtime_of(music), SFE_THUMB_NORMAL, NULL, on_folder_done, &count);
    sfe_thumb_queue_request_folder(q, empty, mtime_of(empty), SFE_THUMB_NORMAL, NULL, on_folder_done, &count);
    g_main_loop_run(count.loop);
    g_assert_cmpint(count.with_pixbuf, ==, 1);
    g_autofree char* uri = g_file_get_uri(art); /* cached under the picture's own URI */
    g_autoptr(GdkPixbuf) cached = sfe_thumb_load_cached(uri, mtime_of(art), SFE_THUMB_NORMAL);
    g_assert_nonnull(cached);
    g_main_loop_unref(count.loop);
}

int main(int argc, char** argv)
{
    root = g_dir_make_tmp("sfe-thumbs-XXXXXX", NULL);
    g_setenv("XDG_CACHE_HOME", root, TRUE); /* before GLib reads it */
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/thumbs/paths", test_paths);
    g_test_add_func("/thumbs/image", test_image);
    g_test_add_func("/thumbs/failure", test_failure);
    g_test_add_func("/thumbs/thumbnailers", test_thumbnailers);
    g_test_add_func("/thumbs/thumbnailer-sandbox", test_thumbnailer_sandbox);
    g_test_add_func("/thumbs/queue", test_queue);
    g_test_add_func("/thumbs/folder-cover", test_folder_cover);
    g_test_add_func("/thumbs/folder-queue", test_folder_queue);
    int status = g_test_run();

    g_autofree char* cmd = g_strdup_printf("rm -rf '%s'", root);
    g_spawn_command_line_sync(cmd, NULL, NULL, NULL, NULL);
    g_free(root);
    return status;
}
