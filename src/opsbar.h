#pragma once

/* A bar at the bottom of a window listing running file operations (status,
 * progress, cancel button). It also asks the operations' questions
 * ("already exists", errors) and reports failures. It slides in only for
 * jobs that take longer than a moment. */

#include "fileops.h"
#include <gtk/gtk.h>

#define SFE_TYPE_OPS_BAR (sfe_ops_bar_get_type())
G_DECLARE_FINAL_TYPE(SfeOpsBar, sfe_ops_bar, SFE, OPS_BAR, GtkRevealer)

GtkWidget* sfe_ops_bar_new(void);

/* Shows and handles `op`, then starts it. */
void sfe_ops_bar_run(SfeOpsBar* self, SfeFileOp* op);
