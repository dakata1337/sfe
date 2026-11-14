#pragma once

/* "Compress…": asks for the archive's name and format (only the formats an
 * installed tool can create are offered). */

#include <gio/gio.h>
#include <gtk/gtk.h>
#include <stdbool.h>

/* Called with the archive to create (inside `dir`) when the user accepts. */
typedef void (*SfeCompressFunc)(GFile* archive, gpointer data);

/* Whether any archive format can be created with the installed tools. */
bool sfe_compress_available(void);

/* `files` (GFile*) are what goes into the archive; they name it by default. */
void sfe_compress_ask(GtkWidget* parent, GList* files, GFile* dir, SfeCompressFunc done, gpointer data,
    GDestroyNotify destroy);
