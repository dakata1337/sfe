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

/* One dialog in progress. */
typedef struct {
    GDBusMethodInvocation* invocation;
    GDBusConnection* connection;
    GSubprocess* chooser;
    guint request_id; /* the Request object at the handle path */
    GPtrArray* save_names; /* SaveFiles: names to put into the chosen folder */
    GVariant* filters; /* a(sa(us)) as asked, to answer current_filter, or NULL */
} Dialog;

static void dialog_free(Dialog* d)
{
    if (d->request_id != 0)
        g_dbus_connection_unregister_object(d->connection, d->request_id);
    g_clear_object(&d->chooser);
    g_object_unref(d->connection);
    g_clear_pointer(&d->save_names, g_ptr_array_unref);
    g_clear_pointer(&d->filters, g_variant_unref);
    g_free(d);
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
        dialog_free(d);
        return;
    }
    GVariantBuilder uris, choices;
    g_variant_builder_init(&uris, G_VARIANT_TYPE_STRING_ARRAY);
    g_variant_builder_init(&choices, G_VARIANT_TYPE("a(ss)"));
    bool any_choice = false;
    int filter = -1;
    g_auto(GStrv) lines = g_strsplit(out ? out : "", "\n", -1);
    for (int i = 0; lines[i] != NULL; i++) {
        if (*lines[i] == '\0')
            continue;
        if (g_str_has_prefix(lines[i], "#filter ")) { /* the filter in use */
            filter = (int)g_ascii_strtoll(lines[i] + strlen("#filter "), NULL, 10);
            continue;
        }
        if (g_str_has_prefix(lines[i], "#choice ")) { /* "#choice ID VALUE" */
            g_auto(GStrv) parts = g_strsplit(lines[i] + strlen("#choice "), " ", 2);
            if (parts[0] != NULL) {
                g_variant_builder_add(&choices, "(ss)", parts[0], parts[1] ? parts[1] : "");
                any_choice = true;
            }
            continue;
        }
        if (lines[i][0] == '#')
            continue;
        if (d->save_names == NULL) {
            g_variant_builder_add(&uris, "s", lines[i]);
            continue;
        }
        /* SaveFiles: the user chose a folder; each file goes into it */
        g_autoptr(GFile) folder = g_file_new_for_uri(lines[i]);
        for (guint k = 0; k < d->save_names->len; k++) {
            g_autoptr(GFile) file = g_file_get_child(folder, g_ptr_array_index(d->save_names, k));
            g_autofree char* uri = g_file_get_uri(file);
            g_variant_builder_add(&uris, "s", uri);
        }
        break;
    }
    GVariantBuilder results;
    g_variant_builder_init(&results, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&results, "{sv}", "uris", g_variant_builder_end(&uris));
    GVariant* choice_list = g_variant_builder_end(&choices);
    if (any_choice)
        g_variant_builder_add(&results, "{sv}", "choices", choice_list);
    else
        g_variant_unref(g_variant_ref_sink(choice_list));
    if (d->filters != NULL && filter >= 0 && filter < (int)g_variant_n_children(d->filters))
        g_variant_builder_add(&results, "{sv}", "current_filter", g_variant_get_child_value(d->filters, filter));
    reply(d, RESPONSE_OK, g_variant_builder_end(&results));
    dialog_free(d);
}

static char* option_bytestring(GVariant* options, const char* key)
{
    g_autoptr(GVariant) v = g_variant_lookup_value(options, key, G_VARIANT_TYPE_BYTESTRING);
    return v ? g_strdup(g_variant_get_bytestring(v)) : NULL;
}

static char* option_string(GVariant* options, const char* key)
{
    const char* s = NULL;
    return g_variant_lookup(options, key, "&s", &s) ? g_strdup(s) : NULL;
}

