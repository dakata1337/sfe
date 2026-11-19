#pragma once

/* Archives through whatever tool is installed (no archive library is
 * linked). For extraction the tools are tried in order:
 *
 *   tar, tar.gz/tgz, tar.bz2, tar.xz, tar.zst, ...   bsdtar, tar
 *   zip                                              bsdtar, unzip, 7z
 *   7z                                               7z, 7zz, bsdtar
 *   rar                                              unrar, 7z, bsdtar
 *   iso                                              bsdtar, 7z
 *
 * and for creating one:
 *
 *   zip                                              bsdtar, zip, 7z
 *   tar, tar.gz, tar.xz, tar.zst                     bsdtar, tar (+ gzip, xz, zstd)
 *   7z                                               7z, 7zz, bsdtar
 *
 * GTK-free and unit-tested; the work itself runs as a file operation
 * (SFE_OP_EXTRACT and SFE_OP_COMPRESS in fileops.c). */

#include <glib.h>
#include <stdbool.h>

/* Looks up a program in PATH (g_find_program_in_path); replaceable in tests. */
typedef char* (*SfeFindProgram)(const char* name);

/* The folder an archive extracts into: "photos.tar.gz" -> "photos". */
char* sfe_archive_folder_name(const char* name);

/* The command that extracts `archive` into the existing folder `dest`, with
 * the first tool `find` locates; NULL if none is installed. */
char** sfe_archive_extract_argv(const char* archive, const char* dest, SfeFindProgram find);

/* Whether `name` is an archive and a tool to extract it is installed. */
bool sfe_archive_can_extract(const char* name);

/* A format sfe can create. */
typedef struct {
    const char* ext; /* ".tar.gz" */
    const char* label; /* "tar.gz" */
} SfeArchiveFormat;

/* The formats sfe can create, in menu order (whether or not a tool for
 * them is installed). */
const SfeArchiveFormat* sfe_archive_formats(size_t* n);

/* The command that creates `archive` (its extension picks the format) from
 * `names`, which are relative to the folder the command runs in; NULL if
 * no tool for that format is installed. */
char** sfe_archive_compress_argv(const char* archive, char* const* names, SfeFindProgram find);

/* Whether a tool to create `ext` (".zip") archives is installed. */
bool sfe_archive_can_compress(const char* ext);
