#pragma once

/* One directory view: top bar (back, forward, parent, path entry, view
 * toggle, split and close buttons) above a grid or list of files.
 *
 * Signals: "directory-changed", "split" (SfeSplitKind), "close". */

#include "config.h"
#include <gtk/gtk.h>
#include <stdbool.h>

typedef enum {
    SFE_SPLIT_VERTICAL, /* side by side: a vertical divider */
    SFE_SPLIT_HORIZONTAL, /* stacked: a horizontal divider */
} SfeSplitKind;

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
SfeViewMode sfe_pane_get_view(SfePane* self);

/* Enables the split / close buttons (the split container decides). */
void sfe_pane_set_split_buttons(SfePane* self, bool vertical, bool horizontal, bool close);

/* Marks the pane that receives shortcuts, paste and sidebar navigation. */
void sfe_pane_set_active(SfePane* self, bool active);

/* Selected files as a list of GFile* (free with g_list_free_full(l, g_object_unref)). */
GList* sfe_pane_get_selected_files(SfePane* self);
