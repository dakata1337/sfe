#pragma once

/* A top-level sfe window: sidebar | directory panes. */

#include "app.h"
#include "fileops.h"
#include <gtk/gtk.h>
#include <stdbool.h>

typedef struct _SfeSplit SfeSplit;
typedef struct _SfePane SfePane;

#define SFE_TYPE_WINDOW (sfe_window_get_type())
G_DECLARE_FINAL_TYPE(SfeWindow, sfe_window, SFE, WINDOW, GtkApplicationWindow)

/* Opens `dir`, or the configured start path when `dir` is NULL. */
SfeWindow* sfe_window_new(SfeApplication* app, GFile* dir);

SfeSplit* sfe_window_get_split(SfeWindow* win);

/* The pane that shortcuts and the sidebar act on. */
SfePane* sfe_window_get_active_pane(SfeWindow* win);

/* Shows progress for `op` in the window and starts it. */
void sfe_window_run_op(SfeWindow* win, SfeFileOp* op);

/* Undoes the history step `id` (0: the newest) and says so in the command
 * bar; false (with the reason shown) if it could not. */
bool sfe_window_undo(SfeWindow* win, guint id);

/* Opens the ":" command line, with `text` typed in (or empty). */
void sfe_window_show_command_line(SfeWindow* win, const char* text);

/* Hides or shows the sidebar (Ctrl+S, the minimal look). */
void sfe_window_toggle_sidebar(SfeWindow* win);
void sfe_window_set_sidebar_visible(SfeWindow* win, bool visible);
bool sfe_window_get_sidebar_visible(SfeWindow* win);

/* Lets the folder tree fill the sidebar, or gives the places their part
 * back (the button above the tree). Shows the sidebar if it was hidden. */
void sfe_window_toggle_tree(SfeWindow* win);
void sfe_window_set_tree_full(SfeWindow* win, bool full);
bool sfe_window_get_tree_full(SfeWindow* win);

/* A line in the command bar until the next key: a note, or in the error
 * colour. */
void sfe_window_show_message(SfeWindow* win, const char* text, bool error);
