#pragma once

/* The right-click menu of a directory view, built from the action table:
 * one set of entries for selected files, another for the folder itself
 * (empty space), plus "Open With" and the user's custom [[actions]]. */

#include "window.h"
#include <gtk/gtk.h>

/* Pops up the menu for `window`'s active pane, at the pointer of `event`
 * or, when `event` is NULL (Menu key), over `attach`. */
void sfe_context_menu_popup(SfeWindow* window, GtkWidget* attach, const GdkEvent* event);
