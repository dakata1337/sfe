#include "dialogs.h"

#include <string.h>

static GtkWindow* parent_window(GtkWidget* widget)
{
    GtkWidget* toplevel = widget ? gtk_widget_get_toplevel(widget) : NULL;
    return GTK_IS_WINDOW(toplevel) ? GTK_WINDOW(toplevel) : NULL;
}

static void style_dialog(GtkWidget* dialog)
{
    GtkStyleContext* style = gtk_widget_get_style_context(dialog);
    gtk_style_context_add_class(style, "sfe-window");
    gtk_style_context_add_class(style, "sfe-dialog");
}

GtkWidget* sfe_dialog_new(GtkWidget* widget, const char* title)
{
    GtkWidget* dialog = gtk_dialog_new();

    gtk_window_set_title(GTK_WINDOW(dialog), title);
    gtk_window_set_transient_for(GTK_WINDOW(dialog), parent_window(widget));
    gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
    style_dialog(dialog);
    return dialog;
}

void sfe_show_error(GtkWidget* widget, const char* title, const char* detail)
{
    GtkWidget* dialog = gtk_message_dialog_new(parent_window(widget),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", title);

    if (detail != NULL)
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", detail);
    style_dialog(dialog);
    g_signal_connect(dialog, "response", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_widget_show(dialog);
}

/* ------------------------------------------------------------------------ */
/* Name dialog                                                               */
/* ------------------------------------------------------------------------ */

typedef struct {
    SfeNameFunc done;
    gpointer data;
    GDestroyNotify destroy;
    GtkWidget* entry;
} NameDialog;

static void name_dialog_free(gpointer p)
{
    NameDialog* nd = p;
    if (nd->destroy != NULL)
        nd->destroy(nd->data);
    g_free(nd);
}

static bool valid_name(const char* name)
{
    return *name != '\0' && strchr(name, '/') == NULL && strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
}

static void on_name_changed(GtkEditable* entry, gpointer dialog)
{
    const char* text = gtk_entry_get_text(GTK_ENTRY(entry));
    gtk_dialog_set_response_sensitive(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT, valid_name(text));
}

static void on_name_response(GtkDialog* dialog, int response, gpointer data)
{
    NameDialog* nd = data;
    g_autofree char* name = g_strdup(gtk_entry_get_text(GTK_ENTRY(nd->entry)));

    if (response == GTK_RESPONSE_ACCEPT && valid_name(name))
        nd->done(name, nd->data);
    gtk_widget_destroy(GTK_WIDGET(dialog)); /* frees nd and its data */
}

void sfe_ask_name(GtkWidget* widget, const char* title, const char* initial, const char* accept,
    SfeNameFunc done, gpointer data, GDestroyNotify destroy)
{
    GtkWidget* dialog = sfe_dialog_new(widget, title);
    GtkWidget* area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    NameDialog* nd = g_new0(NameDialog, 1);

    nd->done = done;
    nd->data = data;
    nd->destroy = destroy;
    nd->entry = gtk_entry_new();
    g_object_set_data_full(G_OBJECT(dialog), "sfe-name-dialog", nd, name_dialog_free);

    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 380, -1);
    gtk_dialog_add_button(GTK_DIALOG(dialog), "Cancel", GTK_RESPONSE_CANCEL);
    GtkWidget* ok = gtk_dialog_add_button(GTK_DIALOG(dialog), accept, GTK_RESPONSE_ACCEPT);
    gtk_style_context_add_class(gtk_widget_get_style_context(ok), "suggested-action");
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);

    gtk_entry_set_text(GTK_ENTRY(nd->entry), initial);
    gtk_entry_set_activates_default(GTK_ENTRY(nd->entry), TRUE);
    gtk_container_set_border_width(GTK_CONTAINER(area), 12);
    gtk_box_pack_start(GTK_BOX(area), nd->entry, FALSE, FALSE, 0);

    g_signal_connect(nd->entry, "changed", G_CALLBACK(on_name_changed), dialog);
    g_signal_connect(dialog, "response", G_CALLBACK(on_name_response), nd);
    gtk_widget_show_all(dialog);

    /* Select the name without its extension, ready to be typed over. */
    const char* dot = strrchr(initial, '.');
    int end = dot && dot != initial ? (int)g_utf8_pointer_to_offset(initial, dot) : -1;
    gtk_editable_select_region(GTK_EDITABLE(nd->entry), 0, end);
}

/* ------------------------------------------------------------------------ */
/* Confirmation                                                              */
/* ------------------------------------------------------------------------ */

typedef struct {
    SfeConfirmFunc done;
    gpointer data;
    GDestroyNotify destroy;
} Confirm;

static void on_confirm_response(GtkDialog* dialog, int response, gpointer p)
{
    Confirm* c = p;
    gtk_widget_destroy(GTK_WIDGET(dialog));
    if (response == GTK_RESPONSE_ACCEPT)
        c->done(c->data);
    if (c->destroy != NULL)
        c->destroy(c->data);
    g_free(c);
}

void sfe_confirm(GtkWidget* widget, const char* title, const char* detail, const char* accept,
    bool destructive, SfeConfirmFunc done, gpointer data, GDestroyNotify destroy)
{
    GtkWidget* dialog = gtk_message_dialog_new(parent_window(widget),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "%s", title);
    Confirm* c = g_new0(Confirm, 1);

    c->done = done;
    c->data = data;
    c->destroy = destroy;
    if (detail != NULL)
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", detail);
    style_dialog(dialog);
    gtk_dialog_add_button(GTK_DIALOG(dialog), "Cancel", GTK_RESPONSE_CANCEL);
    GtkWidget* ok = gtk_dialog_add_button(GTK_DIALOG(dialog), accept, GTK_RESPONSE_ACCEPT);
    gtk_style_context_add_class(gtk_widget_get_style_context(ok),
        destructive ? "destructive-action" : "suggested-action");
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), destructive ? GTK_RESPONSE_CANCEL : GTK_RESPONSE_ACCEPT);
    g_signal_connect(dialog, "response", G_CALLBACK(on_confirm_response), c);
    gtk_widget_show(dialog);
}
