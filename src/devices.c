#include "devices.h"

#include <gio/gunixmounts.h>
#include <string.h>

#define UDISKS_NAME "org.freedesktop.UDisks2"
#define UDISKS_PATH "/org/freedesktop/UDisks2"
#define IFACE_BLOCK UDISKS_NAME ".Block"
#define IFACE_FS UDISKS_NAME ".Filesystem"
#define IFACE_DRIVE UDISKS_NAME ".Drive"
#define IFACE_PARTITION UDISKS_NAME ".Partition"

/* Unmounted internal partitions smaller than this (EFI, recovery, ...) are
 * not worth listing. */
#define MIN_INTERNAL_SIZE (G_GUINT64_CONSTANT(1) << 30)

/* Mounting can wait for a polkit password prompt: no D-Bus timeout. */
#define CALL_TIMEOUT G_MAXINT

#if GLIB_CHECK_VERSION(2, 84, 0)
#define unix_mounts_get g_unix_mount_entries_get
#define unix_mount_get_path g_unix_mount_entry_get_mount_path
#define unix_mount_free g_unix_mount_entry_free
#else
#define unix_mounts_get g_unix_mounts_get
#define unix_mount_get_path g_unix_mount_get_mount_path
#define unix_mount_free g_unix_mount_free
#endif

struct _SfeDevices {
    GObject parent;

    GDBusObjectManager* udisks; /* NULL until connected, or without UDisks2 */
    GUnixMountMonitor* mount_monitor;
    GPtrArray* devices; /* SfeDevice* */
    guint rebuild_source;
};

G_DEFINE_TYPE(SfeDevices, sfe_devices, G_TYPE_OBJECT)

enum {
    SIGNAL_CHANGED,
    N_SIGNALS,
};
static guint signals[N_SIGNALS];

static void device_free(gpointer p)
{
    SfeDevice* d = p;
    g_free(d->id);
    g_free(d->name);
    g_free(d->tooltip);
    g_free(d->icon_name);
    g_free(d->mount_path);
    g_free(d);
}

/* Mount points the user cares about, as opposed to /, /boot, /var/... */
static bool is_user_mount(const char* path)
{
    const char* home = g_get_home_dir();
    size_t n = strlen(home);

    return g_str_has_prefix(path, "/run/media/") || g_str_has_prefix(path, "/media/")
        || g_str_has_prefix(path, "/mnt/")
        || (strncmp(path, home, n) == 0 && path[n] == '/');
}

static int compare_devices(gconstpointer a, gconstpointer b)
{
    const SfeDevice* da = *(SfeDevice* const*)a;
    const SfeDevice* db = *(SfeDevice* const*)b;
    return g_utf8_collate(da->name, db->name);
}

/* ------------------------------------------------------------------------ */
/* UDisks2 property helpers                                                  */
/* ------------------------------------------------------------------------ */

static GDBusProxy* get_proxy(GDBusObject* object, const char* iface)
{
    return object ? G_DBUS_PROXY(g_dbus_object_get_interface(object, iface)) : NULL;
}

static GVariant* get_prop(GDBusObject* object, const char* iface, const char* name)
{
    g_autoptr(GDBusProxy) proxy = get_proxy(object, iface);
    return proxy ? g_dbus_proxy_get_cached_property(proxy, name) : NULL;
}

static char* prop_string(GDBusObject* object, const char* iface, const char* name)
{
    g_autoptr(GVariant) v = get_prop(object, iface, name);
    if (v == NULL || !g_variant_is_of_type(v, G_VARIANT_TYPE_STRING))
        return NULL;
    const char* s = g_variant_get_string(v, NULL);
    return *s ? g_strdup(s) : NULL;
}

static bool prop_bool(GDBusObject* object, const char* iface, const char* name)
{
    g_autoptr(GVariant) v = get_prop(object, iface, name);
    return v != NULL && g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN) && g_variant_get_boolean(v);
}

static guint64 prop_uint64(GDBusObject* object, const char* iface, const char* name)
{
    g_autoptr(GVariant) v = get_prop(object, iface, name);
    return v != NULL && g_variant_is_of_type(v, G_VARIANT_TYPE_UINT64) ? g_variant_get_uint64(v) : 0;
}

