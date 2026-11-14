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
