#pragma once

#include <glib.h>
#include <stdbool.h>

/* Expands a leading "~" or "~/" to the home directory.
 * Returns a newly allocated string. */
char* sfe_expand_home(const char* path);

/* Replaces the home directory prefix with "~" (inverse of sfe_expand_home).
 * Returns a newly allocated string. */
char* sfe_contract_home(const char* path);

/* Splits a file name into stem and extension: "photo.jpg" -> "photo" +
 * ".jpg", "a.tar.gz" -> "a" + ".tar.gz". Directories and dot files such as
 * ".bashrc" have no extension. Both results are newly allocated. */
void sfe_split_extension(const char* name, bool is_dir, char** stem, char** ext);
