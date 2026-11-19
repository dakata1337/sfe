#pragma once

/* Small reusable dialogs. All are non-blocking: results arrive through the
 * callbacks. They use the flat sfe style (.sfe-dialog). */

#include <gtk/gtk.h>
#include <stdbool.h>

/* A dialog transient for the window that contains `widget`, styled for sfe. */
GtkWidget* sfe_dialog_new(GtkWidget* widget, const char* title);

/* Gives a combo box's drop-down list the sfe menu style (it is a popup
 * window of its own, outside the dialog). */
void sfe_style_combo_box(GtkWidget* combo);

/* Error message attached to the window that contains `widget`. */
void sfe_show_error(GtkWidget* widget, const char* title, const char* detail);

/* Asks for confirmation; `done` runs only on "yes". */
typedef void (*SfeConfirmFunc)(gpointer data);
void sfe_confirm(GtkWidget* widget, const char* title, const char* detail, const char* accept,
    bool destructive, SfeConfirmFunc done, gpointer data, GDestroyNotify destroy);
