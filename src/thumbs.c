#include "thumbs.h"
#include "util.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

#define FAIL_DIR "sfe" /* ~/.cache/thumbnails/fail/sfe */
#define TIMEOUT_S 30 /* a thumbnailer taking longer is stopped */

static const char* const size_dirs[] = { "normal", "large", "x-large", "xx-large" };
static const int size_pixels[] = { 128, 256, 512, 1024 };

int sfe_thumb_size_pixels(SfeThumbSize size)
{
    return size_pixels[size];
}

SfeThumbSize sfe_thumb_size_for(int pixels)
{
    for (int s = SFE_THUMB_NORMAL; s < SFE_THUMB_XXLARGE; s++) {
        if (pixels <= size_pixels[s])
            return s;
    }
    return SFE_THUMB_XXLARGE;
}

/* ------------------------------------------------------------------------ */
/* The cache                                                                 */
/* ------------------------------------------------------------------------ */

static char* cache_root(void)
{
    return g_build_filename(g_get_user_cache_dir(), "thumbnails", NULL);
}

static char* md5_name(const char* uri)
{
    g_autofree char* md5 = g_compute_checksum_for_string(G_CHECKSUM_MD5, uri, -1);
    return g_strconcat(md5, ".png", NULL);
}

char* sfe_thumb_path(const char* uri, SfeThumbSize size)
{
    g_autofree char* root = cache_root();
    g_autofree char* name = md5_name(uri);
    return g_build_filename(root, size_dirs[size], name, NULL);
}

char* sfe_thumb_fail_path(const char* uri)
{
    g_autofree char* root = cache_root();
    g_autofree char* name = md5_name(uri);
    return g_build_filename(root, "fail", FAIL_DIR, name, NULL);
}

bool sfe_thumb_in_cache_dir(GFile* file)
{
    g_autofree char* root = cache_root();
    g_autoptr(GFile) dir = g_file_new_for_path(root);
    return g_file_has_prefix(file, dir);
}

/* The PNG at `path` if it belongs to this version of `uri`. */
static GdkPixbuf* load_matching(const char* path, const char* uri, gint64 mtime)
{
    if (!g_file_test(path, G_FILE_TEST_IS_REGULAR))
        return NULL;
    g_autoptr(GdkPixbuf) pixbuf = gdk_pixbuf_new_from_file(path, NULL);
    if (pixbuf == NULL)
        return NULL;
    const char* thumb_uri = gdk_pixbuf_get_option(pixbuf, "tEXt::Thumb::URI");
    const char* thumb_mtime = gdk_pixbuf_get_option(pixbuf, "tEXt::Thumb::MTime");
    if (g_strcmp0(thumb_uri, uri) != 0 || thumb_mtime == NULL || g_ascii_strtoll(thumb_mtime, NULL, 10) != mtime)
        return NULL;
    return g_steal_pointer(&pixbuf);
}

GdkPixbuf* sfe_thumb_load_cached(const char* uri, gint64 mtime, SfeThumbSize size)
{
    g_autofree char* path = sfe_thumb_path(uri, size);
    return load_matching(path, uri, mtime);
}

bool sfe_thumb_has_failed(const char* uri, gint64 mtime)
{
    g_autofree char* path = sfe_thumb_fail_path(uri);
    g_autoptr(GdkPixbuf) marker = load_matching(path, uri, mtime);
    return marker != NULL;
}

/* Writes `pixbuf` with the spec's keys, atomically and private (0600). */
static bool save(GdkPixbuf* pixbuf, const char* path, const char* uri, gint64 mtime, guint64 size, GError** error)
{
    g_autofree char* dir = g_path_get_dirname(path);
    if (g_mkdir_with_parents(dir, 0700) != 0) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "cannot create %s", dir);
        return false;
    }
    g_autofree char* tmp = g_strdup_printf("%s.sfe-%u.tmp", path, g_random_int());
    g_autofree char* mtime_text = g_strdup_printf("%" G_GINT64_FORMAT, mtime);
    g_autofree char* size_text = g_strdup_printf("%" G_GUINT64_FORMAT, size);
    char* keys[] = { "tEXt::Thumb::URI", "tEXt::Thumb::MTime", "tEXt::Thumb::Size", "tEXt::Software", NULL };
    char* values[] = { (char*)uri, mtime_text, size_text, "sfe", NULL };
    if (!gdk_pixbuf_savev(pixbuf, tmp, "png", keys, values, error))
        return false;
    g_chmod(tmp, 0600);
    if (g_rename(tmp, path) != 0) {
        g_unlink(tmp);
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "cannot write %s", path);
        return false;
    }
    return true;
}