/* The mount point to show from Filesystem.MountPoints (aay): one the user
 * cares about (see is_user_mount), else "/" for the system disk (`root`);
 * NULL when not mounted. `system_only` says it is mounted only elsewhere
 * (/boot, /var/lib/docker ...). */
static bool is_user_mount(const char* path);

/* The first entry of Filesystem.MountPoints (aay), or NULL. */
static char* first_mount_point(GDBusObject* object)
{
    g_autoptr(GVariant) v = get_prop(object, IFACE_FS, "MountPoints");
    if (v == NULL || g_variant_n_children(v) == 0)
        return NULL;
    g_autoptr(GVariant) first = g_variant_get_child_value(v, 0);
    return g_strdup(g_variant_get_bytestring(first));
}

static char* pick_mount_point(GDBusObject* object, bool* root, bool* system_only)
{
    g_autoptr(GVariant) v = get_prop(object, IFACE_FS, "MountPoints");
    *root = *system_only = false;
    if (v == NULL || g_variant_n_children(v) == 0)
        return NULL;
    for (gsize i = 0; i < g_variant_n_children(v); i++) {
        g_autoptr(GVariant) child = g_variant_get_child_value(v, i);
        const char* path = g_variant_get_bytestring(child);
        if (is_user_mount(path))
            return g_strdup(path);
        if (strcmp(path, "/") == 0)
            *root = true;
    }
    if (*root)
        return g_strdup("/");
    *system_only = true;
    return NULL;
}

/* The Drive object a block device belongs to, or NULL. */
static GDBusObject* get_drive(SfeDevices* self, GDBusObject* block)
{
    g_autoptr(GVariant) v = get_prop(block, IFACE_BLOCK, "Drive");
    const char* path = v ? g_variant_get_string(v, NULL) : "/";
    return strcmp(path, "/") != 0 ? g_dbus_object_manager_get_object(self->udisks, path) : NULL;
}

static bool drive_is_removable(GDBusObject* drive)
{
    return drive != NULL
        && (prop_bool(drive, IFACE_DRIVE, "Removable") || prop_bool(drive, IFACE_DRIVE, "MediaRemovable")
            || prop_bool(drive, IFACE_DRIVE, "Ejectable") || prop_bool(drive, IFACE_DRIVE, "CanPowerOff"));
}

/* ------------------------------------------------------------------------ */
/* Building the list                                                         */
/* ------------------------------------------------------------------------ */

/* How many mounted-or-mountable filesystems live on `drive`. */
static int filesystems_on_drive(SfeDevices* self, GDBusObject* drive)
{
    const char* drive_path = g_dbus_object_get_object_path(drive);
    GList* objects = g_dbus_object_manager_get_objects(self->udisks);
    int n = 0;

    for (GList* l = objects; l != NULL; l = l->next) {
        g_autoptr(GDBusProxy) fs = get_proxy(l->data, IFACE_FS);
        g_autoptr(GVariant) v = get_prop(l->data, IFACE_BLOCK, "Drive");
        if (fs != NULL && v != NULL && !prop_bool(l->data, IFACE_BLOCK, "HintIgnore")
            && strcmp(g_variant_get_string(v, NULL), drive_path) == 0)
            n++;
    }
    g_list_free_full(objects, g_object_unref);
    return n;
}

/* A name people recognise: the partition's label, else the drive's vendor
 * and model ("Kingston DataTraveler 3.0", plus the partition number when
 * the drive holds several filesystems), else the size. */
static char* device_name(SfeDevices* self, GDBusObject* object, GDBusObject* drive, guint64 size)
{
    char* name = prop_string(object, IFACE_BLOCK, "HintName");
    if (name == NULL)
        name = prop_string(object, IFACE_BLOCK, "IdLabel");
    if (name != NULL)
        return name;

    g_autofree char* vendor = drive ? prop_string(drive, IFACE_DRIVE, "Vendor") : NULL;
    g_autofree char* model = drive ? prop_string(drive, IFACE_DRIVE, "Model") : NULL;
    if (model != NULL) {
        g_autofree char* base = vendor ? g_strdup_printf("%s %s", vendor, model) : g_strdup(model);
        g_strstrip(base);
        g_autoptr(GVariant) number = get_prop(object, IFACE_PARTITION, "Number");
        if (number != NULL && filesystems_on_drive(self, drive) > 1)
            return g_strdup_printf("%s (%u)", base, g_variant_get_uint32(number));
        return g_steal_pointer(&base);
    }
    g_autofree char* sz = g_format_size(size);
    return g_strdup_printf("%s Volume", sz);
}

