#pragma once

/* The home trash (~/.local/share/Trash, the freedesktop.org trash spec),
 * read directly so it works without GVfs. g_file_trash() puts files there:
 *
 *   files/NAME              the trashed item
 *   info/NAME.trashinfo     where it came from (Path=) and when
 *
 * GTK-free and unit-tested. */

#include <gio/gio.h>
#include <stdbool.h>

/* ~/.local/share/Trash/files (not created). */
GFile* sfe_trash_files_dir(void);

/* Whether `file` is an item in the trash (directly inside files/). */
bool sfe_trash_contains(GFile* file);

/* Whether `dir` is the trash folder itself. */
bool sfe_trash_is_trash_dir(GFile* dir);

bool sfe_trash_is_empty(void);

/* The info file of a trashed item: info/NAME.trashinfo. */
GFile* sfe_trash_info_file(GFile* item);

/* Where a trashed item came from. */
GFile* sfe_trash_original_location(GFile* item, GError** error);

/* Moves a trashed item back where it came from (recreating missing parent
 * folders) and drops its info file. Fails with G_IO_ERROR_EXISTS rather
 * than replacing something. */
bool sfe_trash_restore(GFile* item, GError** error);

/* The newest trashed item that came from `original`, or NULL. */
GFile* sfe_trash_find(GFile* original);

/* Items (GFile*) to delete to empty the trash or to delete `items` from it:
 * the items plus their info files. */
GList* sfe_trash_with_info(GList* items);

/* Everything in files/ and info/, to empty the trash. */
GList* sfe_trash_everything(void);