static void record_failure(const char* uri, gint64 mtime)
{
    g_autoptr(GdkPixbuf) marker = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, 1, 1);
    gdk_pixbuf_fill(marker, 0);
    g_autofree char* path = sfe_thumb_fail_path(uri);
    save(marker, path, uri, mtime, 0, NULL);
}

/* ------------------------------------------------------------------------ */
/* Thumbnailers                                                              */
/* ------------------------------------------------------------------------ */

static void thumbnailer_free(gpointer p)
{
    SfeThumbnailer* t = p;
    g_free(t->exec);
    g_strfreev(t->mime_types);
    g_free(t);
}

static bool program_exists(const char* program)
{
    if (g_path_is_absolute(program))
        return g_file_test(program, G_FILE_TEST_IS_EXECUTABLE);
    g_autofree char* found = g_find_program_in_path(program);
    return found != NULL;
}

static int compare_names(gconstpointer a, gconstpointer b)
{
    return strcmp(*(char* const*)a, *(char* const*)b);
}

static void load_dir(GPtrArray* out, const char* dir)
{
    g_autofree char* path = g_build_filename(dir, "thumbnailers", NULL);
    g_autoptr(GDir) d = g_dir_open(path, 0, NULL);
    const char* name;
    g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);

    while (d != NULL && (name = g_dir_read_name(d)) != NULL) {
        if (g_str_has_suffix(name, ".thumbnailer"))
            g_ptr_array_add(names, g_strdup(name));
    }
    g_ptr_array_sort(names, compare_names);
    for (guint i = 0; i < names->len; i++) {
        g_autofree char* file = g_build_filename(path, g_ptr_array_index(names, i), NULL);
        g_autoptr(GKeyFile) kf = g_key_file_new();
        if (!g_key_file_load_from_file(kf, file, G_KEY_FILE_NONE, NULL))
            continue;
        g_autofree char* exec = g_key_file_get_string(kf, "Thumbnailer Entry", "Exec", NULL);
        g_autofree char* try_exec = g_key_file_get_string(kf, "Thumbnailer Entry", "TryExec", NULL);
        char** types = g_key_file_get_string_list(kf, "Thumbnailer Entry", "MimeType", NULL, NULL);
        g_auto(GStrv) argv = NULL;
        bool usable = exec != NULL && types != NULL;
        if (usable && try_exec != NULL)
            usable = program_exists(try_exec);
        if (usable && try_exec == NULL) { /* no TryExec: the program of Exec */
            usable = g_shell_parse_argv(exec, NULL, &argv, NULL) && program_exists(argv[0]);
        }
        if (!usable) {
            g_strfreev(types);
            continue;
        }
        SfeThumbnailer* t = g_new0(SfeThumbnailer, 1);
        t->exec = g_steal_pointer(&exec);
        t->mime_types = types;
        g_ptr_array_add(out, t);
    }
}

GPtrArray* sfe_thumbnailers_load(const char* const* data_dirs)
{
    GPtrArray* out = g_ptr_array_new_with_free_func(thumbnailer_free);
    if (data_dirs != NULL) {
        for (int i = 0; data_dirs[i] != NULL; i++)
            load_dir(out, data_dirs[i]);
        return out;
    }
    load_dir(out, g_get_user_data_dir());
    const char* const* system = g_get_system_data_dirs();
    for (int i = 0; system[i] != NULL; i++)
        load_dir(out, system[i]);
    return out;
}

