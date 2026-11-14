#pragma once

/* Cut / copy / paste of files through the system clipboard, in the formats
 * other file managers use (x-special/gnome-copied-files, text/uri-list) plus
 * plain text paths. Because it is the system clipboard, it works between
 * splits, between sfe windows and with other applications. */

#include <gtk/gtk.h>
#include <stdbool.h>

/* Puts `files` (GFile*) on the clipboard. */
void sfe_clipboard_set(GList* files, bool cut);

/* Reads files from the clipboard; `files` is NULL when it holds none. The
 * list is freed after the callback returns. */
typedef void (*SfeClipboardFunc)(GList* files, bool cut, gpointer data);
void sfe_clipboard_get(SfeClipboardFunc func, gpointer data);

/* Whether the clipboard currently offers files (kept up to date). */
bool sfe_clipboard_has_files(void);

/* After a cut-and-paste the files are gone from their old place: drop them
 * from the clipboard if sfe put them there. */
void sfe_clipboard_clear_if_cut(void);
