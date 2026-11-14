#pragma once

/* The Preferences dialog (Ctrl+P). Built from the config's option table,
 * so a new option shows up here without extra code. Changes apply at once
 * and are saved to config.toml. */

#include <gtk/gtk.h>

/* Shows the dialog for the window that contains `parent` (one per window). */
void sfe_show_preferences(GtkWidget* parent);
