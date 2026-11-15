#pragma once

/* The Settings window (Ctrl+P, :preferences): pages for the options (built
 * from the config's option table, so a new option shows up without extra
 * code), the keys, custom commands, programs per file type and system
 * integration. Changes apply at once and are saved to config.toml. */

#include <gtk/gtk.h>

/* Shows the dialog for the window that contains `parent` (one per window). */
void sfe_show_preferences(GtkWidget* parent);

/* The same, on a page: "general", "thumbnails", "sidebar", "keys",
 * "commands", "programs" or "system" (NULL or unknown: as it was). */
void sfe_show_preferences_page(GtkWidget* parent, const char* page);

/* The page names, NULL-terminated (for :settings completion). */
const char* const* sfe_preferences_pages(void);