const SfeThumbnailer* sfe_thumbnailers_find(GPtrArray* thumbnailers, const char* content_type)
{
    if (thumbnailers == NULL || content_type == NULL)
        return NULL;
    g_autofree char* mime = g_content_type_get_mime_type(content_type);
    for (guint i = 0; i < thumbnailers->len; i++) {
        const SfeThumbnailer* t = g_ptr_array_index(thumbnailers, i);
        for (int k = 0; t->mime_types[k] != NULL; k++) {
            if (g_strcmp0(t->mime_types[k], mime) == 0 || g_strcmp0(t->mime_types[k], content_type) == 0)
                return t;
        }
    }
    return NULL;
}

char** sfe_thumbnailer_argv(const SfeThumbnailer* t, const char* path, const char* uri, const char* output, int size,
    GError** error)
{
    g_auto(GStrv) words = NULL;
    if (!g_shell_parse_argv(t->exec, NULL, &words, error))
        return NULL;
    GPtrArray* argv = g_ptr_array_new();
    for (int i = 0; words[i] != NULL; i++) {
        GString* arg = g_string_new(NULL);
        for (const char* p = words[i]; *p != '\0'; p++) {
            if (*p != '%' || p[1] == '\0') {
                g_string_append_c(arg, *p);
                continue;
            }
            switch (*++p) {
            case 'i': g_string_append(arg, path); break;
            case 'u': g_string_append(arg, uri); break;
            case 'o': g_string_append(arg, output); break;
            case 's': g_string_append_printf(arg, "%d", size); break;
            case '%': g_string_append_c(arg, '%'); break;
            default: break; /* unknown codes are dropped */
            }
        }
        g_ptr_array_add(argv, g_string_free(arg, FALSE));
    }
    g_ptr_array_add(argv, NULL);
    return (char**)g_ptr_array_free(argv, FALSE);
}

/* ------------------------------------------------------------------------ */
/* Making thumbnails                                                         */
/* ------------------------------------------------------------------------ */

/* The MIME types gdk-pixbuf can read (computed once). */
static bool pixbuf_reads(const char* content_type)
{
    static GHashTable* types;
    static GMutex lock;
    g_mutex_lock(&lock);
    if (types == NULL) {
        types = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        GSList* formats = gdk_pixbuf_get_formats();
        for (GSList* l = formats; l != NULL; l = l->next) {
            if (gdk_pixbuf_format_is_disabled(l->data))
                continue;
            g_auto(GStrv) mimes = gdk_pixbuf_format_get_mime_types(l->data);
            for (int i = 0; mimes != NULL && mimes[i] != NULL; i++)
                g_hash_table_add(types, g_strdup(mimes[i]));
        }
        g_slist_free(formats);
    }
    g_autofree char* mime = content_type ? g_content_type_get_mime_type(content_type) : NULL;
    bool ok = mime != NULL && g_hash_table_contains(types, mime);
    g_mutex_unlock(&lock);
    return ok;
}

static bool is_video(const char* content_type)
{
    g_autofree char* mime = content_type ? g_content_type_get_mime_type(content_type) : NULL;
    return mime != NULL && g_str_has_prefix(mime, "video/");
}

static bool have_ffmpeg(void)
{
    static gsize checked;
    static bool found;
    if (g_once_init_enter(&checked)) {
        found = program_exists("ffmpeg");
        g_once_init_leave(&checked, 1);
    }
    return found;
}

bool sfe_thumb_supported(GPtrArray* thumbnailers, const char* content_type)
{
    return pixbuf_reads(content_type) || sfe_thumbnailers_find(thumbnailers, content_type) != NULL
        || (is_video(content_type) && have_ffmpeg());
}

/* Fits `pixbuf` into a `size` square, never enlarging (the spec). */
static GdkPixbuf* fit(GdkPixbuf* pixbuf, int size)
{
    int w = gdk_pixbuf_get_width(pixbuf);
    int h = gdk_pixbuf_get_height(pixbuf);
    if (w <= size && h <= size)
        return g_object_ref(pixbuf);
    double k = MIN((double)size / w, (double)size / h);
    return gdk_pixbuf_scale_simple(pixbuf, MAX(1, (int)(w * k + 0.5)), MAX(1, (int)(h * k + 0.5)),
        GDK_INTERP_BILINEAR);
}

