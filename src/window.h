#pragma once

/* A top-level sfe window: sidebar | directory panes. */

#include "app.h"
#include "fileops.h"
#include <gtk/gtk.h>

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

/* Opens the ":" command line, with `text` typed in (or empty). */
void sfe_window_show_command_line(SfeWindow* win, const char* text);

/* Hides or shows the sidebar (Ctrl+S, the minimal look). */
void sfe_window_toggle_sidebar(SfeWindow* win);

/* Lets the folder tree fill the sidebar, or gives the places their part
 * back (the button above the tree). Shows the sidebar if it was hidden. */
void sfe_window_toggle_tree(SfeWindow* win);
