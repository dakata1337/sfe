#pragma once

/* The floating search field of a pane (Ctrl+F): a card with an entry and a
 * "matches / total" counter, shown over the files. The pane places it and
 * does the filtering; this widget only reports what the user does:
 *
 *   "query-changed" (const char*)   the text changed
 *   "activate"                      Enter: keep the matches as a filter
 *   "move" (int)                    Up / Down: highlight another match
 *   "close"                         Esc */

#include <gtk/gtk.h>

#define SFE_TYPE_SEARCH_BAR (sfe_search_bar_get_type())
G_DECLARE_FINAL_TYPE(SfeSearchBar, sfe_search_bar, SFE, SEARCH_BAR, GtkBox)

GtkWidget* sfe_search_bar_new(void);

/* Shows the bar (if hidden) and puts the cursor in it, text selected. */
void sfe_search_bar_focus(SfeSearchBar* self);

/* The text typed; setting it emits "query-changed". */
const char* sfe_search_bar_get_text(SfeSearchBar* self);
void sfe_search_bar_set_text(SfeSearchBar* self, const char* text);

/* Clears the text without emitting "query-changed". */
void sfe_search_bar_reset(SfeSearchBar* self);

/* "12 / 300" (shown / in the folder), or "best 1000 of 15000" when more
 * matched than are shown. */
void sfe_search_bar_set_count(SfeSearchBar* self, int shown, int matched, int total);
