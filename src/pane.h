#pragma once

/* One directory view: top bar (back, forward, parent, path entry, view
 * toggle, split and close buttons) above a grid or list of files.
 *
 * Signals: "directory-changed", "split" (SfeSplitKind), "close". */

#include "config.h"
#include "filefilter.h"
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

/* Opens (or focuses) the floating fuzzy search of this pane. */
void sfe_pane_start_search(SfePane* self);
void sfe_pane_focus_view(SfePane* self);
void sfe_pane_set_view(SfePane* self, SfeViewMode mode);
SfeViewMode sfe_pane_get_view(SfePane* self);

/* Enables the split / close buttons (the split container decides). */
void sfe_pane_set_split_buttons(SfePane* self, bool vertical, bool horizontal, bool close);

/* Marks the pane that receives shortcuts, paste and sidebar navigation. */
void sfe_pane_set_active(SfePane* self, bool active);

/* Selected files as a list of GFile* (free with g_list_free_full(l, g_object_unref)). */
GList* sfe_pane_get_selected_files(SfePane* self);
/* Every file shown, in display order (same freeing as above). */
GList* sfe_pane_get_all_files(SfePane* self);
void sfe_pane_select_all(SfePane* self);

/* A file dialog's type filter for this view (borrowed), or NULL. */
void sfe_pane_set_file_filter(SfePane* self, const SfeFileFilter* filter);

/* A row of the view, for command line completion. */
typedef struct {
    char* name; /* display name */
    GFile* file;
    bool is_dir;
} SfePaneEntry;

/* The rows shown now (SfePaneEntry*, freed with the array); no disk access. */
GPtrArray* sfe_pane_get_entries(SfePane* self);

typedef enum {
    SFE_CURSOR_DOWN,
    SFE_CURSOR_UP,
    SFE_CURSOR_FIRST,
    SFE_CURSOR_LAST,
    SFE_CURSOR_PREVIOUS, /* the item before / after (in a grid: on the same row) */
    SFE_CURSOR_NEXT,
} SfeCursorMove;

/* Moves the cursor like the arrow keys / Home / End (selecting the item it
 * lands on, as they do), focusing the view. */
void sfe_pane_move_cursor(SfePane* self, SfeCursorMove move);

/* Toggles the selection of the item under the cursor and moves the cursor
 * to the next item without touching the selection. */
void sfe_pane_toggle_cursor_item(SfePane* self);

/* Visual mode (v): the cursor moves without touching the selection and a
 * "chunk" of rows from an anchor to the cursor is selected, like a
 * Shift+click range. Ending a chunk keeps it and lets the cursor move
 * freely to start the next one, so several chunks add up like Ctrl+clicks.
 * Leaving visual mode never changes the selection; going to another folder,
 * switching the view, searching, clicking or another view becoming active
 * leave it. Emits "visual-changed". */
void sfe_pane_visual_start(SfePane* self);
/* Ends the chunk being drawn, or starts a new one at the cursor (enters
 * visual mode if needed). */
void sfe_pane_visual_chunk(SfePane* self);
/* The cursor jumps to the other end of the chunk (vim's "o"). */
void sfe_pane_visual_swap(SfePane* self);
void sfe_pane_visual_end(SfePane* self);
bool sfe_pane_in_visual(SfePane* self);
/* Whether a chunk is being drawn (rather than moving between chunks). */
bool sfe_pane_visual_drawing(SfePane* self);

/* Scrolls the cursor's item to the middle of the view; in visual mode the
 * first selected item. */
void sfe_pane_center_cursor(SfePane* self);

/* Opens files like double-clicking them (directories open in this pane). */
void sfe_pane_open_files(SfePane* self, GList* files);

/* Selects the entry `name`, now or as soon as it appears (after creating it). */
void sfe_pane_reveal(SfePane* self, const char* name);