static bool option_bool(GVariant* options, const char* key)
{
    gboolean b = FALSE;
    return g_variant_lookup(options, key, "b", &b) && b;
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
    g_debug("%s from %s (%s): %s", method, app_id, parent, title);

    Dialog* d = g_new0(Dialog, 1);
    d->invocation = invocation;
    d->connection = g_object_ref(connection);
    g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(argv, g_strdup(sfe_binary()));
    g_autofree char* folder = option_bytestring(options, "current_folder");
    g_autofree char* name = NULL;

    if (strcmp(method, "OpenFile") == 0) {
        g_ptr_array_add(argv, g_strdup("--choose"));
        g_ptr_array_add(argv, g_strdup(option_bool(options, "directory") ? "folder" : "open"));
        if (option_bool(options, "multiple"))
            g_ptr_array_add(argv, g_strdup("--multiple"));
    } else if (strcmp(method, "SaveFile") == 0) {
        g_ptr_array_add(argv, g_strdup("--choose"));
        g_ptr_array_add(argv, g_strdup("save"));
        name = option_string(options, "current_name");
        g_autofree char* current = option_bytestring(options, "current_file"); /* replacing a file */
        if (current != NULL && *current != '\0') {
            g_free(folder);
            folder = g_path_get_dirname(current);
            g_free(name);
            name = g_path_get_basename(current);
        }
    } else { /* SaveFiles: a folder for several files */
        g_ptr_array_add(argv, g_strdup("--choose"));
        g_ptr_array_add(argv, g_strdup("folder"));
        d->save_names = g_ptr_array_new_with_free_func(g_free);
        g_autoptr(GVariant) files = g_variant_lookup_value(options, "files", G_VARIANT_TYPE_BYTESTRING_ARRAY);
        for (gsize i = 0; files != NULL && i < g_variant_n_children(files); i++) {
            g_autoptr(GVariant) f = g_variant_get_child_value(files, i);
            g_ptr_array_add(d->save_names, g_path_get_basename(g_variant_get_bytestring(f)));
        }
    }
    if (title != NULL && *title != '\0') {
        g_ptr_array_add(argv, g_strdup("--title"));
        g_ptr_array_add(argv, g_strdup(title));
    }
    if (folder != NULL && *folder != '\0') {
        g_ptr_array_add(argv, g_strdup("--folder"));
        g_ptr_array_add(argv, g_strdup(folder));
    }
    if (name != NULL && *name != '\0') {
        g_ptr_array_add(argv, g_strdup("--name"));
        g_ptr_array_add(argv, g_strdup(name));
    }
    if (parent != NULL && *parent != '\0') { /* "wayland:HANDLE" / "x11:XID": sfe attaches to it */
        g_ptr_array_add(argv, g_strdup("--parent"));
        g_ptr_array_add(argv, g_strdup(parent));
    }
    /* File type filters, and the one to start with. current_filter should
     * be one of the filters; alone (no list) the program wants only those
     * files, and one missing from the list is added to it. */
    g_autoptr(GVariant) asked = g_variant_lookup_value(options, "filters", G_VARIANT_TYPE("a(sa(us))"));
    g_autoptr(GVariant) current = g_variant_lookup_value(options, "current_filter", G_VARIANT_TYPE("(sa(us))"));
    GVariantBuilder list;
    g_variant_builder_init(&list, G_VARIANT_TYPE("a(sa(us))"));
    gsize n = 0;
    int start = -1;
    for (gsize i = 0; asked != NULL && i < g_variant_n_children(asked); i++, n++) {
        g_autoptr(GVariant) f = g_variant_get_child_value(asked, i);
        if (start < 0 && current != NULL && g_variant_equal(f, current))
            start = (int)i;
        g_variant_builder_add_value(&list, f);
    }
    if (current != NULL && start < 0) {
        g_variant_builder_add_value(&list, current);
        start = (int)n++;
    }
    d->filters = g_variant_ref_sink(g_variant_builder_end(&list));
    if (n > 0) {
        g_ptr_array_add(argv, g_strdup("--filters"));
        g_ptr_array_add(argv, g_variant_print(d->filters, TRUE));
        if (start >= 0) {
            g_ptr_array_add(argv, g_strdup("--current-filter"));
            g_ptr_array_add(argv, g_strdup_printf("%d", start));
        }
    } else {
        g_clear_pointer(&d->filters, g_variant_unref);
    }
    /* Extra options (check boxes, drop-downs), answered as "choices". */
    g_autoptr(GVariant) choice_list = g_variant_lookup_value(options, "choices", G_VARIANT_TYPE("a(ssa(ss)s)"));
    if (choice_list != NULL && g_variant_n_children(choice_list) > 0) {
        g_ptr_array_add(argv, g_strdup("--choices"));
        g_ptr_array_add(argv, g_variant_print(choice_list, TRUE));
    }
    g_autofree char* accept = option_string(options, "accept_label");
    if (accept != NULL && *accept != '\0') {
        g_ptr_array_add(argv, g_strdup("--accept-label"));
        g_ptr_array_add(argv, g_strdup(accept));
    }
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
    g_main_loop_run(loop);
    g_bus_unown_name(owner);
    return 0;
}
