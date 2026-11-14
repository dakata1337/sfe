#include "defaultfm.h"

#include "dialogs.h"
#include <gio/gdesktopappinfo.h>
#include <string.h>

#define DESKTOP_ID "sfe.desktop"
#define FOLDER_TYPE "inode/directory"

bool sfe_default_fm_available(void)
{
    g_autoptr(GDesktopAppInfo) app = g_desktop_app_info_new(DESKTOP_ID);
    return app != NULL;
}

bool sfe_default_fm_is_set(void)
{
    g_autoptr(GAppInfo) app = g_app_info_get_default_for_type(FOLDER_TYPE, FALSE);
    return app != NULL && g_strcmp0(g_app_info_get_id(app), DESKTOP_ID) == 0;
}

bool sfe_default_fm_set(GError** error)
{
    g_autoptr(GDesktopAppInfo) app = g_desktop_app_info_new(DESKTOP_ID);
    if (app == NULL) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
            "sfe.desktop is not installed (install sfe with meson install or the package).");
        return false;
    }
    return g_app_info_set_as_default_for_type(G_APP_INFO(app), FOLDER_TYPE, error);
}

/* ------------------------------------------------------------------------ */
/* Asking once                                                               */
/* ------------------------------------------------------------------------ */

static char* asked_path(void)
{
    return g_build_filename(g_get_user_state_dir(), "sfe", "asked-default-file-manager", NULL);
}

static void remember_asked(void)
{
    g_autofree char* path = asked_path();
    g_autofree char* dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    g_file_set_contents(path, "", 0, NULL);
}

static void on_make_default(gpointer data)
{
    g_autoptr(GError) error = NULL;
    if (!sfe_default_fm_set(&error))
        sfe_show_error(GTK_WIDGET(data), "Cannot make sfe the default file manager", error->message);
}

void sfe_default_fm_maybe_ask(GtkWindow* parent)
{
    g_autofree char* path = asked_path();

    if (g_file_test(path, G_FILE_TEST_EXISTS) || !sfe_default_fm_available() || sfe_default_fm_is_set())
        return;
    remember_asked(); /* ask only once, whatever the answer */
    sfe_confirm(GTK_WIDGET(parent), "Make sfe your default file manager?",
        "Folders opened from other applications will open in sfe. You can change this later in Preferences.",
        "Make Default", false, on_make_default, parent, NULL);
}
