#pragma once

/* Archive extraction through whatever tool is installed (no archive library
 * is linked). For each format the tools are tried in order:
 *
 *   tar, tar.gz/tgz, tar.bz2, tar.xz, tar.zst, ...   bsdtar, tar
 *   zip                                              bsdtar, unzip, 7z
 *   7z                                               7z, 7zz, bsdtar
 *   rar                                              unrar, 7z, bsdtar
 *   iso                                              bsdtar, 7z
 *
 * GTK-free and unit-tested; the extraction itself runs as a file operation
 * (SFE_OP_EXTRACT in fileops.c). */

#include <glib.h>
#include <stdbool.h>

/* Looks up a program in PATH (g_find_program_in_path); replaceable in tests. */
typedef char* (*SfeFindProgram)(const char* name);

/* Whether the name has an extension sfe knows how to extract. */
bool sfe_archive_is_archive(const char* name);

/* The folder an archive extracts into: "photos.tar.gz" -> "photos". */
char* sfe_archive_folder_name(const char* name);

/* The command that extracts `archive` into the existing folder `dest`, with
 * the first tool `find` locates; NULL if none is installed. */
char** sfe_archive_extract_argv(const char* archive, const char* dest, SfeFindProgram find);

/* Whether `name` is an archive and a tool to extract it is installed. */
bool sfe_archive_can_extract(const char* name);
