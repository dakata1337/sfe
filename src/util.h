#pragma once

#include <glib.h>

/* Expands a leading "~" or "~/" to the home directory.
 * Returns a newly allocated string. */
char* sfe_expand_home(const char* path);

/* Replaces the home directory prefix with "~" (inverse of sfe_expand_home).
 * Returns a newly allocated string. */
char* sfe_contract_home(const char* path);
