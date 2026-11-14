#pragma once

/* A top-level sfe window: sidebar | directory panes. */

#include "app.h"
#include <gtk/gtk.h>

#define SFE_TYPE_WINDOW (sfe_window_get_type())
G_DECLARE_FINAL_TYPE(SfeWindow, sfe_window, SFE, WINDOW, GtkApplicationWindow)

/* Opens `dir`, or the configured start path when `dir` is NULL. */
SfeWindow* sfe_window_new(SfeApplication* app, GFile* dir);