static char* device_tooltip(GDBusObject* object, guint64 size, const char* mount)
{
    g_autofree char* sz = g_format_size(size);
    g_autofree char* type = prop_string(object, IFACE_BLOCK, "IdType");
    g_autoptr(GVariant) dev = get_prop(object, IFACE_BLOCK, "PreferredDevice");
    const char* node = dev ? g_variant_get_bytestring(dev) : "";
    return g_strdup_printf("%s %s · %s%s%s", sz, type ? type : "", node,
        mount ? " · " : "", mount ? mount : "");
}

/* Returns NULL for block devices that should not be shown. */
static SfeDevice* device_from_udisks(SfeDevices* self, GDBusObject* object)
{
    g_autoptr(GDBusProxy) fs = get_proxy(object, IFACE_FS);
    if (fs == NULL || prop_bool(object, IFACE_BLOCK, "HintIgnore"))
        return NULL;

    bool root = false, system_only = false;
    g_autofree char* mount = pick_mount_point(object, &root, &system_only);
    g_autoptr(GDBusObject) drive = get_drive(self, object);
    bool removable = drive_is_removable(drive);
    bool system = prop_bool(object, IFACE_BLOCK, "HintSystem");
    guint64 size = prop_uint64(object, IFACE_BLOCK, "Size");

    if (system_only)
        return NULL; /* /boot, /var/...: not for the sidebar */
    if (mount == NULL && system && size < MIN_INTERNAL_SIZE)
        return NULL;

    SfeDevice* d = g_new0(SfeDevice, 1);
    d->id = g_strdup(g_dbus_object_get_object_path(object));
    d->name = device_name(self, object, drive, size);
    g_autofree char* details = device_tooltip(object, size, mount);
    d->tooltip = g_strdup_printf("%s\n%s", d->name, details); /* full name: the row may cut it off */
    d->icon_name = prop_string(object, IFACE_BLOCK, "HintSymbolicIconName");
    if (d->icon_name == NULL)
        d->icon_name = g_strdup(removable ? "drive-removable-media-symbolic" : "drive-harddisk-symbolic");
    d->mount_path = g_steal_pointer(&mount);
    d->removable = removable;
    d->system_root = root;
    d->can_mount = d->mount_path == NULL;
    d->can_unmount = d->mount_path != NULL && !root; /* the system cannot be unmounted */
    return d;
}

static void add_udisks_devices(SfeDevices* self)
{
    GList* objects = g_dbus_object_manager_get_objects(self->udisks);
    for (GList* l = objects; l != NULL; l = l->next) {
        SfeDevice* d = device_from_udisks(self, l->data);
        if (d != NULL)
            g_ptr_array_add(self->devices, d);
    }
    g_list_free_full(objects, g_object_unref);
}

/* Without UDisks2: list what is mounted in the usual removable locations. */
static void add_mounted_filesystems(SfeDevices* self)
{
    GList* mounts = unix_mounts_get(NULL);
    for (GList* l = mounts; l != NULL; l = l->next) {
        const char* path = unix_mount_get_path(l->data);
        if (is_user_mount(path)) {
            SfeDevice* d = g_new0(SfeDevice, 1);
            d->id = g_strdup(path);
            d->name = g_path_get_basename(path);
            d->icon_name = g_strdup("drive-harddisk-symbolic");
            d->mount_path = g_strdup(path);
            g_ptr_array_add(self->devices, d);
        }
    }
    g_list_free_full(mounts, (GDestroyNotify)unix_mount_free);
}

static bool udisks_available(SfeDevices* self)
{
    if (self->udisks == NULL)
        return false;
    g_autofree char* owner = g_dbus_object_manager_client_get_name_owner(
        G_DBUS_OBJECT_MANAGER_CLIENT(self->udisks));
    return owner != NULL;
}

