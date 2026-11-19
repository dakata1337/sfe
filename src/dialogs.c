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

void sfe_style_combo_box(GtkWidget* combo)
{
    AtkObject* popup = gtk_combo_box_get_popup_accessible(GTK_COMBO_BOX(combo));
    GtkWidget* menu = popup ? gtk_accessible_get_widget(GTK_ACCESSIBLE(popup)) : NULL;
    if (menu != NULL)
        gtk_style_context_add_class(gtk_widget_get_style_context(menu), "sfe-menu");
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
