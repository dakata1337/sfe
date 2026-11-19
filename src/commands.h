#pragma once

/* The ":" commands (PLAN.md section 7) and their completion.
 *
 * Every row of the action table is a command too ("paste", "preferences",
 * "show_hidden", ...: dashes become underscores); the commands here add
 * arguments: ":copy a b ~/backup", ":cd ~/Projects", ":split move left".
 * Normal-mode keys run commands through the same function. */

#include "cmdline.h"
#include "window.h"
#include <stdbool.h>

/* A command, for the user guide and the completion list. */
typedef struct {
    const char* name; /* the main (short) name */
    const char* aliases[3]; /* other names, NULL-terminated */
    const char* usage; /* the arguments, "" for none */
    const char* help;
} SfeCommandInfo;

/* Runs one command line in `window`; false with `error` set (shown in the
 * command bar). */
bool sfe_command_run(SfeWindow* window, const char* line, GError** error);

/* Completion of the argument the cursor (a byte offset) is in: ranked
 * candidates (SfeCandidate*), and the byte range of `line` that the chosen
 * one replaces (quoted with sfe_cmdline_quote()). `rank` receives the text
 * the candidates were matched against (for highlighting; free it). */
GPtrArray* sfe_command_complete(SfeWindow* window, const char* line, int cursor, int* start, int* end, char** rank);
