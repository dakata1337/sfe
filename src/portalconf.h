#pragma once

/* "Use sfe for Open / Save dialogs": which xdg-desktop-portal backend
 * answers file dialogs is set in portals.conf (xdg-desktop-portal 1.18+):
 *
 *   ~/.config/xdg-desktop-portal/<desktop>-portals.conf
 *   [preferred]
 *   default=hyprland;gtk
 *   org.freedesktop.impl.portal.FileChooser=sfe
 *
 * This reads and edits that line, leaving the rest of the file (and its
 * comments) alone. A new file starts from the settings in effect before
 * (the user's portals.conf, else the system's file for the desktop), so the
 * other portals (screenshots, screen sharing) keep their backends.
 * GTK-free and unit-tested. */

#include <glib.h>
#include <stdbool.h>

/* The desktop's name as portals.conf files use it: the first entry of
 * $XDG_CURRENT_DESKTOP in lower case ("hyprland"), or NULL. */
char* sfe_portal_desktop(void);

/* The user's file for `desktop` (portals.conf when NULL). */
char* sfe_portal_conf_path(const char* desktop);

/* Whether file dialogs are set to sfe in the file at `path`. */
bool sfe_portal_conf_uses_sfe(const char* path);

/* Sets file dialogs to sfe in the file at `path`, or takes that setting
 * out again (`use_sfe` false). A file that does not exist yet starts as a
 * copy of [preferred] from `seed` (a portals.conf path, may be NULL). */
bool sfe_portal_conf_set(const char* path, const char* seed, bool use_sfe, GError** error);

/* The file whose settings are in effect for `desktop` before sfe writes
 * its own: the user's portals.conf, else the system's
 * <desktop>-portals.conf, else the system's portals.conf; NULL if none. */
char* sfe_portal_conf_seed(const char* desktop);

/* Whether sfe-portal is installed (its sfe.portal file in a data dir). */
bool sfe_portal_installed(void);

/* Stops the user's xdg-desktop-portal so it reads the change; D-Bus starts
 * it again on the next request. Returns how many were stopped. */
int sfe_portal_restart(void);
