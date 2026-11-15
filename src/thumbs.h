#pragma once

/* Thumbnails, following the freedesktop.org thumbnail spec, so the cache in
 * ~/.cache/thumbnails is shared with other file managers:
 *
 *   ~/.cache/thumbnails/{normal,large,x-large,xx-large}/MD5(uri).png
 *      PNGs with Thumb::URI and Thumb::MTime; stale when the file changed
 *   ~/.cache/thumbnails/fail/sfe/MD5(uri).png
 *      files that could not be thumbnailed (not tried again until changed)
 *
 * Making one: images that gdk-pixbuf reads are scaled in-process; other
 * types use an installed thumbnailer (the .thumbnailer files in /usr/share/thumbnailers,
 * e.g. ffmpegthumbnailer, gsf-office); videos without one fall back to
 * ffmpeg.
 *
 * The work happens in a small thread pool; results arrive on the main
 * thread. The most recently requested files go first (the rows just
 * scrolled into view), and requests whose GCancellable was cancelled are
 * dropped before they start. GTK-free and unit-tested. */

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>
#include <stdbool.h>

typedef enum {
    SFE_THUMB_NORMAL, /* 128 px */
    SFE_THUMB_LARGE, /* 256 px */
    SFE_THUMB_XLARGE, /* 512 px */
    SFE_THUMB_XXLARGE, /* 1024 px */
} SfeThumbSize;

/* The edge length of a size, and the smallest size that holds `pixels`. */
int sfe_thumb_size_pixels(SfeThumbSize size);
SfeThumbSize sfe_thumb_size_for(int pixels);

/* ~/.cache/thumbnails/<size>/<md5 of uri>.png ($XDG_CACHE_HOME honoured). */
char* sfe_thumb_path(const char* uri, SfeThumbSize size);
char* sfe_thumb_fail_path(const char* uri);

/* Whether `file` lies in the thumbnail cache (never thumbnailed). */
bool sfe_thumb_in_cache_dir(GFile* file);

/* The cached thumbnail if it is there and up to date (Thumb::URI matches,
 * Thumb::MTime equals `mtime`, seconds since the epoch), else NULL. */
GdkPixbuf* sfe_thumb_load_cached(const char* uri, gint64 mtime, SfeThumbSize size);

/* Whether a failure is recorded for this version of the file. */
bool sfe_thumb_has_failed(const char* uri, gint64 mtime);

/* An installed thumbnailer (a .thumbnailer file). */
typedef struct {
    char* exec; /* Exec=, with %i %u %o %s */
    char** mime_types;
} SfeThumbnailer;

/* The thumbnailers in <dir>/thumbnailers of each of `data_dirs` (NULL: the
 * XDG data dirs, user's first); entries whose TryExec program is missing
 * are skipped. SfeThumbnailer*, the first one for a type wins. */
GPtrArray* sfe_thumbnailers_load(const char* const* data_dirs);
const SfeThumbnailer* sfe_thumbnailers_find(GPtrArray* thumbnailers, const char* content_type);

/* The command line of a thumbnailer for one file: %i path, %u URI,
 * %o output file, %s size in pixels, %% a percent sign. */
char** sfe_thumbnailer_argv(const SfeThumbnailer* t, const char* path, const char* uri, const char* output, int size,
    GError** error);

/* Whether sfe can make a thumbnail for this content type at all. */
bool sfe_thumb_supported(GPtrArray* thumbnailers, const char* content_type);

/* Makes the thumbnail for `file` (local files only), writes it to the cache
 * and returns it; on failure records it (sfe_thumb_has_failed) and returns
 * NULL with `error` set. Blocks: call it from a worker thread. */
GdkPixbuf* sfe_thumb_generate(GPtrArray* thumbnailers, GFile* file, const char* content_type, gint64 mtime,
    SfeThumbSize size, GCancellable* cancel, GError** error);

/* ------------------------------------------------------------------------ */
/* The worker pool                                                           */

/* `pixbuf` is NULL when there is no thumbnail (it failed, or the file is
 * not supported). Runs on the thread that created the queue. */
typedef void (*SfeThumbDone)(GFile* file, GdkPixbuf* pixbuf, gpointer data);

typedef struct SfeThumbQueue SfeThumbQueue;

/* The application's queue (created on first use, on the main thread). */
SfeThumbQueue* sfe_thumb_queue_get_default(void);

/* A queue with its own thumbnailer list (tests). */
SfeThumbQueue* sfe_thumb_queue_new(int threads, GPtrArray* thumbnailers);

/* Whether a request for this content type can give a thumbnail. */
bool sfe_thumb_queue_supports(SfeThumbQueue* queue, const char* content_type);

/* Asks for the thumbnail of `file`. `done` runs later unless `cancel` was
 * cancelled by then (cancel it before freeing `data`). Requests for the same
 * file and size share one job. */
void sfe_thumb_queue_request(SfeThumbQueue* queue, GFile* file, const char* content_type, gint64 mtime,
    SfeThumbSize size, GCancellable* cancel, SfeThumbDone done, gpointer data);