static gboolean rebuild(gpointer data)
{
    SfeDevices* self = data;

    self->rebuild_source = 0;
    g_ptr_array_set_size(self->devices, 0);
    if (udisks_available(self))
        add_udisks_devices(self);
    else
        add_mounted_filesystems(self);
    g_ptr_array_sort(self->devices, compare_devices);
    g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
    return G_SOURCE_REMOVE;
}

/* UDisks2 sends bursts of signals (e.g. per partition on plug-in). */
static void schedule_rebuild(SfeDevices* self)
{
    if (self->rebuild_source == 0)
        self->rebuild_source = g_idle_add(rebuild, self);
}

static void on_udisks_changed(SfeDevices* self)
{
    schedule_rebuild(self);
}

static void on_udisks_ready(GObject* source, GAsyncResult* res, gpointer data)
{
    (void)source;
    g_autoptr(SfeDevices) self = data;
    g_autoptr(GError) error = NULL;

    self->udisks = g_dbus_object_manager_client_new_for_bus_finish(res, &error);
    if (self->udisks == NULL) {
        g_debug("UDisks2 unavailable: %s", error->message);
    } else {
        const char* sigs[] = { "object-added", "object-removed", "interface-added", "interface-removed",
            "interface-proxy-properties-changed", "notify::name-owner" };
        for (size_t i = 0; i < G_N_ELEMENTS(sigs); i++)
            g_signal_connect_swapped(self->udisks, sigs[i], G_CALLBACK(on_udisks_changed), self);
    }
    schedule_rebuild(self);
}

static void on_mounts_changed(GUnixMountMonitor* monitor, gpointer data)
{
    (void)monitor;
    SfeDevices* self = data;
    if (!udisks_available(self))
        schedule_rebuild(self);
}

/* ------------------------------------------------------------------------ */
/* Mount / unmount / eject                                                   */
/* ------------------------------------------------------------------------ */

static void return_dbus_error(GTask* task, GError* error)
{
    g_debug("udisks error: %s", error->message);
    g_dbus_error_strip_remote_error(error);
    g_task_return_error(task, error);
    g_object_unref(task);
}

static GDBusProxy* filesystem_for_id(SfeDevices* self, const char* id, GTask* task)
{
    g_autoptr(GDBusObject) object = udisks_available(self)
        ? g_dbus_object_manager_get_object(self->udisks, id)
        : NULL;
    GDBusProxy* fs = get_proxy(object, IFACE_FS);

    if (fs == NULL) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
            "This device can only be managed when udisks2 is installed and running");
        g_object_unref(task);
    }
    return fs;
}

/* A mount in progress (the GTask's data). */
typedef struct {
    GDBusProxy* fs;
    bool ntfs; /* an NTFS filesystem: the kernel driver may refuse it */
    bool retried; /* already asked for ntfs-3g */
} MountJob;

static void mount_job_free(gpointer p)
{
    MountJob* job = p;
    g_object_unref(job->fs);
    g_free(job);
}

/* udisks mounts NTFS with the kernel's ntfs3 driver first, which refuses a
 * volume Windows did not close cleanly (Fast Startup, hibernation), with a
 * bare "wrong fs type, bad option, bad superblock" error. ntfs-3g mounts
 * those: asking for the fstype "ntfs" makes udisks use it (mount.ntfs). */
static bool ntfs_3g_installed(void)
{
    g_autofree char* helper = g_find_program_in_path("mount.ntfs");
    g_autofree char* fallback = helper ? NULL : g_find_program_in_path("ntfs-3g");
    return helper != NULL || fallback != NULL;
}

static void call_mount(GTask* task, const char* fstype);

