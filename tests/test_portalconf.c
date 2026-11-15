#include "portalconf.h"

#include <glib/gstdio.h>
#include <string.h>

static char* root; /* temporary XDG_CONFIG_HOME */

static char* write_file(const char* name, const char* text)
{
    char* path = g_build_filename(root, name, NULL);
    g_autofree char* dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_assert_true(g_file_set_contents(path, text, -1, NULL));
    return path;
}

static char* read_file(const char* path)
{
    char* text = NULL;
    g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
    return text;
}

static void test_desktop(void)
{
    g_setenv("XDG_CURRENT_DESKTOP", "Hyprland:wlroots", TRUE);
    g_autofree char* d = sfe_portal_desktop();
    g_assert_cmpstr(d, ==, "hyprland");
    g_autofree char* path = sfe_portal_conf_path(d);
    g_assert_true(g_str_has_suffix(path, "/xdg-desktop-portal/hyprland-portals.conf"));
    g_unsetenv("XDG_CURRENT_DESKTOP");
    g_autofree char* none = sfe_portal_desktop();
    g_assert_null(none);
}

/* A new file starts from the seed's [preferred], so the other portals keep
 * their backends; turning sfe off takes only its line out again. */
static void test_new_file(void)
{
    g_autofree char* seed = write_file("system/hyprland-portals.conf", "[preferred]\ndefault=hyprland;gtk\n");
    g_autofree char* path = g_build_filename(root, "new", "hyprland-portals.conf", NULL);

    g_assert_false(sfe_portal_conf_uses_sfe(path));
    g_assert_true(sfe_portal_conf_set(path, seed, false, NULL)); /* off with no file: nothing written */
    g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));

    g_assert_true(sfe_portal_conf_set(path, seed, true, NULL));
    g_assert_true(sfe_portal_conf_uses_sfe(path));
    g_autofree char* text = read_file(path);
    g_assert_nonnull(strstr(text, "default=hyprland;gtk"));
    g_assert_nonnull(strstr(text, "org.freedesktop.impl.portal.FileChooser=sfe"));

    g_assert_true(sfe_portal_conf_set(path, seed, false, NULL));
    g_assert_false(sfe_portal_conf_uses_sfe(path));
    g_autofree char* after = read_file(path);
    g_assert_nonnull(strstr(after, "default=hyprland;gtk"));
    g_assert_null(strstr(after, "FileChooser"));
}

/* A file the user wrote keeps its other keys and comments. */
static void test_existing_file(void)
{
    g_autofree char* path = write_file("own/hyprland-portals.conf",
        "# mine\n[preferred]\ndefault=hyprland;gtk\norg.freedesktop.impl.portal.Screenshot=wlr\n");
    g_assert_true(sfe_portal_conf_set(path, NULL, true, NULL));
    g_autofree char* text = read_file(path);
    g_assert_nonnull(strstr(text, "# mine"));
    g_assert_nonnull(strstr(text, "Screenshot=wlr"));
    g_assert_true(sfe_portal_conf_uses_sfe(path));

    /* Another backend chosen by hand is not sfe. */
    g_autofree char* other = write_file("other/x-portals.conf", "[preferred]\norg.freedesktop.impl.portal.FileChooser=gtk\n");
    g_assert_false(sfe_portal_conf_uses_sfe(other));
}

int main(int argc, char** argv)
{
    root = g_dir_make_tmp("sfe-portalconf-XXXXXX", NULL);
    g_setenv("XDG_CONFIG_HOME", root, TRUE);
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/portalconf/desktop", test_desktop);
    g_test_add_func("/portalconf/new-file", test_new_file);
    g_test_add_func("/portalconf/existing-file", test_existing_file);
    int status = g_test_run();
    g_autofree char* cmd = g_strdup_printf("rm -rf '%s'", root);
    g_spawn_command_line_sync(cmd, NULL, NULL, NULL, NULL);
    g_free(root);
    return status;
}
