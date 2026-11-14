#include "dialogs.h"

void sfe_show_error(GtkWidget* widget, const char* title, const char* detail)
{
    GtkWidget* toplevel = widget ? gtk_widget_get_toplevel(widget) : NULL;
    GtkWidget* dialog = gtk_message_dialog_new(GTK_IS_WINDOW(toplevel) ? GTK_WINDOW(toplevel) : NULL,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", title);

    if (detail != NULL)
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", detail);
    g_signal_connect(dialog, "response", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_widget_show(dialog);
}