static void on_mount_done(GObject* source, GAsyncResult* res, gpointer data)
{
    GTask* task = data;
    MountJob* job = g_task_get_task_data(task);
    GError* error = NULL;
    g_autoptr(GVariant) ret = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), res, &error);

    if (ret == NULL) {
        /* Refused, not denied or cancelled: try ntfs-3g once. */
        bool failed = g_dbus_error_is_remote_error(error)
            && g_strcmp0(g_dbus_error_get_remote_error(error), "org.freedesktop.UDisks2.Error.Failed") == 0;
        if (job->ntfs && !job->retried && failed) {
            if (ntfs_3g_installed()) {
                g_debug("ntfs3 refused the volume (%s); trying ntfs-3g", error->message);
                g_error_free(error);
                job->retried = true;
                call_mount(task, "ntfs");
                return;
            }
            g_dbus_error_strip_remote_error(error);
            g_prefix_error(&error, "The kernel's NTFS driver refused this volume; Windows probably did not close it "
                                   "cleanly (Fast Startup or hibernation). Installing ntfs-3g lets sfe mount it. ");
        }
        return_dbus_error(task, error);
        return;
    }
    char* path = NULL;
    g_variant_get(ret, "(s)", &path);
    g_task_return_pointer(task, path, g_free);
    g_object_unref(task);
}

/* Filesystem.Mount, with udisks' default driver or `fstype`. */
static void call_mount(GTask* task, const char* fstype)
{
    MountJob* job = g_task_get_task_data(task);
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    if (fstype != NULL)
        g_variant_builder_add(&options, "{sv}", "fstype", g_variant_new_string(fstype));
    g_dbus_proxy_call(job->fs, "Mount", g_variant_new("(a{sv})", &options), G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION,
        CALL_TIMEOUT, g_task_get_cancellable(task), on_mount_done, task);
}

void sfe_devices_mount(SfeDevices* self, const char* id, GCancellable* cancel,
    GAsyncReadyCallback callback, gpointer data)
{
    GTask* task = g_task_new(self, cancel, callback, data);
    GDBusProxy* fs = filesystem_for_id(self, id, task);

    g_debug("mount %s", id);
    if (fs == NULL)
        return;
    g_autoptr(GDBusObject) object = g_dbus_object_manager_get_object(self->udisks, id);
    g_autofree char* type = prop_string(object, IFACE_BLOCK, "IdType");
    MountJob* job = g_new0(MountJob, 1);
    job->fs = fs;
    job->ntfs = g_strcmp0(type, "ntfs") == 0;
    g_task_set_task_data(task, job, mount_job_free);
    call_mount(task, NULL);
}

char* sfe_devices_mount_finish(SfeDevices* self, GAsyncResult* res, GError** error)
{
    g_return_val_if_fail(g_task_is_valid(res, self), NULL);
    return g_task_propagate_pointer(G_TASK(res), error);
}

/* An unmount, then optionally more unmounts and a power-off for eject. */
typedef struct {
    GQueue filesystems; /* GDBusProxy*, still to unmount */
    GDBusProxy* drive; /* power off / eject at the end, or NULL */
} UnmountJob;

static void unmount_job_free(gpointer p)
{
    UnmountJob* job = p;
    g_queue_clear_full(&job->filesystems, g_object_unref);
    g_clear_object(&job->drive);
    g_free(job);
}

static void unmount_next(GTask* task);

static void on_step_done(GObject* source, GAsyncResult* res, gpointer data)
{
    GTask* task = data;
    GError* error = NULL;
    g_autoptr(GVariant) ret = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), res, &error);

    if (ret == NULL)
        return_dbus_error(task, error);
    else
        unmount_next(task);
}

static void unmount_next(GTask* task)
{
    UnmountJob* job = g_task_get_task_data(task);
    GCancellable* cancel = g_task_get_cancellable(task);
    g_autoptr(GDBusProxy) fs = g_queue_pop_head(&job->filesystems);

    if (fs != NULL) {
        g_dbus_proxy_call(fs, "Unmount", g_variant_new("(a{sv})", NULL),
            G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, CALL_TIMEOUT, cancel, on_step_done, task);
        return;
    }
    if (job->drive != NULL) {
        g_autoptr(GDBusProxy) drive = g_steal_pointer(&job->drive);
        g_autoptr(GVariant) can_power_off = g_dbus_proxy_get_cached_property(drive, "CanPowerOff");
        const char* method = can_power_off && g_variant_get_boolean(can_power_off) ? "PowerOff" : "Eject";
        g_dbus_proxy_call(drive, method, g_variant_new("(a{sv})", NULL),
            G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, CALL_TIMEOUT, cancel, on_step_done, task);
        return;
    }
    g_task_return_boolean(task, TRUE);
    g_object_unref(task);
}

