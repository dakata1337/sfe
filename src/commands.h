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
    const char* name;
    const char* alias; /* or NULL */
    const char* usage; /* the arguments, "" for none */
    const char* help;
} SfeCommandInfo;

/* The commands with arguments (n receives the count). */
const SfeCommandInfo* sfe_commands_info(size_t* n);

/* Runs one command line in `window`; false with `error` set (shown in the
 * command bar). */
bool sfe_command_run(SfeWindow* window, const char* line, GError** error);

/* Completion of the argument the cursor (a byte offset) is in: ranked
 * candidates (SfeCandidate*), and the byte range of `line` that the chosen
 * one replaces (quoted with sfe_cmdline_quote()). */
GPtrArray* sfe_command_complete(SfeWindow* window, const char* line, int cursor, int* start, int* end);
