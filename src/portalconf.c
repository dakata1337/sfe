#include "portalconf.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define GROUP "preferred"
#define KEY "org.freedesktop.impl.portal.FileChooser"
#define BACKEND "sfe"

char* sfe_portal_desktop(void)
{
    const char* desktops = g_getenv("XDG_CURRENT_DESKTOP");
    if (desktops == NULL || *desktops == '\0')
        return NULL;
    g_auto(GStrv) parts = g_strsplit(desktops, ":", 2);
    return parts[0] && *parts[0] ? g_ascii_strdown(parts[0], -1) : NULL;
}

static char* conf_name(const char* desktop)
{
    return desktop ? g_strconcat(desktop, "-portals.conf", NULL) : g_strdup("portals.conf");
}

char* sfe_portal_conf_path(const char* desktop)
{
    g_autofree char* name = conf_name(desktop);
    return g_build_filename(g_get_user_config_dir(), "xdg-desktop-portal", name, NULL);
}

bool sfe_portal_conf_uses_sfe(const char* path)
{
    g_autoptr(GKeyFile) kf = g_key_file_new();
    if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL))
        return false;
    g_autofree char* value = g_key_file_get_string(kf, GROUP, KEY, NULL);
    return value != NULL && strcmp(g_strstrip(value), BACKEND) == 0;
}

/* `name` in the system data dirs' xdg-desktop-portal folders, or NULL. */
static char* system_file(const char* name)
{
    const char* const* dirs = g_get_system_data_dirs();
    for (int i = 0; dirs[i] != NULL; i++) {
        char* path = g_build_filename(dirs[i], "xdg-desktop-portal", name, NULL);
        if (g_file_test(path, G_FILE_TEST_IS_REGULAR))
            return path;
        g_free(path);
    }
    return NULL;
}

char* sfe_portal_conf_seed(const char* desktop)
{
    g_autofree char* user = sfe_portal_conf_path(NULL);
    if (g_file_test(user, G_FILE_TEST_IS_REGULAR))
        return g_steal_pointer(&user);
    g_autofree char* name = conf_name(desktop);
    char* path = desktop ? system_file(name) : NULL;
    return path ? path : system_file("portals.conf");
}

bool sfe_portal_conf_set(const char* path, const char* seed, bool use_sfe, GError** error)
{
    g_autoptr(GKeyFile) kf = g_key_file_new();
    GKeyFileFlags flags = G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS;
    bool exists = g_file_test(path, G_FILE_TEST_EXISTS);

    if (exists && !g_key_file_load_from_file(kf, path, flags, error))
        return false;
    if (!exists) {
        if (!use_sfe)
            return true; /* nothing to take out */
        g_autoptr(GKeyFile) base = g_key_file_new();
        if (seed != NULL && g_key_file_load_from_file(base, seed, G_KEY_FILE_NONE, NULL)) {
            g_auto(GStrv) keys = g_key_file_get_keys(base, GROUP, NULL, NULL);
            for (int i = 0; keys != NULL && keys[i] != NULL; i++) {
                if (strcmp(keys[i], "Encoding") == 0)
                    continue; /* GKeyFile's own; in the first group (ours here) it breaks loading */
                g_autofree char* value = g_key_file_get_value(base, GROUP, keys[i], NULL);
                g_key_file_set_value(kf, GROUP, keys[i], value);
            }
        }
        g_key_file_set_comment(kf, NULL, NULL, " Written by sfe (Settings → System → File dialogs).", NULL);
    }
    if (use_sfe)
        g_key_file_set_string(kf, GROUP, KEY, BACKEND);
    else
        g_key_file_remove_key(kf, GROUP, KEY, NULL); /* (fine if absent) */

    g_autofree char* dir = g_path_get_dirname(path);
    if (g_mkdir_with_parents(dir, 0755) != 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "Cannot create %s", dir);
        return false;
    }
    /* Never write a file that does not load again: xdg-desktop-portal reads
     * it with the same parser, and file dialogs would stop following it. */
    gsize len = 0;
    g_autofree char* text = g_key_file_to_data(kf, &len, NULL);
    g_autoptr(GKeyFile) check = g_key_file_new();
    if (!g_key_file_load_from_data(check, text, len, G_KEY_FILE_NONE, error))
        return false;
    return g_file_set_contents(path, text, (gssize)len, error);
}

bool sfe_portal_installed(void)
{
    g_autofree char* portal = system_file("portals" G_DIR_SEPARATOR_S "sfe.portal");
    return portal != NULL;
}

int sfe_portal_restart(void)
{
    int stopped = 0;
    g_autoptr(GDir) proc = g_dir_open("/proc", 0, NULL);
    const char* entry;
    while (proc != NULL && (entry = g_dir_read_name(proc)) != NULL) {
        char* end = NULL;
        long pid = strtol(entry, &end, 10);
        if (end == entry || *end != '\0')
            continue;
        g_autofree char* dir = g_strdup_printf("/proc/%ld", pid);
        g_autofree char* exe_link = g_strconcat(dir, "/exe", NULL);
        g_autofree char* exe = g_file_read_link(exe_link, NULL); /* readable for our own processes only */
        g_autofree char* base = exe ? g_path_get_basename(exe) : NULL;
        GStatBuf owner;
        bool portal = base != NULL && (strcmp(base, "xdg-desktop-portal") == 0 || strcmp(base, "sfe-portal") == 0);
        if (portal && g_stat(dir, &owner) == 0
            && owner.st_uid == getuid() && kill((pid_t)pid, SIGTERM) == 0)
            stopped++;
    }
    return stopped;
}
