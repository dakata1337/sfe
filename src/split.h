#pragma once

/* Up to three directory panes:  left | right-top / right-bottom.
 *
 *   1 pane:   [ left ]
 *   2 panes:  [ left | right ]                 vertical split of the only pane
 *   3 panes:  [ left | right-top / right-bot ] horizontal split of the right pane
 *
 * Closing the left pane slides the right column over: right-top becomes
 * the left pane and right-bottom becomes the right one. One pane is
 * "active": the last one that had keyboard focus.
 *
 * In a narrow window the views stack top / bottom instead of side by side;
 * the single view then splits with "split top / bottom". */

#include "pane.h"
#include <gtk/gtk.h>

#define SFE_TYPE_SPLIT (sfe_split_get_type())
G_DECLARE_FINAL_TYPE(SfeSplit, sfe_split, SFE, SPLIT, GtkBox)

/* Signals:
 *   "active-changed"             another pane became active
 *   "directory-changed" (pane)   any pane changed directory */
SfeSplit* sfe_split_new(GFile* dir);

SfePane* sfe_split_get_active(SfeSplit* self);
int sfe_split_get_n_panes(SfeSplit* self);

/* 0 = left, 1 = right (top), 2 = right-bottom; NULL if absent. */
SfePane* sfe_split_get_pane(SfeSplit* self, int index);

/* Splits `pane` if the layout allows it (see above). */
void sfe_split_split(SfeSplit* self, SfePane* pane, SfeSplitKind kind);

/* Closes `pane` unless it is the last one. */
void sfe_split_close(SfeSplit* self, SfePane* pane);

/* Makes the pane containing `widget` active (call when the focus moves). */
void sfe_split_track_focus(SfeSplit* self, GtkWidget* widget);

/* Moves the focus to the next pane (left → right-top → right-bottom). */
void sfe_split_focus_next(SfeSplit* self);
