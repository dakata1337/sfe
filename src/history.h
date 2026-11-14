#pragma once

/* Back/forward navigation history of one pane. */

#include <gio/gio.h>
#include <stdbool.h>

typedef struct SfeHistory SfeHistory;

SfeHistory* sfe_history_new(void);
void sfe_history_free(SfeHistory* h);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(SfeHistory, sfe_history_free)

/* Records leaving `from` for a new location; clears the forward stack. */
void sfe_history_push(SfeHistory* h, GFile* from);

/* Steps back/forward from `current`. Returns the location to go to (new
 * reference) or NULL when there is nothing in that direction. */
GFile* sfe_history_back(SfeHistory* h, GFile* current);
GFile* sfe_history_forward(SfeHistory* h, GFile* current);

bool sfe_history_can_back(const SfeHistory* h);
bool sfe_history_can_forward(const SfeHistory* h);
