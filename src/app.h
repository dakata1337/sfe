#pragma once

/* The application: owns the configuration and all windows. One process
 * serves every sfe window, so clipboard operations work between them. */

#include "chooser.h"
#include "config.h"
#include <gtk/gtk.h>

#define SFE_TYPE_APPLICATION (sfe_application_get_type())
G_DECLARE_FINAL_TYPE(SfeApplication, sfe_application, SFE, APPLICATION, GtkApplication)

SfeApplication* sfe_application_new(void);

/* The running application instance. */
SfeApplication* sfe_app(void);

/* The current configuration. Do not keep the pointer: it is replaced on every
 * reload. Connect to the "config-changed" signal to react to changes. */
const SfeConfig* sfe_config(void);

/* Takes ownership of `cfg`, writes it to disk and emits "config-changed". */
void sfe_application_set_config(SfeApplication* app, SfeConfig* cfg);

/* Chooser mode (sfe --choose, see chooser.h): its options, or NULL; the
 * exit status (0 chosen, 1 cancelled), which main() returns. */
const SfeChooser* sfe_application_get_chooser(SfeApplication* app);
void sfe_application_set_exit_status(SfeApplication* app, int status);
/* Writes one line of the chooser's answer to the real standard output. */
void sfe_application_answer(SfeApplication* app, const char* line);
int sfe_application_get_exit_status(SfeApplication* app, int run_status);

/* A copy of the current configuration, for editing and passing back to
 * sfe_application_set_config(). */
SfeConfig* sfe_application_dup_config(SfeApplication* app);

/* Sorts every view by `key` in `order` and saves it ([general] sort_by,
 * sort_order); nothing happens when that is the sort already. */
void sfe_application_set_sort(SfeApplication* app, SfeSortKey key, SfeSortOrder order);
