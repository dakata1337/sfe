#include "search.h"
#include "theme.h"

struct _SfeSearchBar {
    GtkBox parent;
    GtkWidget* entry;
    GtkWidget* count;
    gboolean resetting;
};

G_DEFINE_TYPE(SfeSearchBar, sfe_search_bar, GTK_TYPE_BOX)

enum {
    SIGNAL_QUERY_CHANGED,
    SIGNAL_ACTIVATE,
    SIGNAL_MOVE,
    SIGNAL_CLOSE,
    N_SIGNALS,
};
static guint signals[N_SIGNALS];

static void on_changed(GtkEditable* entry, gpointer data)
{
    SfeSearchBar* self = data;
    if (!self->resetting)
        g_signal_emit(self, signals[SIGNAL_QUERY_CHANGED], 0, gtk_entry_get_text(GTK_ENTRY(entry)));
}

static gboolean on_key_press(GtkWidget* entry, GdkEventKey* event, gpointer data)
{
    (void)entry;
    SfeSearchBar* self = data;

    switch (event->keyval) {
    case GDK_KEY_Escape:
        g_signal_emit(self, signals[SIGNAL_CLOSE], 0);
        return TRUE;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
        g_signal_emit(self, signals[SIGNAL_ACTIVATE], 0);
        return TRUE;
    case GDK_KEY_Up:
    case GDK_KEY_KP_Up:
        g_signal_emit(self, signals[SIGNAL_MOVE], 0, -1);
        return TRUE;
    case GDK_KEY_Down:
    case GDK_KEY_KP_Down:
        g_signal_emit(self, signals[SIGNAL_MOVE], 0, 1);
        return TRUE;
    default:
        return FALSE;
    }
}

void sfe_search_bar_focus(SfeSearchBar* self)
{
    gtk_widget_show(GTK_WIDGET(self));
    gtk_widget_grab_focus(self->entry); /* selects the text */
}

const char* sfe_search_bar_get_text(SfeSearchBar* self)
{
    return gtk_entry_get_text(GTK_ENTRY(self->entry));
}

void sfe_search_bar_set_text(SfeSearchBar* self, const char* text)
{
    gtk_entry_set_text(GTK_ENTRY(self->entry), text);
}

void sfe_search_bar_reset(SfeSearchBar* self)
{
    self->resetting = TRUE;
    gtk_entry_set_text(GTK_ENTRY(self->entry), "");
    self->resetting = FALSE;
    gtk_label_set_text(GTK_LABEL(self->count), "");
}

void sfe_search_bar_set_count(SfeSearchBar* self, int shown, int matched, int total)
{
    g_autofree char* text = matched > shown ? g_strdup_printf("best %d of %d", shown, matched)
                                            : g_strdup_printf("%d / %d", shown, total);
    gtk_label_set_text(GTK_LABEL(self->count), text);
}

static void sfe_search_bar_class_init(SfeSearchBarClass* klass)
{
    GType type = G_TYPE_FROM_CLASS(klass);
    signals[SIGNAL_QUERY_CHANGED] = g_signal_new("query-changed", type, G_SIGNAL_RUN_LAST, 0,
        NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
    signals[SIGNAL_ACTIVATE] = g_signal_new("activate", type, G_SIGNAL_RUN_LAST, 0,
        NULL, NULL, NULL, G_TYPE_NONE, 0);
    signals[SIGNAL_MOVE] = g_signal_new("move", type, G_SIGNAL_RUN_LAST, 0,
        NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
    signals[SIGNAL_CLOSE] = g_signal_new("close", type, G_SIGNAL_RUN_LAST, 0,
        NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void sfe_search_bar_init(SfeSearchBar* self)
{
    GtkWidget* icon = sfe_scaled_icon_new("system-search-symbolic", 16);

    gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_HORIZONTAL);
    gtk_box_set_spacing(GTK_BOX(self), 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(self)), "sfe-search");

    self->entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(self->entry), "Search in this folder");
    gtk_entry_set_input_hints(GTK_ENTRY(self->entry), GTK_INPUT_HINT_NO_SPELLCHECK);
    gtk_entry_set_width_chars(GTK_ENTRY(self->entry), 4);
    self->count = gtk_label_new(NULL);
    gtk_style_context_add_class(gtk_widget_get_style_context(self->count), "sfe-search-count");
    gtk_style_context_add_class(gtk_widget_get_style_context(icon), "sfe-search-icon");

    gtk_box_pack_start(GTK_BOX(self), icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(self), self->entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(self), self->count, FALSE, FALSE, 0);
    gtk_widget_show_all(GTK_WIDGET(self));
    gtk_widget_set_no_show_all(GTK_WIDGET(self), TRUE);
    gtk_widget_hide(GTK_WIDGET(self));

    g_signal_connect(self->entry, "changed", G_CALLBACK(on_changed), self);
    g_signal_connect(self->entry, "key-press-event", G_CALLBACK(on_key_press), self);
}

GtkWidget* sfe_search_bar_new(void)
{
    return g_object_new(SFE_TYPE_SEARCH_BAR, NULL);
}
