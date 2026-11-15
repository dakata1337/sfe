#pragma once

/* The ":" command line: a bar across the bottom of the views with the
 * completion list above it, and the keys of a pending chord (showcmd).
 *
 * Enter runs the command (sfe_command_run), Esc cancels, Tab / Shift+Tab
 * cycle through the completions, Up / Down walk the session's history.
 * Errors stay in the bar, in the error colour, until the next key.
 *
 * Emits "done" when the bar closes, so the window can focus the view. */

#include "window.h"
#include <gtk/gtk.h>
#include <stdbool.h>

#define SFE_TYPE_COMMAND_BAR (sfe_command_bar_get_type())
G_DECLARE_FINAL_TYPE(SfeCommandBar, sfe_command_bar, SFE, COMMAND_BAR, GtkBox)

SfeCommandBar* sfe_command_bar_new(SfeWindow* window);

/* Opens the bar for typing, with `text` already typed (or empty). */
void sfe_command_bar_start(SfeCommandBar* self, const char* text);

bool sfe_command_bar_is_active(SfeCommandBar* self);

/* Shows `message` in the error colour until sfe_command_bar_dismiss(). */
void sfe_command_bar_show_error(SfeCommandBar* self, const char* message);

/* The same in the normal colour ("undid: copy 3 items to ~/Backup"). */
void sfe_command_bar_show_message(SfeCommandBar* self, const char* message);

/* Hides an error message (the window calls this on the next key). */
void sfe_command_bar_dismiss(SfeCommandBar* self);
