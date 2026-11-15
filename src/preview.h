#pragma once

/* The preview beside the files in a file dialog (sfe --choose): the
 * selected file's thumbnail (images, and videos or documents with a
 * thumbnailer; thumbs.c) or its icon, with its name, type, size, date and,
 * for images, the dimensions. Several selected files show their count;
 * none a hint. */

#include <gtk/gtk.h>

#define SFE_TYPE_PREVIEW (sfe_preview_get_type())
G_DECLARE_FINAL_TYPE(SfePreview, sfe_preview, SFE, PREVIEW, GtkBox)

GtkWidget* sfe_preview_new(void);

/* Shows `files` (GFile*, the selection; may be NULL). */
void sfe_preview_set_files(SfePreview* self, GList* files);
