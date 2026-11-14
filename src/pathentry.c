#include "pathentry.h"

#include "util.h"
#include <string.h>

#define MAX_COMPLETIONS 2000

typedef struct {
    GtkListStore* candidates; /* full paths of the subdirectories of `dir` */
    char* dir; /* directory part of the text (as typed, with trailing '/') */
    gboolean programmatic; /* text is being set by sfe, not typed */
} PathEntry;

static PathEntry* get_data(GtkWidget* entry)
{
    return g_object_get_data(G_OBJECT(entry), "sfe-path-entry");
}

static void path_entry_free(gpointer p)
{
    PathEntry* pe = p;
    g_object_unref(pe->candidates);
    g_free(pe->dir);
    g_free(pe);
}

/* Lists the subdirectories of `typed_dir` ("~/foo/") into the store. */
static void fill_candidates(PathEntry* pe, const char* typed_dir)
{
    g_autofree char* expanded = sfe_expand_home(typed_dir);
    g_autoptr(GFile) dir = g_file_new_for_path(expanded);
    g_autoptr(GFileEnumerator) en = g_file_enumerate_children(dir,
        G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE,
        G_FILE_QUERY_INFO_NONE, NULL, NULL);
    int count = 0;

    gtk_list_store_clear(pe->candidates);
    if (en == NULL)
        return;

    GFileInfo* info;
    while (count < MAX_COMPLETIONS && (info = g_file_enumerator_next_file(en, NULL, NULL)) != NULL) {
        if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
            g_autofree char* full = g_strconcat(typed_dir, g_file_info_get_name(info), "/", NULL);
            gtk_list_store_insert_with_values(pe->candidates, NULL, -1, 0, full, -1);
            count++;
        }
        g_object_unref(info);
    }
}

/* Prefix match; hidden directories only once the user typed a leading dot. */
static gboolean match_candidate(GtkEntryCompletion* completion, const char* key,
    GtkTreeIter* iter, gpointer data)
{
    (void)key, (void)data;
    GtkWidget* entry = gtk_entry_completion_get_entry(completion);
    const char* text = gtk_entry_get_text(GTK_ENTRY(entry));
    const char* typed_base = strrchr(text, '/');
    g_autofree char* candidate = NULL;

    gtk_tree_model_get(gtk_entry_completion_get_model(completion), iter, 0, &candidate, -1);
    if (candidate == NULL || !g_str_has_prefix(candidate, text))
        return FALSE;

    const char* cand_base = candidate + (typed_base ? typed_base - text + 1 : 0);
    return cand_base[0] != '.' || (typed_base && typed_base[1] == '.');
}

static void on_changed(GtkEditable* editable, gpointer data)
{
    (void)data;
    GtkWidget* entry = GTK_WIDGET(editable);
    PathEntry* pe = get_data(entry);
    const char* text = gtk_entry_get_text(GTK_ENTRY(entry));
    const char* slash = strrchr(text, '/');

    sfe_path_entry_set_error(entry, FALSE);
    if (pe->programmatic || slash == NULL)
        return;

    g_autofree char* dir = g_strndup(text, slash - text + 1);
    if (g_strcmp0(dir, pe->dir) != 0) {
        g_free(pe->dir);
        pe->dir = g_steal_pointer(&dir);
        fill_candidates(pe, pe->dir);
    }
}

/* Tab accepts the inline completion instead of moving the focus. */
static gboolean on_key_press(GtkWidget* entry, GdkEventKey* event, gpointer data)
{
    (void)data;
    int start, end;

    if (event->keyval != GDK_KEY_Tab)
        return FALSE;
    if (gtk_editable_get_selection_bounds(GTK_EDITABLE(entry), &start, &end))
        gtk_editable_set_position(GTK_EDITABLE(entry), -1);
    return TRUE;
}

GtkWidget* sfe_path_entry_new(void)
{
    GtkWidget* entry = gtk_entry_new();
    GtkEntryCompletion* completion = gtk_entry_completion_new();
    PathEntry* pe = g_new0(PathEntry, 1);

    pe->candidates = gtk_list_store_new(1, G_TYPE_STRING);
    g_object_set_data_full(G_OBJECT(entry), "sfe-path-entry", pe, path_entry_free);

    gtk_entry_completion_set_model(completion, GTK_TREE_MODEL(pe->candidates));
    gtk_entry_completion_set_text_column(completion, 0);
    gtk_entry_completion_set_match_func(completion, match_candidate, NULL, NULL);
    gtk_entry_completion_set_inline_completion(completion, TRUE);
    gtk_entry_completion_set_popup_single_match(completion, FALSE);
    gtk_entry_set_completion(GTK_ENTRY(entry), completion);
    g_object_unref(completion);

    gtk_entry_set_input_hints(GTK_ENTRY(entry), GTK_INPUT_HINT_NO_SPELLCHECK);
    g_signal_connect(entry, "changed", G_CALLBACK(on_changed), NULL);
    g_signal_connect(entry, "key-press-event", G_CALLBACK(on_key_press), NULL);
    return entry;
}

void sfe_path_entry_set_dir(GtkWidget* entry, GFile* dir)
{
    PathEntry* pe = get_data(entry);
    g_autofree char* path = g_file_get_path(dir);
    g_autofree char* uri = path ? NULL : g_file_get_uri(dir);

    pe->programmatic = TRUE;
    gtk_entry_set_text(GTK_ENTRY(entry), path ? path : uri);
    pe->programmatic = FALSE;
    g_clear_pointer(&pe->dir, g_free); /* refresh candidates on next edit */
}

char* sfe_path_entry_get_path(GtkWidget* entry)
{
    g_autofree char* text = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(entry))));
    return sfe_expand_home(text);
}

void sfe_path_entry_set_error(GtkWidget* entry, gboolean error)
{
    GtkStyleContext* style = gtk_widget_get_style_context(entry);
    if (error)
        gtk_style_context_add_class(style, GTK_STYLE_CLASS_ERROR);
    else
        gtk_style_context_remove_class(style, GTK_STYLE_CLASS_ERROR);
}
