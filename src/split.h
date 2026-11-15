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
 *   "directory-changed" (pane)   any pane changed directory */
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

/* Moves the focus to the next pane (left → right-top → right-bottom). */
void sfe_split_focus_next(SfeSplit* self);

typedef enum {
    SFE_DIRECTION_LEFT,
    SFE_DIRECTION_RIGHT,
    SFE_DIRECTION_UP,
    SFE_DIRECTION_DOWN,
} SfeDirection;

/* The pane next to `pane` in that direction on screen (so narrow, stacked
 * windows work too). At the edge it wraps around to the farthest pane the
 * other way (left from the left view: the right one). NULL when no other
 * pane is on that line. */
SfePane* sfe_split_neighbor(SfeSplit* self, SfePane* pane, SfeDirection direction);

/* Makes `pane` active and focuses its view. */
void sfe_split_focus(SfeSplit* self, SfePane* pane);

/* Swaps the active pane with its neighbor in that direction (wrapping
 * around like sfe_split_neighbor()); it stays active. False if there is
 * none. */
bool sfe_split_swap(SfeSplit* self, SfeDirection direction);

/* The name of the pane at `index`: "left" with one pane; "left" / "right"
 * side by side or "top" / "bottom" stacked with two; "left", "right-top",
 * "right-bottom" with three. NULL past the last pane. */
const char* sfe_split_position_name(SfeSplit* self, int index);

/* The pane with that position name, or NULL. */
SfePane* sfe_split_find_position(SfeSplit* self, const char* name);