static gboolean cancel_later(gpointer data)
{
    g_cancellable_cancel(data);
    return G_SOURCE_REMOVE;
}

static void forward_cancel(GCancellable* cancel, gpointer data)
{
    (void)cancel;
    g_cancellable_cancel(data);
}

/* Runs a program, stopping it after TIMEOUT_S or when `cancel` fires. */
static bool run(char** argv, GCancellable* cancel, GError** error)
{
    g_autoptr(GSubprocess) proc = g_subprocess_newv((const char* const*)argv,
        G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, error);
    if (proc == NULL)
        return false;
    g_autoptr(GCancellable) stop = g_cancellable_new();
    gulong forward = cancel ? g_cancellable_connect(cancel, G_CALLBACK(forward_cancel), g_object_ref(stop), g_object_unref) : 0;
    GSource* timer = g_timeout_source_new_seconds(TIMEOUT_S);
    g_source_set_callback(timer, cancel_later, g_object_ref(stop), g_object_unref);
    g_source_attach(timer, NULL); /* the main loop's context */

    bool ok = g_subprocess_wait_check(proc, stop, error);
    if (!ok && g_cancellable_is_cancelled(stop))
        g_subprocess_force_exit(proc);
    g_source_destroy(timer);
    g_source_unref(timer);
    if (cancel != NULL)
        g_cancellable_disconnect(cancel, forward);
    return ok;
}

/* A program writes a PNG (or JPEG) to a temporary file, which is loaded. */
static GdkPixbuf* from_program(char** argv, const char* output, int size, GCancellable* cancel, GError** error)
{
    bool ok = run(argv, cancel, error);
    GdkPixbuf* pixbuf = NULL;
    if (ok) {
        g_autoptr(GdkPixbuf) raw = gdk_pixbuf_new_from_file(output, error);
        pixbuf = raw ? fit(raw, size) : NULL;
    }
    g_unlink(output);
    return pixbuf;
}

static char* temp_output(void)
{
    g_autofree char* name = g_strdup_printf("sfe-thumb-%u-%u.png", (unsigned)getpid(), g_random_int());
    return g_build_filename(g_get_tmp_dir(), name, NULL);
}

/* A frame a few seconds in (the very first is often black), else the first. */
static GdkPixbuf* video_frame(const char* path, int size, GCancellable* cancel, GError** error)
{
    static const char* const seeks[] = { "5", "0" };
    g_autofree char* scale = g_strdup_printf("thumbnail,scale=%d:%d:force_original_aspect_ratio=decrease", size, size);
    GError* last = NULL;
    for (size_t i = 0; i < G_N_ELEMENTS(seeks); i++) {
        g_autofree char* output = temp_output();
        char* argv[] = { "ffmpeg", "-nostdin", "-y", "-loglevel", "error", "-ss", (char*)seeks[i], "-i", (char*)path,
            "-frames:v", "1", "-vf", scale, "-f", "image2", "-c:v", "png", output, NULL };
        g_clear_error(&last);
        GdkPixbuf* pixbuf = from_program(argv, output, size, cancel, &last);
        if (pixbuf != NULL)
            return pixbuf;
        if (g_cancellable_is_cancelled(cancel))
            break;
    }
    g_propagate_error(error, last);
    return NULL;
}

