#pragma once

/* The action table: every command sfe offers, in one place.
 *
 * Each row names a GAction ("win.<name>"), its label, default shortcuts,
 * where it appears in the right-click menu and the function that runs it.
 * The window registers all rows as actions, the application installs their
 * shortcuts, and the context menu is built from them. Adding a command means
 * adding one row and one function in actions.c. */

#include "pane.h"
#include "window.h"
#include <stdbool.h>

/* What an action works on: the active pane of a window. */
typedef struct {
    SfeWindow* window;
    SfePane* pane;
    GList* files; /* selected GFile*, may be NULL */
    GFile* dir; /* the pane's directory */
} SfeActionTarget;

/* Where an action shows up in the context menu (flags). */
enum {
    SFE_MENU_NONE = 0,
    SFE_MENU_ITEMS = 1 << 0, /* right-click on selected files */
    SFE_MENU_FOLDER = 1 << 1, /* right-click on empty space */
};

typedef struct {
    const char* name;
    const char* label;
    const char* accels[3]; /* NULL-terminated */
    unsigned menu; /* SFE_MENU_* */
    int section; /* menu items with the same section are grouped */
    bool (*visible)(const SfeActionTarget* target); /* in the menu; NULL = always */
    void (*run)(const SfeActionTarget* target);
} SfeAction;

/* The table (n receives its length). */
const SfeAction* sfe_actions_get(size_t* n);

/* Registers every action, plus the parameterized "open-with" (s) and
 * "user-action" (i) ones, on `window`. */
void sfe_actions_add_to_window(SfeWindow* window);

/* Updates check-mark actions (e.g. "Show Hidden Files") after a config change. */
void sfe_actions_sync_state(SfeWindow* window);

/* Installs the default shortcuts of every action. */
void sfe_actions_set_accels(GtkApplication* app);

/* Builds the target for `window`'s active pane (free with the function below). */
SfeActionTarget sfe_action_target_for(SfeWindow* window);
void sfe_action_target_clear(SfeActionTarget* target);
