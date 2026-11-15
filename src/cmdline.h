#pragma once

/* The ":" command line, without the UI: splitting a line into arguments
 * like a shell does, quoting names for insertion, and ranking completion
 * candidates. GTK-free and unit-tested.
 *
 * Splitting: whitespace separates arguments; '...' is literal; "..." is
 * literal except for \" and \\; a backslash outside quotes takes the next
 * character literally. While the user is still typing, a quote may be left
 * open: the last argument then runs to the end of the line. */

#include <glib.h>
#include <stdbool.h>

typedef struct {
    char* value; /* the argument with quoting removed */
    int start; /* byte offsets of the argument in the line */
    int end;
    bool open_quote; /* ends inside an unfinished quote */
} SfeCmdArg;

/* All arguments (SfeCmdArg*), for completion. */
GPtrArray* sfe_cmdline_tokenize(const char* line);

/* The arguments of a finished line (NULL-terminated); NULL with an error
 * when a quote is left open. An empty line gives an empty vector. */
char** sfe_cmdline_split(const char* line, GError** error);

/* `arg` written so that sfe_cmdline_split() gives it back: special
 * characters get a backslash ("My Files" -> "My\ Files"). */
char* sfe_cmdline_quote(const char* arg);

/* ------------------------------------------------------------------------ */
/* Completion                                                                */

typedef struct {
    char* insert; /* what replaces the argument (unquoted) */
    char* match; /* what the typed text is matched against (a file name) */
    char* tag; /* where it comes from: "left", "right-top", "bookmark", ... */
    char* label; /* shown instead of `insert` ("cp | copy"), or NULL */
    char* args; /* shown dimmed after it ("<file>… <destination>"), or NULL */
    char** names; /* alternatives (a command and its aliases), or NULL */
    bool dim; /* shown dimmed (commands meant for key bindings) */
    int group; /* lower groups come first: the active view is group 0 */
    int score; /* filled in by sfe_cmdline_rank() */
    int order; /* insertion order, the last tie-breaker */
} SfeCandidate;

SfeCandidate* sfe_candidate_new(const char* insert, const char* match, const char* tag, int group);

/* One row for several names (a command and its aliases), labelled
 * "mkdir | new_directory | new_folder". Ranking scores every name and makes
 * the best-matching one the `insert` and the `match`. */
SfeCandidate* sfe_candidate_new_names(const char* const* names, const char* tag, int group);
void sfe_candidate_free(gpointer candidate);

/* Keeps the candidates that fuzzy-match `query` and orders them: by group,
 * then best match first (sfe_fuzzy_score), then in the order they were
 * added. Duplicates of an earlier `insert` are dropped. Takes `candidates`
 * (SfeCandidate*, freed with sfe_candidate_free) and returns the result. */
GPtrArray* sfe_cmdline_rank(GPtrArray* candidates, const char* query);