/* Queues every mounted filesystem on `drive`, starting with `first`. */
static void queue_drive_filesystems(SfeDevices* self, UnmountJob* job, GDBusObject* drive, GDBusProxy* first)
{
    const char* drive_path = g_dbus_object_get_object_path(drive);
    GList* objects = g_dbus_object_manager_get_objects(self->udisks);

    for (GList* l = objects; l != NULL; l = l->next) {
        g_autoptr(GVariant) v = get_prop(l->data, IFACE_BLOCK, "Drive");
        g_autofree char* mount = first_mount_point(l->data);
        GDBusProxy* fs = get_proxy(l->data, IFACE_FS);

        if (fs != NULL && fs != first && mount != NULL && v != NULL
            && strcmp(g_variant_get_string(v, NULL), drive_path) == 0)
            g_queue_push_tail(&job->filesystems, fs);
        else if (fs != NULL)
            g_object_unref(fs);
    }
    g_list_free_full(objects, g_object_unref);
}

void sfe_devices_unmount(SfeDevices* self, const char* id, bool eject, GCancellable* cancel,
    GAsyncReadyCallback callback, gpointer data)
{
    GTask* task = g_task_new(self, cancel, callback, data);
    GDBusProxy* fs = filesystem_for_id(self, id, task);

    g_debug("%s %s", eject ? "eject" : "unmount", id);
    if (fs == NULL)
        return;

    UnmountJob* job = g_new0(UnmountJob, 1);
    g_task_set_task_data(task, job, unmount_job_free);

    g_autofree char* mount = first_mount_point(g_dbus_interface_get_object(G_DBUS_INTERFACE(fs)));
    if (mount != NULL)
        g_queue_push_tail(&job->filesystems, g_object_ref(fs));

    if (eject) {
        g_autoptr(GDBusObject) object = g_dbus_object_manager_get_object(self->udisks, id);
        g_autoptr(GDBusObject) drive = get_drive(self, object);
        if (drive != NULL) {
            queue_drive_filesystems(self, job, drive, fs);
            job->drive = get_proxy(drive, IFACE_DRIVE);
        }
    }
    g_object_unref(fs);
    unmount_next(task);
}

bool sfe_devices_unmount_finish(SfeDevices* self, GAsyncResult* res, GError** error)
{
    g_return_val_if_fail(g_task_is_valid(res, self), false);
    return g_task_propagate_boolean(G_TASK(res), error);
}

/* ------------------------------------------------------------------------ */
/* GObject                                                                   */
/* ------------------------------------------------------------------------ */

GPtrArray* sfe_devices_get_list(SfeDevices* self)
{
    return self->devices;
}

static void sfe_devices_dispose(GObject* object)
{
    SfeDevices* self = SFE_DEVICES(object);

    g_clear_handle_id(&self->rebuild_source, g_source_remove);
    if (self->udisks != NULL)
        g_signal_handlers_disconnect_by_data(self->udisks, self);
    g_clear_object(&self->udisks);
    if (self->mount_monitor != NULL)
        g_signal_handlers_disconnect_by_data(self->mount_monitor, self);
    g_clear_object(&self->mount_monitor);
    g_clear_pointer(&self->devices, g_ptr_array_unref);
    G_OBJECT_CLASS(sfe_devices_parent_class)->dispose(object);
}

static void sfe_devices_class_init(SfeDevicesClass* klass)
{
    G_OBJECT_CLASS(klass)->dispose = sfe_devices_dispose;
    signals[SIGNAL_CHANGED] = g_signal_new("changed", G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void sfe_devices_init(SfeDevices* self)
{
    self->devices = g_ptr_array_new_with_free_func(device_free);
    self->mount_monitor = g_unix_mount_monitor_get();
    g_signal_connect(self->mount_monitor, "mounts-changed", G_CALLBACK(on_mounts_changed), self);

    g_dbus_object_manager_client_new_for_bus(G_BUS_TYPE_SYSTEM, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
        UDISKS_NAME, UDISKS_PATH, NULL, NULL, NULL, NULL, on_udisks_ready, g_object_ref(self));
}

SfeDevices* sfe_devices_get_default(void)
{
    static SfeDevices* instance;
    if (instance == NULL)
        instance = g_object_new(SFE_TYPE_DEVICES, NULL);
    return instance;
}