static GdkPixbuf* make(GPtrArray* thumbnailers, const char* path, const char* uri, const char* content_type, int size,
    GCancellable* cancel, GError** error)
{
    if (pixbuf_reads(content_type)) {
        int w = 0, h = 0;
        gdk_pixbuf_get_file_info(path, &w, &h);
        bool small = w > 0 && h > 0 && w <= size && h <= size; /* loading "at size" would enlarge it */
        g_autoptr(GdkPixbuf) raw = small ? gdk_pixbuf_new_from_file(path, error)
                                         : gdk_pixbuf_new_from_file_at_size(path, size, size, error);
        if (raw == NULL)
            return NULL;
        g_autoptr(GdkPixbuf) turned = gdk_pixbuf_apply_embedded_orientation(raw); /* camera photos */
        return fit(turned ? turned : raw, size);
    }
    const SfeThumbnailer* t = sfe_thumbnailers_find(thumbnailers, content_type);
    if (t != NULL) {
        g_autofree char* output = temp_output();
        g_auto(GStrv) argv = sfe_thumbnailer_argv(t, path, uri, output, size, error);
        return argv ? from_program(argv, output, size, cancel, error) : NULL;
    }
    if (is_video(content_type) && have_ffmpeg())
        return video_frame(path, size, cancel, error);
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "no thumbnailer for %s", content_type);
    return NULL;
}

GdkPixbuf* sfe_thumb_generate(GPtrArray* thumbnailers, GFile* file, const char* content_type, gint64 mtime,
    SfeThumbSize size, GCancellable* cancel, GError** error)
{
    g_autofree char* path = g_file_get_path(file);
    g_autofree char* uri = g_file_get_uri(file);
    if (path == NULL) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "only local files get thumbnails");
        return NULL;
    }
    GError* local = NULL;
    GdkPixbuf* pixbuf = make(thumbnailers, path, uri, content_type, size_pixels[size], cancel, &local);
    if (pixbuf == NULL) {
        if (!g_cancellable_is_cancelled(cancel)) /* stopped, not broken: try again next time */
            record_failure(uri, mtime);
        g_propagate_error(error, local);
        return NULL;
    }
    GStatBuf st;
    guint64 bytes = g_stat(path, &st) == 0 ? (guint64)st.st_size : 0;
    g_autofree char* thumb = sfe_thumb_path(uri, size);
    save(pixbuf, thumb, uri, mtime, bytes, NULL); /* a read-only cache still shows it */
    return pixbuf;
}

/* ------------------------------------------------------------------------ */
/* Folder covers                                                             */
/* ------------------------------------------------------------------------ */

#define COVER_SCAN_LIMIT 5000 /* entries read before giving up on a folder */

/* How good a name is as a cover: lower is better, -1 for not a cover. */
static int cover_rank(const char* name)
{
    static const char* const stems[] = { "cover", "folder", "front", "albumart", "album", ".folder" };
    static const char* const exts[] = { ".jpg", ".jpeg", ".png", ".webp", ".gif", ".bmp" };
    g_autofree char* lower = g_utf8_strdown(name, -1);
    const char* dot = strrchr(lower, '.');
    if (dot == NULL || dot == lower)
        return -1;
    bool image = false;
    for (size_t i = 0; i < G_N_ELEMENTS(exts) && !image; i++)
        image = strcmp(dot, exts[i]) == 0;
    if (!image)
        return -1;
    g_autofree char* stem = g_strndup(lower, dot - lower);
    for (size_t i = 0; i < G_N_ELEMENTS(stems); i++) {
        if (strcmp(stem, stems[i]) == 0)
            return (int)i * 2;
        /* "AlbumArt_{GUID}_Large", "AlbumArtSmall" (Windows Media Player) */
        if (strcmp(stems[i], "albumart") == 0 && g_str_has_prefix(stem, "albumart"))
            return (int)i * 2 + 1;
    }
    return -1;
}

GFile* sfe_folder_cover_find(GFile* dir, GCancellable* cancel)
{
    g_autoptr(GFileEnumerator) en = g_file_enumerate_children(dir,
        G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NONE, cancel, NULL);
    g_autofree char* best = NULL;
    int best_rank = G_MAXINT;
    GFileInfo* info;
    for (int n = 0; en != NULL && n < COVER_SCAN_LIMIT && best_rank > 0
         && (info = g_file_enumerator_next_file(en, cancel, NULL)) != NULL;
         n++) {
        const char* name = g_file_info_get_name(info);
        int rank = g_file_info_get_file_type(info) == G_FILE_TYPE_REGULAR ? cover_rank(name) : -1;
        if (rank >= 0 && (rank < best_rank || (rank == best_rank && g_strcmp0(name, best) < 0))) {
            best_rank = rank;
            g_free(best);
            best = g_strdup(name);
        }
        g_object_unref(info);
    }
    return best ? g_file_get_child(dir, best) : NULL;
}

