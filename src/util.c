#include "util.h"

#include <string.h>

char* sfe_expand_home(const char* path)
{
    if (path == NULL)
        return g_strdup(g_get_home_dir());
    if (path[0] == '~' && (path[1] == '\0' || path[1] == '/'))
        return g_build_filename(g_get_home_dir(), path + 1, NULL);
    return g_strdup(path);
}

char* sfe_contract_home(const char* path)
{
    const char* home = g_get_home_dir();
    size_t n = strlen(home);

    if (strncmp(path, home, n) == 0 && (path[n] == '\0' || path[n] == '/'))
        return g_strconcat("~", path + n, NULL);
    return g_strdup(path);
}

void sfe_split_extension(const char* name, bool is_dir, char** stem, char** ext)
{
    const char* dot = is_dir ? NULL : strrchr(name, '.');

    if (dot == NULL || dot == name || dot[1] == '\0') {
        *stem = g_strdup(name);
        *ext = g_strdup("");
        return;
    }
    if (dot - name > 4 && strncmp(dot - 4, ".tar", 4) == 0)
        dot -= 4;
    *stem = g_strndup(name, dot - name);
    *ext = g_strdup(dot);
}

void sfe_on_main_thread(GMainContext* context, GSourceFunc func, gpointer data, GDestroyNotify notify)
{
    GSource* source = g_idle_source_new();
    g_source_set_priority(source, G_PRIORITY_DEFAULT);
    g_source_set_callback(source, func, data, notify);
    g_source_attach(source, context);
    g_source_unref(source);
}
