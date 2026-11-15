#include "app.h"

int main(int argc, char** argv)
{
    /* Enable GTK debug messages for debug builds */
#if defined(DEBUG)
    g_setenv("G_MESSAGES_DEBUG", "sfe", true);
#endif

    /* Wayland first; X11 only as a fallback (e.g. when run under XWayland). */
    gdk_set_allowed_backends("wayland,x11");

    g_autoptr(SfeApplication) app = sfe_application_new();
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    return sfe_application_get_exit_status(app, status); /* chooser mode: chosen or cancelled */
}
