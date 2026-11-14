#include "opener.h"

#include <string.h>

const char* sfe_opener_lookup(GHashTable* open_with, const char* filename)
{
    g_autofree char* lower = g_utf8_strdown(filename, -1);

    /* Start after the first character so ".bashrc" has no extension. */
    for (const char* dot = strchr(lower + (lower[0] != '\0'), '.'); dot != NULL; dot = strchr(dot + 1, '.')) {
        const char* command = g_hash_table_lookup(open_with, dot + 1);
        if (command != NULL)
            return command;
    }
    return NULL;
}

static char* replace_all(const char* str, const char* find, const char* with)
{
    g_auto(GStrv) parts = g_strsplit(str, find, -1);
    return g_strjoinv(with, parts);
}

char** sfe_opener_build_argv(const char* command, const char* path, GError** error)
{
    char** argv = NULL;
    bool substituted = false;

    if (!g_shell_parse_argv(command, NULL, &argv, error))
        return NULL;

    for (char** arg = argv; *arg != NULL; arg++) {
        if (strstr(*arg, "%f") != NULL) {
            char* expanded = replace_all(*arg, "%f", path);
            g_free(*arg);
            *arg = expanded;
            substituted = true;
        }
    }
    if (!substituted) {
        guint n = g_strv_length(argv);
        argv = g_renew(char*, argv, n + 2);
        argv[n] = g_strdup(path);
        argv[n + 1] = NULL;
    }
    return argv;
}

static bool spawn(char** argv, GFile* file, GError** error)
{
    g_autoptr(GFile) parent = g_file_get_parent(file);
    g_autofree char* cwd = parent ? g_file_get_path(parent) : NULL;

    return g_spawn_async(cwd, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, error);
}

bool sfe_open_file(const SfeConfig* cfg, GFile* file, GError** error)
{
    g_autofree char* basename = g_file_get_basename(file);
    g_autofree char* path = g_file_get_path(file);
    g_autofree char* uri = g_file_get_uri(file);
    const char* target = path ? path : uri;
    const char* command = sfe_opener_lookup(cfg->open_with, basename);

    if (command != NULL) {
        g_auto(GStrv) argv = sfe_opener_build_argv(command, target, error);
        return argv != NULL && spawn(argv, file, error);
    }

    g_autofree char* xdg_open = g_find_program_in_path("xdg-open");
    if (xdg_open != NULL) {
        char* argv[] = { xdg_open, (char*)target, NULL };
        return spawn(argv, file, error);
    }
    return g_app_info_launch_default_for_uri(uri, NULL, error);
}

static char* local_path(GFile* file)
{
    char* path = g_file_get_path(file);
    return path ? path : g_file_get_uri(file);
}

char** sfe_build_user_command(const char* command, GList* files, GFile* dir, GError** error)
{
    g_auto(GStrv) template = NULL;
    if (!g_shell_parse_argv(command, NULL, &template, error))
        return NULL;

    g_autofree char* dir_path = dir ? local_path(dir) : g_strdup("");
    g_autofree char* first = files ? local_path(files->data) : g_strdup("");
    GPtrArray* argv = g_ptr_array_new();

    for (char** arg = template; *arg != NULL; arg++) {
        if (strcmp(*arg, "%f") == 0) {
            for (GList* l = files; l != NULL; l = l->next)
                g_ptr_array_add(argv, local_path(l->data));
            continue;
        }
        g_autofree char* with_dir = replace_all(*arg, "%d", dir_path);
        g_ptr_array_add(argv, replace_all(with_dir, "%f", first));
    }
    g_ptr_array_add(argv, NULL);
    return (char**)g_ptr_array_free(argv, FALSE);
}

bool sfe_run_user_command(const char* command, GList* files, GFile* dir, GError** error)
{
    g_auto(GStrv) argv = sfe_build_user_command(command, files, dir, error);
    g_autofree char* cwd = dir ? g_file_get_path(dir) : NULL;

    if (argv == NULL)
        return false;
    if (argv[0] == NULL) {
        g_set_error(error, G_SHELL_ERROR, G_SHELL_ERROR_EMPTY_STRING, "The command is empty");
        return false;
    }
    return g_spawn_async(cwd, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, error);
}
