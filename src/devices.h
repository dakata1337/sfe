#pragma once

/* Drives and partitions for the sidebar.
 *
 * Talks to UDisks2 directly over the system D-Bus (no gvfs, no systemd), so
 * removable media mount without a root password: polkit allows it for the
 * active local session (elogind is enough). Internal partitions need admin
 * rights unless a polkit rule allows them.
 *
 * Without UDisks2, mounted filesystems under /run/media, /media and /mnt
 * are still listed (read from the mount table), but nothing can be mounted. */

#include <gio/gio.h>
#include <stdbool.h>

typedef struct {
    char* id; /* UDisks2 object path, or the mount path without UDisks2 */
    char* name;
    char* tooltip; /* size, filesystem, device node, mount point */
    char* icon_name; /* symbolic icon */
    char* mount_path; /* NULL when not mounted */
    bool removable; /* "eject" unmounts it and powers the drive off */
    bool system_root; /* the filesystem mounted at "/" */
    bool can_mount;
    bool can_unmount;
} SfeDevice;

#define SFE_TYPE_DEVICES (sfe_devices_get_type())
G_DECLARE_FINAL_TYPE(SfeDevices, sfe_devices, SFE, DEVICES, GObject)

/* The shared instance. Emits "changed" whenever the list changes. */
SfeDevices* sfe_devices_get_default(void);

/* Current devices (SfeDevice*, owned by `self`, sorted by name). */
GPtrArray* sfe_devices_get_list(SfeDevices* self);

/* Mounts the device; the result is the mount path (free with g_free). */
void sfe_devices_mount(SfeDevices* self, const char* id, GCancellable* cancel,
    GAsyncReadyCallback callback, gpointer data);
char* sfe_devices_mount_finish(SfeDevices* self, GAsyncResult* res, GError** error);

/* Unmounts the device. With `eject`, every filesystem of its drive is
 * unmounted and the drive is powered off / ejected so it can be unplugged. */
void sfe_devices_unmount(SfeDevices* self, const char* id, bool eject, GCancellable* cancel,
    GAsyncReadyCallback callback, gpointer data);
bool sfe_devices_unmount_finish(SfeDevices* self, GAsyncResult* res, GError** error);
