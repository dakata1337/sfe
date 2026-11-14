#pragma once

/* Small reusable dialogs. All are non-blocking: results arrive through the
 * callbacks. They use the flat sfe style (.sfe-dialog). */

#include <gtk/gtk.h>
#include <stdbool.h>

/* A dialog transient for the window that contains `widget`, styled for sfe. */
GtkWidget* sfe_dialog_new(GtkWidget* widget, const char* title);

/* Error message attached to the window that contains `widget`. */
void sfe_show_error(GtkWidget* widget, const char* title, const char* detail);

/* Asks for a file name. `done` runs only when the user accepts; names that
 * are empty or contain '/' cannot be accepted. `destroy` frees `data`. */
typedef void (*SfeNameFunc)(const char* name, gpointer data);
void sfe_ask_name(GtkWidget* widget, const char* title, const char* initial, const char* accept,
    SfeNameFunc done, gpointer data, GDestroyNotify destroy);

/* Asks for confirmation; `done` runs only on "yes". */
typedef void (*SfeConfirmFunc)(gpointer data);
void sfe_confirm(GtkWidget* widget, const char* title, const char* detail, const char* accept,
    bool destructive, SfeConfirmFunc done, gpointer data, GDestroyNotify destroy);
