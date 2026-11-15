/* sfe-portal: an xdg-desktop-portal backend that answers the desktop's file
 * dialogs (org.freedesktop.impl.portal.FileChooser) with sfe.
 *
 * Programs that ask the portal for a file (browsers such as Firefox / Zen,
 * Flatpak apps, GTK apps with GTK_USE_PORTAL=1) reach xdg-desktop-portal,
 * which forwards the request to the backend named for FileChooser in
 * ~/.config/xdg-desktop-portal/<desktop>-portals.conf:
 *
 *   [preferred]
 *   org.freedesktop.impl.portal.FileChooser=sfe
 *
 * For each request this runs `sfe --choose ...` (see src/chooser.h) and
 * returns the URIs it prints, with the filter in use and the values of the
 * program's extra options ("choices"). GIO only. */

#include "portalreq.h"

#include <gio/gio.h>
#include <stdbool.h>
#include <string.h>

#define BUS_NAME "org.freedesktop.impl.portal.desktop.sfe"
#define OBJECT_PATH "/org/freedesktop/portal/desktop"

#ifndef SFE_BINDIR
#define SFE_BINDIR "/usr/bin"
#endif

enum { RESPONSE_OK = 0, RESPONSE_CANCELLED = 1, RESPONSE_OTHER = 2 };

static const char interface_xml[] =
    "<node>"
    "  <interface name='org.freedesktop.impl.portal.FileChooser'>"
    "    <method name='OpenFile'>"
    "      <arg type='o' name='handle' direction='in'/><arg type='s' name='app_id' direction='in'/>"
    "      <arg type='s' name='parent_window' direction='in'/><arg type='s' name='title' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/><arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <method name='SaveFile'>"
    "      <arg type='o' name='handle' direction='in'/><arg type='s' name='app_id' direction='in'/>"
    "      <arg type='s' name='parent_window' direction='in'/><arg type='s' name='title' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/><arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <method name='SaveFiles'>"
    "      <arg type='o' name='handle' direction='in'/><arg type='s' name='app_id' direction='in'/>"
    "      <arg type='s' name='parent_window' direction='in'/><arg type='s' name='title' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/><arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <property name='version' type='u' access='read'/>"
    "  </interface>"
    "</node>";

static const char request_xml[] =
    "<node><interface name='org.freedesktop.impl.portal.Request'><method name='Close'/></interface></node>";

static GDBusNodeInfo* interface_info;
static GDBusNodeInfo* request_info;
static GMainLoop* loop;

/* With no dialog open for this long the backend exits, so an updated
 * sfe-portal takes over at the next request (D-Bus starts it again). */
#define IDLE_EXIT_SECONDS 120

static guint idle_source;
static int open_dialogs;

static gboolean on_idle_exit(gpointer data)
{
    (void)data;
    idle_source = 0;
    g_debug("idle for %d s: exiting", IDLE_EXIT_SECONDS);
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

static void dialogs_changed(int delta)
{
    open_dialogs += delta;
    g_clear_handle_id(&idle_source, g_source_remove);
    if (open_dialogs == 0)
        idle_source = g_timeout_add_seconds(IDLE_EXIT_SECONDS, on_idle_exit, NULL);
}

/* One dialog in progress. */
typedef struct {
    GDBusMethodInvocation* invocation;
    GDBusConnection* connection;
    GSubprocess* chooser;
    guint request_id; /* the Request object at the handle path */
    SfePortalRequest* request;
} Dialog;

static void dialog_free(Dialog* d)
{
    if (d->request_id != 0)
        g_dbus_connection_unregister_object(d->connection, d->request_id);
    g_clear_object(&d->chooser);
    g_object_unref(d->connection);
    sfe_portal_request_free(d->request);
    g_free(d);
    dialogs_changed(-1);
}

static void reply(Dialog* d, guint response, GVariant* results)
{
    if (results == NULL)
        results = g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0);
    g_dbus_method_invocation_return_value(d->invocation, g_variant_new("(u@a{sv})", response, results));
    d->invocation = NULL;
}

/* The app or the frontend gave up on the dialog: close sfe. */
static void on_request_call(GDBusConnection* connection, const char* sender, const char* path, const char* iface,
    const char* method, GVariant* params, GDBusMethodInvocation* invocation, gpointer data)
{
    (void)connection, (void)sender, (void)path, (void)iface, (void)params;
    Dialog* d = data;
    if (g_strcmp0(method, "Close") == 0 && d->chooser != NULL)
        g_subprocess_force_exit(d->chooser);
    g_dbus_method_invocation_return_value(invocation, NULL);
}

