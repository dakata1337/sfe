#pragma once

/* File icons from the GTK icon theme, cached by (icon, size, scale).
 * The cache is dropped when the icon theme emits "changed"; users that need
 * to re-fetch icons should connect to that signal with g_signal_connect_after()
 * so they run after the cache was cleared. */

#include <gtk/gtk.h>

/* Returns a new reference to a cairo surface `size` logical pixels wide,
 * rendered for `scale` (the widget's scale factor), so icons stay sharp on
 * HiDPI outputs. Never returns NULL. */
cairo_surface_t* sfe_icon_for_info(GFileInfo* info, int size, int scale);
