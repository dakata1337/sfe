#pragma once

/* Opening files: per-extension commands from the [open] config table, falling
 * back to xdg-open (or GIO's default handler when xdg-open is missing). */

#include "config.h"
#include <gio/gio.h>

/* The override command for `filename`, or NULL. The longest matching
 * extension wins: "a.tar.gz" tries "tar.gz" before "gz". */
const char* sfe_opener_lookup(GHashTable* open_with, const char* filename);

/* Splits `command` like a shell and replaces "%f" with `path` (without
 * shell interpretation, so any file name is safe). If the command has no
 * "%f", the path is appended as the last argument. */
char** sfe_opener_build_argv(const char* command, const char* path, GError** error);

/* Opens `file` in the default or configured application, without blocking. */
bool sfe_open_file(const SfeConfig* cfg, GFile* file, GError** error);

/* Builds the argv of a custom [[actions]] command. An argument that is
 * exactly "%f" becomes one argument per file; "%f" inside a longer argument
 * becomes the first file; "%d" becomes the directory. No shell is involved. */
char** sfe_build_user_command(const char* command, GList* files, GFile* dir, GError** error);

/* Runs a custom command in `dir` without blocking. */
bool sfe_run_user_command(const char* command, GList* files, GFile* dir, GError** error);