static void on_chooser_done(GObject* source, GAsyncResult* res, gpointer data)
{
    Dialog* d = data;
    g_autofree char* out = NULL;
    g_autoptr(GError) error = NULL;
    bool ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), res, &out, NULL, &error);

    if (!ok || !g_subprocess_get_if_exited(d->chooser) || g_subprocess_get_exit_status(d->chooser) != 0) {
        if (error != NULL)
            g_warning("sfe --choose: %s", error->message);
        reply(d, g_subprocess_get_if_exited(d->chooser) ? RESPONSE_CANCELLED : RESPONSE_OTHER, NULL);
    } else {
        reply(d, RESPONSE_OK, sfe_portal_request_results(d->request, out));
    }
    dialog_free(d);
}

/* The sfe to run: SFE_PORTAL_SFE (tests, uninstalled builds), else the
 * installed one. */
static const char* sfe_binary(void)
{
    const char* env = g_getenv("SFE_PORTAL_SFE");
    return env && *env ? env : SFE_BINDIR "/sfe";
}

static void on_method_call(GDBusConnection* connection, const char* sender, const char* path, const char* iface,
    const char* method, GVariant* params, GDBusMethodInvocation* invocation, gpointer data)
{
    (void)sender, (void)path, (void)iface, (void)data;
    const char *handle, *app_id, *parent, *title;
    g_autoptr(GVariant) options = NULL;
    g_variant_get(params, "(&o&s&s&s@a{sv})", &handle, &app_id, &parent, &title, &options);
    if (g_log_writer_default_would_drop(G_LOG_LEVEL_DEBUG, G_LOG_DOMAIN) == FALSE) {
        g_autofree char* text = g_variant_print(options, TRUE);
        g_debug("%s from %s (%s): %s %s", method, app_id, parent, title, text);
    }

    Dialog* d = g_new0(Dialog, 1);
    dialogs_changed(+1);
    d->invocation = invocation;
    d->connection = g_object_ref(connection);
    d->request = sfe_portal_request_new(method, parent, title, options);
    g_autoptr(GPtrArray) argv = g_ptr_array_new();
    g_ptr_array_add(argv, (char*)sfe_binary());
    for (guint i = 0; i < d->request->args->len; i++)
        g_ptr_array_add(argv, g_ptr_array_index(d->request->args, i));
    g_ptr_array_add(argv, NULL);

    static const GDBusInterfaceVTable request_vtable = { on_request_call, NULL, NULL, { 0 } };
    d->request_id = g_dbus_connection_register_object(connection, handle, request_info->interfaces[0],
        &request_vtable, d, NULL, NULL);

    g_autoptr(GError) error = NULL;
    d->chooser = g_subprocess_newv((const char* const*)argv->pdata, G_SUBPROCESS_FLAGS_STDOUT_PIPE, &error);
    if (d->chooser == NULL) {
        g_warning("cannot run %s: %s", (char*)argv->pdata[0], error->message);
        reply(d, RESPONSE_OTHER, NULL);
        dialog_free(d);
        return;
    }
    g_subprocess_communicate_utf8_async(d->chooser, NULL, NULL, on_chooser_done, d);
}

static GVariant* on_get_property(GDBusConnection* connection, const char* sender, const char* path, const char* iface,
    const char* property, GError** error, gpointer data)
{
    (void)connection, (void)sender, (void)path, (void)iface, (void)error, (void)data;
    return g_strcmp0(property, "version") == 0 ? g_variant_new_uint32(3) : NULL;
}

static void on_bus_acquired(GDBusConnection* connection, const char* name, gpointer data)
{
    (void)name, (void)data;
    static const GDBusInterfaceVTable vtable = { on_method_call, on_get_property, NULL, { 0 } };
    g_autoptr(GError) error = NULL;
    if (g_dbus_connection_register_object(connection, OBJECT_PATH, interface_info->interfaces[0], &vtable, NULL,
            NULL, &error)
        == 0) {
        g_printerr("sfe-portal: %s\n", error->message);
        g_main_loop_quit(loop);
    }
}

static void on_name_lost(GDBusConnection* connection, const char* name, gpointer data)
{
    (void)connection, (void)data;
    g_printerr("sfe-portal: cannot own %s (another one running?)\n", name);
    g_main_loop_quit(loop);
}

int main(void)
{
    interface_info = g_dbus_node_info_new_for_xml(interface_xml, NULL);
    request_info = g_dbus_node_info_new_for_xml(request_xml, NULL);
    loop = g_main_loop_new(NULL, FALSE);
    guint owner = g_bus_own_name(G_BUS_TYPE_SESSION, BUS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE, on_bus_acquired, NULL,
        on_name_lost, NULL, NULL);
    dialogs_changed(0); /* the idle timer starts now */
    g_main_loop_run(loop);
    g_bus_unown_name(owner);
    return 0;
}