/* ------------------------------------------------------------------------ */
/* The worker pool                                                           */
/* ------------------------------------------------------------------------ */

typedef struct {
    SfeThumbDone done;
    gpointer data;
    GCancellable* cancel;
} Waiter;

typedef struct {
    SfeThumbQueue* queue;
    char* key; /* "size uri" */
    GFile* file;
    char* uri;
    char* content_type;
    gint64 mtime;
    SfeThumbSize size;
    bool folder; /* `file` is a folder: thumbnail its cover */
    guint64 seq; /* later requests first */
    GSList* waiters; /* Waiter*, guarded by the queue's lock */
    GdkPixbuf* result;
} Job;

struct SfeThumbQueue {
    GThreadPool* pool;
    GMainContext* context; /* where results are delivered */
    GPtrArray* thumbnailers;
    GMutex lock;
    GHashTable* jobs; /* key -> Job*, queued or running */
    guint64 seq;
};

static void waiter_free(gpointer p)
{
    Waiter* w = p;
    g_clear_object(&w->cancel);
    g_free(w);
}

static void job_free(Job* job)
{
    g_free(job->key);
    g_object_unref(job->file);
    g_free(job->uri);
    g_free(job->content_type);
    g_slist_free_full(job->waiters, waiter_free);
    g_clear_object(&job->result);
    g_free(job);
}

static bool all_cancelled(Job* job)
{
    for (GSList* l = job->waiters; l != NULL; l = l->next) {
        Waiter* w = l->data;
        if (w->cancel == NULL || !g_cancellable_is_cancelled(w->cancel))
            return false;
    }
    return true;
}

/* Main thread: hands the result to everyone still waiting. */
static gboolean deliver(gpointer data)
{
    Job* job = data;
    SfeThumbQueue* q = job->queue;
    g_mutex_lock(&q->lock);
    if (g_hash_table_lookup(q->jobs, job->key) == job)
        g_hash_table_remove(q->jobs, job->key);
    GSList* waiters = g_steal_pointer(&job->waiters);
    g_mutex_unlock(&q->lock);

    for (GSList* l = waiters; l != NULL; l = l->next) {
        Waiter* w = l->data;
        if (w->cancel == NULL || !g_cancellable_is_cancelled(w->cancel))
            w->done(job->file, job->result, w->data);
    }
    g_slist_free_full(waiters, waiter_free);
    job_free(job);
    return G_SOURCE_REMOVE;
}

static void work(gpointer data, gpointer user_data)
{
    Job* job = data;
    SfeThumbQueue* q = user_data;

    g_mutex_lock(&q->lock);
    bool skip = all_cancelled(job); /* scrolled away before its turn */
    if (skip && g_hash_table_lookup(q->jobs, job->key) == job)
        g_hash_table_remove(q->jobs, job->key); /* a request from now on starts its own job, not this skipped one */
    g_mutex_unlock(&q->lock);

    if (!skip && job->folder) {
        g_autoptr(GFile) cover = sfe_folder_cover_find(job->file, NULL);
        g_autoptr(GFileInfo) info = cover ? g_file_query_info(cover,
                                                G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE "," G_FILE_ATTRIBUTE_TIME_MODIFIED,
                                                G_FILE_QUERY_INFO_NONE, NULL, NULL)
                                          : NULL;
        if (info != NULL) {
            g_autofree char* uri = g_file_get_uri(cover);
            gint64 mtime = (gint64)g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
            job->result = sfe_thumb_load_cached(uri, mtime, job->size);
            if (job->result == NULL && !sfe_thumb_has_failed(uri, mtime))
                job->result = sfe_thumb_generate(q->thumbnailers, cover, g_file_info_get_content_type(info), mtime,
                    job->size, NULL, NULL);
        }
    } else if (!skip) {
        job->result = sfe_thumb_load_cached(job->uri, job->mtime, job->size);
        if (job->result == NULL && !sfe_thumb_has_failed(job->uri, job->mtime))
            job->result = sfe_thumb_generate(q->thumbnailers, job->file, job->content_type, job->mtime, job->size,
                NULL, NULL);
    }
    sfe_on_main_thread(q->context, deliver, job, NULL);
}

