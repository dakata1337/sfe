#pragma once

/* sfe as the system's default file manager: the handler of
 * "inode/directory" in mimeapps.list, which xdg-open and other
 * applications follow. Needs the installed sfe.desktop. */

#include <gtk/gtk.h>
#include <stdbool.h>

/* Whether sfe.desktop is installed (an uninstalled build cannot be set). */
bool sfe_default_fm_available(void);

/* Whether sfe already opens folders by default. */
bool sfe_default_fm_is_set(void);

bool sfe_default_fm_set(GError** error);

/* On first start, asks once (remembered in $XDG_STATE_HOME/sfe) whether
 * sfe should become the default file manager. */
void sfe_default_fm_maybe_ask(GtkWindow* parent);
