#include "clipboard.h"

#include <string.h>

#define GNOME_TARGET "x-special/gnome-copied-files"
#define URI_TARGET "text/uri-list"

enum {
    TARGET_GNOME,
    TARGET_URIS,
    TARGET_TEXT,
};

static const GtkTargetEntry targets[] = {
    { GNOME_TARGET, 0, TARGET_GNOME },
    { URI_TARGET, 0, TARGET_URIS },
    { "UTF8_STRING", 0, TARGET_TEXT },
    { "text/plain;charset=utf-8", 0, TARGET_TEXT },
};

typedef struct {
    GList* files; /* GFile* */
    bool cut;
} Contents;

static Contents* owned; /* what sfe currently offers, or NULL */
static bool has_files;

static GtkClipboard* clipboard(void)
{
    return gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
}

/* ------------------------------------------------------------------------ */
/* Offering                                                                  */
/* ------------------------------------------------------------------------ */

static void contents_free(Contents* c)
{
    g_list_free_full(c->files, g_object_unref);
    g_free(c);
}

static char* join_files(GList* files, bool uris, const char* separator)
{
    GString* out = g_string_new(NULL);
    for (GList* l = files; l != NULL; l = l->next) {
        g_autofree char* s = uris ? g_file_get_uri(l->data) : g_file_get_parse_name(l->data);
        if (out->len > 0)
            g_string_append(out, separator);
        g_string_append(out, s);
    }
    return g_string_free(out, FALSE);
}

static void get_contents(GtkClipboard* clip, GtkSelectionData* sel, guint info, gpointer data)
{
    (void)clip;
    Contents* c = data;
    g_autofree char* text = NULL;

    switch (info) {
    case TARGET_GNOME: {
        g_autofree char* uris = join_files(c->files, true, "\n");
        text = g_strconcat(c->cut ? "cut\n" : "copy\n", uris, NULL);
        break;
    }
    case TARGET_URIS: {
        g_autofree char* uris = join_files(c->files, true, "\r\n");
        text = g_strconcat(uris, "\r\n", NULL);
        break;
    }
    default: {
        g_autofree char* paths = join_files(c->files, false, "\n");
        gtk_selection_data_set_text(sel, paths, -1);
        return;
    }
    }
    gtk_selection_data_set(sel, gtk_selection_data_get_target(sel), 8, (const guchar*)text, (int)strlen(text));
}

static void clear_contents(GtkClipboard* clip, gpointer data)
{
    (void)clip;
    if (owned == data)
        owned = NULL;
    contents_free(data);
}

/* ------------------------------------------------------------------------ */
/* Tracking whether files are available                                      */
/* ------------------------------------------------------------------------ */

static void on_targets(GtkClipboard* clip, GdkAtom* atoms, int n, gpointer data)
{
    (void)clip, (void)data;
    has_files = false;
    for (int i = 0; i < n; i++) {
        g_autofree char* name = gdk_atom_name(atoms[i]);
        if (strcmp(name, GNOME_TARGET) == 0 || strcmp(name, URI_TARGET) == 0)
            has_files = true;
    }
}

static void on_owner_change(GtkClipboard* clip, GdkEvent* event, gpointer data)
{
    (void)event, (void)data;
    gtk_clipboard_request_targets(clip, on_targets, NULL);
}

static void ensure_watching(void)
{
    static bool watching;
    if (watching)
        return;
    watching = true;
    g_signal_connect(clipboard(), "owner-change", G_CALLBACK(on_owner_change), NULL);
    gtk_clipboard_request_targets(clipboard(), on_targets, NULL);
}

bool sfe_clipboard_has_files(void)
{
    ensure_watching();
    return has_files || owned != NULL;
}

/* ------------------------------------------------------------------------ */
/* Public API                                                                */
/* ------------------------------------------------------------------------ */

static gpointer ref_copy(gconstpointer object, gpointer data)
{
    (void)data;
    return g_object_ref((gpointer)object);
}

void sfe_clipboard_set(GList* files, bool cut)
{
    Contents* c = g_new0(Contents, 1);

    ensure_watching();
    c->files = g_list_copy_deep(files, ref_copy, NULL);
    c->cut = cut;
    if (gtk_clipboard_set_with_data(clipboard(), targets, G_N_ELEMENTS(targets), get_contents, clear_contents, c))
        owned = c;
    else
        contents_free(c);
}

void sfe_clipboard_clear_if_cut(void)
{
    if (owned != NULL && owned->cut)
        gtk_clipboard_clear(clipboard());
}

typedef struct {
    SfeClipboardFunc func;
    gpointer data;
} Request;

static void deliver(Request* req, GList* files, bool cut)
{
    req->func(files, cut, req->data);
    g_list_free_full(files, g_object_unref);
    g_free(req);
}

static void on_uri_list(GtkClipboard* clip, GtkSelectionData* sel, gpointer data)
{
    (void)clip;
    g_auto(GStrv) uris = gtk_selection_data_get_uris(sel);
    GList* files = NULL;

    for (char** u = uris; u != NULL && *u != NULL; u++)
        files = g_list_prepend(files, g_file_new_for_uri(*u));
    deliver(data, g_list_reverse(files), false);
}

/* "copy\nfile:///a\nfile:///b" or "cut\n..." */
static void on_gnome_files(GtkClipboard* clip, GtkSelectionData* sel, gpointer data)
{
    int len = gtk_selection_data_get_length(sel);
    if (len <= 0) { /* not offered: fall back to a plain URI list */
        gtk_clipboard_request_contents(clip, gdk_atom_intern_static_string(URI_TARGET), on_uri_list, data);
        return;
    }

    g_autofree char* text = g_strndup((const char*)gtk_selection_data_get_data(sel), len);
    g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
    bool cut = lines[0] != NULL && g_str_has_prefix(lines[0], "cut");
    GList* files = NULL;

    for (int i = 1; lines[0] != NULL && lines[i] != NULL; i++) {
        g_strstrip(lines[i]);
        if (lines[i][0] != '\0')
            files = g_list_prepend(files, g_file_new_for_uri(lines[i]));
    }
    deliver(data, g_list_reverse(files), cut);
}

void sfe_clipboard_get(SfeClipboardFunc func, gpointer data)
{
    Request* req = g_new0(Request, 1);
    req->func = func;
    req->data = data;
    gtk_clipboard_request_contents(clipboard(), gdk_atom_intern_static_string(GNOME_TARGET), on_gnome_files, req);
}
