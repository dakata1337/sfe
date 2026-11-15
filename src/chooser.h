#pragma once

/* Chooser mode: sfe as a file dialog for other programs.
 *
 *   sfe --choose open   [--multiple] [--folder DIR] [--title T] [--accept-label L]
 *   sfe --choose folder [--folder DIR] ...
 *   sfe --choose save   [--name NAME] [--folder DIR] ...
 *   ... [--parent wayland:HANDLE | x11:XID]  (attach to the asking program)
 *   ... [--filters 'a(sa(us)) text' [--current-filter N]] [--choices 'a(ssa(ss)s) text']
 *
 * Before the URIs, lines starting with "#" answer the rest: "#filter N"
 * (the filter in use) and "#choice ID VALUE" (each option's value).
 *
 * A separate sfe instance opens one window with a bar at the bottom
 * ([name] Cancel / Open). What is chosen is printed to standard output as
 * file URIs, one per line, and sfe exits with 0; cancelling exits with 1.
 * sfe-portal (src/portal) uses this to answer the desktop's file dialogs. */

#include <stdbool.h>

typedef enum {
    SFE_CHOOSE_OPEN, /* existing files */
    SFE_CHOOSE_SAVE, /* a new (or replaced) file name */
    SFE_CHOOSE_FOLDER, /* a folder */
} SfeChooseMode;

typedef struct {
    SfeChooseMode mode;
    bool multiple; /* open: several files */
    char* title; /* window title, or NULL */
    char* folder; /* where to start, or NULL */
    char* name; /* save: the suggested file name, or NULL */
    char* accept_label; /* "Open", "Save", "Upload" ..., or NULL */
    char* parent; /* the asking program's window: "wayland:HANDLE" / "x11:XID", or NULL */
    char* filters; /* type filters, GVariant text a(sa(us)), or NULL */
    int current_filter; /* index into them, -1 for the first */
    char* choices; /* extra options, GVariant text a(ssa(ss)s), or NULL */
} SfeChooser;

void sfe_chooser_free(SfeChooser* chooser);

/* "open" / "save" / "folder" -> mode; false for anything else. */
bool sfe_chooser_parse_mode(const char* text, SfeChooseMode* mode);
