#pragma once

/* What sfe-portal does with one file dialog request, without D-Bus: the
 * portal's OpenFile / SaveFile / SaveFiles options become the arguments of
 * `sfe --choose` (src/chooser.h), and what sfe prints becomes the portal's
 * results (uris, current_filter, choices). GIO only and unit-tested. */

#include <gio/gio.h>

typedef struct {
    GPtrArray* args; /* char*: the arguments after the program's name */
    GVariant* filters; /* a(sa(us)) offered to the user (to answer current_filter), or NULL */
    GPtrArray* save_names; /* SaveFiles: the names to put into the chosen folder, or NULL */
} SfePortalRequest;

/* `method` is "OpenFile", "SaveFile" or "SaveFiles"; `options` the a{sv}.
 *
 * Filters: `filters` as sent, starting with `current_filter`. A
 * current_filter that is not in the list is added to it, and one sent
 * without a list is the only filter (the program wants only those files).
 * A folder dialog (directory, SaveFiles) gets no filters. */
SfePortalRequest* sfe_portal_request_new(const char* method, const char* parent_window, const char* title,
    GVariant* options);
void sfe_portal_request_free(SfePortalRequest* request);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(SfePortalRequest, sfe_portal_request_free)

/* The results (a{sv}, floating) for what `sfe --choose` printed after the
 * user chose: URIs, "#filter N", "#choice ID VALUE" lines. */
GVariant* sfe_portal_request_results(const SfePortalRequest* request, const char* output);
