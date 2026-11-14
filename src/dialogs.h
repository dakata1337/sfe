#pragma once

/* Small reusable dialogs. */

#include <gtk/gtk.h>

/* Non-blocking error message attached to the window that contains `widget`. */
void sfe_show_error(GtkWidget* widget, const char* title, const char* detail);
