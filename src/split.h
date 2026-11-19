#pragma once

/* Up to three directory panes:  left | right-top / right-bottom.
 *
 *   1 pane:   [ left ]
 *   2 panes:  [ left | right ]  or  [ top / bottom ]
 *   3 panes:  [ left | right-top / right-bottom ]
 *
 * One pane splits either way. Two side by side split top / bottom and two
 * top / bottom split side by side: the pane whose button was used becomes
 * right-top, the new one right-bottom and the other one the left pane.
 * Closing the left pane slides the right column over: right-top becomes
 * the left pane and right-bottom becomes the right one. One pane is
 * "active": the last one that had keyboard focus.
 *
 * In a narrow window the views stack top / bottom instead of side by side;
 * the single view then splits only top / bottom. */

#include "pane.h"
#include <gtk/gtk.h>
#include <stdbool.h>

#define SFE_TYPE_SPLIT (sfe_split_get_type())
G_DECLARE_FINAL_TYPE(SfeSplit, sfe_split, SFE, SPLIT, GtkBox)

/* Signals:
 *   "active-changed"             another pane became active
 *   "directory-changed" (pane)   any pane changed directory
 *   "selection-changed" (pane)   the files selected in a pane changed */
SfeSplit* sfe_split_new(GFile* dir);

SfePane* sfe_split_get_active(SfeSplit* self);
int sfe_split_get_n_panes(SfeSplit* self);

/* 0 = left, 1 = right (top), 2 = right-bottom; NULL if absent. */
SfePane* sfe_split_get_pane(SfeSplit* self, int index);

/* Splits `pane` if the layout allows it (see above). */
void sfe_split_split(SfeSplit* self, SfePane* pane, SfeSplitKind kind);

/* Opens `dir` in a new split: a new right pane, or a new right-bottom pane
 * when there is a right one; with three panes, right-bottom goes to `dir`.
 * Returns that pane (now active). */
SfePane* sfe_split_open(SfeSplit* self, GFile* dir);

/* Closes `pane` unless it is the last one. */
void sfe_split_close(SfeSplit* self, SfePane* pane);

/* Makes the pane containing `widget` active (call when the focus moves). */
void sfe_split_track_focus(SfeSplit* self, GtkWidget* widget);

/* Moves the focus `step` panes on: 1 clockwise (left → right-top →
 * right-bottom → left), -1 counter-clockwise; it wraps around. */
void sfe_split_focus_step(SfeSplit* self, int step);

/* Makes `pane` active and focuses its view. */
void sfe_split_focus(SfeSplit* self, SfePane* pane);

/* Swaps the active pane with the one `step` places on (1 clockwise, -1
 * counter-clockwise, wrapping around); it stays active. False with one pane. */
bool sfe_split_swap(SfeSplit* self, int step);

/* The name of the pane at `index`: "left" with one pane; "left" / "right"
 * side by side or "top" / "bottom" stacked with two; "left", "right-top",
 * "right-bottom" with three. NULL past the last pane. */
const char* sfe_split_position_name(SfeSplit* self, int index);

/* The pane with that position name, or NULL. */
SfePane* sfe_split_find_position(SfeSplit* self, const char* name);