static gint newest_first(gconstpointer a, gconstpointer b, gpointer data)
{
    (void)data;
    const Job* x = a;
    const Job* y = b;
    return x->seq < y->seq ? 1 : x->seq > y->seq ? -1 : 0;
}

SfeThumbQueue* sfe_thumb_queue_new(int threads, GPtrArray* thumbnailers)
{
    SfeThumbQueue* q = g_new0(SfeThumbQueue, 1);
    g_mutex_init(&q->lock);
    q->context = g_main_context_ref_thread_default();
    q->thumbnailers = thumbnailers ? g_ptr_array_ref(thumbnailers) : sfe_thumbnailers_load(NULL);
    q->jobs = g_hash_table_new(g_str_hash, g_str_equal);
    q->pool = g_thread_pool_new(work, q, MAX(1, threads), FALSE, NULL);
    g_thread_pool_set_sort_function(q->pool, newest_first, NULL);
    return q;
}

SfeThumbQueue* sfe_thumb_queue_get_default(void)
{
    static SfeThumbQueue* queue;
    if (queue == NULL)
        queue = sfe_thumb_queue_new(CLAMP((int)g_get_num_processors() / 2, 1, 4), NULL);
    return queue;
}

bool sfe_thumb_queue_supports(SfeThumbQueue* queue, const char* content_type)
{
    return sfe_thumb_supported(queue->thumbnailers, content_type);
}

static void request(SfeThumbQueue* q, GFile* file, bool folder, const char* content_type, gint64 mtime,
    SfeThumbSize size, GCancellable* cancel, SfeThumbDone done, gpointer data)
{
    g_autofree char* uri = g_file_get_uri(file);
    g_autofree char* key = g_strdup_printf("%d %s%s", size, folder ? "folder " : "", uri);
    Waiter* w = g_new0(Waiter, 1);
    w->done = done;
    w->data = data;
    w->cancel = cancel ? g_object_ref(cancel) : NULL;

    g_mutex_lock(&q->lock);
    Job* job = g_hash_table_lookup(q->jobs, key);
    if (job != NULL && job->mtime == mtime) { /* already asked for: wait for it too */
        job->waiters = g_slist_prepend(job->waiters, w);
        g_mutex_unlock(&q->lock);
        return;
    }
    job = g_new0(Job, 1);
    job->queue = q;
    job->key = g_steal_pointer(&key);
    job->file = g_object_ref(file);
    job->uri = g_steal_pointer(&uri);
    job->content_type = g_strdup(content_type);
    job->mtime = mtime;
    job->size = size;
    job->folder = folder;
    job->seq = ++q->seq;
    job->waiters = g_slist_prepend(NULL, w);
    if (!g_hash_table_contains(q->jobs, job->key))
        g_hash_table_insert(q->jobs, job->key, job);
    g_mutex_unlock(&q->lock);
    g_thread_pool_push(q->pool, job, NULL);
}

void sfe_thumb_queue_request(SfeThumbQueue* q, GFile* file, const char* content_type, gint64 mtime,
    SfeThumbSize size, GCancellable* cancel, SfeThumbDone done, gpointer data)
{
    request(q, file, false, content_type, mtime, size, cancel, done, data);
}

void sfe_thumb_queue_request_folder(SfeThumbQueue* q, GFile* dir, gint64 mtime, SfeThumbSize size,
    GCancellable* cancel, SfeThumbDone done, gpointer data)
{
    request(q, dir, true, NULL, mtime, size, cancel, done, data);
}
