#pragma once

/* Fuzzy matching of file names, used by the search overlay and (later) the
 * command line completion.
 *
 * Matches are ranked in tiers, best first:
 *   exact name  >  exact name without extension  >  prefix  >
 *   substring at a word start  >  substring  >  fuzzy (letters in order)
 * Within a tier, shorter names and earlier matches win; fuzzy matches favour
 * consecutive letters and letters at word starts ("fb" -> "foo_bar").
 * Matching ignores case unless the query contains an uppercase letter. */

#include <glib.h>

#define SFE_FUZZY_NO_MATCH (-1)

/* Higher is better; SFE_FUZZY_NO_MATCH when `text` does not match. An empty
 * query matches everything with score 0. */
int sfe_fuzzy_score(const char* query, const char* text);
