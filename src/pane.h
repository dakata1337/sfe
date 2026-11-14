#pragma once

/* One directory view: top bar (back, forward, parent, path entry, view
 * toggle) above a grid or list of files. */

#include "config.h"
#include <gtk/gtk.h>

#define SFE_TYPE_PANE (sfe_pane_get_type())
G_DECLARE_FINAL_TYPE(SfePane, sfe_pane, SFE, PANE, GtkBox)

SfePane* sfe_pane_new(GFile* dir);

GFile* sfe_pane_get_dir(SfePane* self);

/* Goes to `dir`, recording the current directory in the history. */
void sfe_pane_navigate(SfePane* self, GFile* dir);
void sfe_pane_go_back(SfePane* self);
void sfe_pane_go_forward(SfePane* self);
void sfe_pane_go_up(SfePane* self);
void sfe_pane_reload(SfePane* self);

void sfe_pane_focus_location(SfePane* self);
void sfe_pane_focus_view(SfePane* self);
void sfe_pane_set_view(SfePane* self, SfeViewMode mode);

/* Selected files as a list of GFile* (free with g_list_free_full(l, g_object_unref)). */
GList* sfe_pane_get_selected_files(SfePane* self);
