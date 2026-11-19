#pragma once

/* File icons from the GTK icon theme, cached by (file type, size, scale).
 * The cache is dropped when the icon theme emits "changed"; users that need
 * to re-fetch icons should connect to that signal with g_signal_connect_after()
 * so they run after the cache was cleared. */

#include <gtk/gtk.h>

/* The icon of the file `info` describes (its content type's, or the home
 * and XDG folders' own, found through `parent`, the folder it is in; may be
 * NULL). Returns a new reference to a cairo surface `size` logical pixels
 * wide, rendered for `scale` (the widget's scale factor), so icons stay
 * sharp on HiDPI outputs. Never returns NULL. */
cairo_surface_t* sfe_icon_for_info(GFileInfo* info, GFile* parent, int size, int scale);

/* The icon of `content_type` ("inode/directory", "text/plain") drawn in a
 * file_icons style (SfeFileIcons), for previews; not cached. */
GdkPixbuf* sfe_icon_preview(const char* content_type, int style, int size, int scale);

/* Drops the cached icons and makes the views fetch them again: the
 * theme's colours or [general] file_icons changed. */
void sfe_icons_reset(void);
