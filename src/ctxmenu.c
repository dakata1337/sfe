#include "ctxmenu.h"

#include "actions.h"
#include "app.h"
#include <stdbool.h>
#include <string.h>

#define N_SECTIONS 10
#define USER_SECTION 8 /* custom commands, just above "Properties" */

/* "Open With ▸": applications registered for the first file's type, with
 * their icons, the default one first, then "Other Application…". */
static GMenu* open_with_menu(GFile* file)
{
    GMenu* menu = g_menu_new();
    g_autoptr(GFileInfo) info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
        G_FILE_QUERY_INFO_NONE, NULL, NULL);
    const char* type = info ? g_file_info_get_content_type(info) : NULL;

    if (type != NULL) {
        g_autoptr(GAppInfo) def = g_app_info_get_default_for_type(type, FALSE);
        GList* apps = g_app_info_get_all_for_type(type);
        if (def != NULL)
            apps = g_list_prepend(apps, g_object_ref(def));

        GHashTable* seen = g_hash_table_new(g_str_hash, g_str_equal);
        for (GList* l = apps; l != NULL; l = l->next) {
            const char* id = g_app_info_get_id(l->data);
            if (id == NULL || g_hash_table_contains(seen, id))
                continue;
            g_hash_table_add(seen, (gpointer)id);
            g_autoptr(GMenuItem) item = g_menu_item_new(g_app_info_get_name(l->data), NULL);
            g_menu_item_set_action_and_target_value(item, "win.open-with", g_variant_new_string(id));
            GIcon* icon = g_app_info_get_icon(l->data);
            g_autoptr(GIcon) fallback = icon ? NULL : g_themed_icon_new("application-x-executable");
            g_menu_item_set_icon(item, icon ? icon : fallback); /* every row has one: labels line up */
            g_menu_append_item(menu, item);
        }
        g_hash_table_unref(seen);
        g_list_free_full(apps, g_object_unref);
    }
    g_autoptr(GMenuItem) other = g_menu_item_new("Other Application…", "win.open-with-other");
    g_autoptr(GIcon) other_icon = g_themed_icon_new("view-more-symbolic");
    g_menu_item_set_icon(other, other_icon);
    g_menu_append_item(menu, other);
    return menu;
}

static GMenuModel* build_menu(const SfeActionTarget* t)
{
    unsigned where = t->files ? SFE_MENU_ITEMS : SFE_MENU_FOLDER;
    GMenu* sections[N_SECTIONS] = { NULL };
    size_t n = 0;
    const SfeAction* table = sfe_actions_get(&n);

    for (size_t i = 0; i < n; i++) {
        const SfeAction* a = &table[i];
        if (!(a->menu & where) || (a->visible != NULL && !a->visible(t)))
            continue;

        GMenu** section = &sections[CLAMP(a->section, 0, N_SECTIONS - 1)];
        if (*section == NULL)
            *section = g_menu_new();
        g_autofree char* detailed = g_strconcat("win.", a->name, NULL);
        g_menu_append(*section, a->label, detailed);

        if (strcmp(a->name, "open") == 0) {
            g_autoptr(GMenu) apps = open_with_menu(t->files->data);
            g_menu_append_submenu(*section, "Open With", G_MENU_MODEL(apps));
        }
    }

    /* Custom commands defined for every kind in the selection (files,
     * directories, or both for a mixed selection), each name once. */
    const SfeConfig* cfg = sfe_config();
    unsigned kinds = sfe_action_target_kinds(t);
    g_autoptr(GHashTable) listed = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < cfg->actions->len; i++) {
        const SfeUserAction* ua = g_ptr_array_index(cfg->actions, i);
        bool fits = (!(kinds & SFE_ACTION_FILES) || sfe_find_user_action(ua->name, SFE_ACTION_FILES))
            && (!(kinds & SFE_ACTION_DIRECTORIES) || sfe_find_user_action(ua->name, SFE_ACTION_DIRECTORIES));
        if (!fits || g_hash_table_contains(listed, ua->name))
            continue;
        g_hash_table_add(listed, ua->name);
        if (sections[USER_SECTION] == NULL)
            sections[USER_SECTION] = g_menu_new();
        g_autoptr(GMenuItem) item = g_menu_item_new(ua->name, NULL);
        g_menu_item_set_action_and_target_value(item, "win.user-action", g_variant_new_string(ua->name));
        g_menu_append_item(sections[USER_SECTION], item);
    }

    GMenu* menu = g_menu_new();
    for (int s = 0; s < N_SECTIONS; s++) {
        if (sections[s] != NULL) {
            g_menu_append_section(menu, NULL, G_MENU_MODEL(sections[s]));
            g_object_unref(sections[s]);
        }
    }
    return G_MENU_MODEL(menu);
}

static gboolean destroy_menu(gpointer menu)
{
    gtk_widget_destroy(menu);
    return G_SOURCE_REMOVE;
}

/* GTK emits "deactivate" before running the chosen item. */
static void destroy_menu_later(GtkMenuShell* menu, gpointer data)
{
    (void)data;
    g_idle_add(destroy_menu, menu);
}

/* Submenus (Open With) are separate menus: style them too. */
static void style_menu(GtkWidget* menu)
{
    gtk_style_context_add_class(gtk_widget_get_style_context(menu), "sfe-menu");
    GList* items = gtk_container_get_children(GTK_CONTAINER(menu));
    for (GList* l = items; l != NULL; l = l->next) {
        GtkWidget* sub = GTK_IS_MENU_ITEM(l->data) ? gtk_menu_item_get_submenu(l->data) : NULL;
        if (sub != NULL)
            style_menu(sub);
    }
    g_list_free(items);
}

void sfe_context_menu_popup(SfeWindow* window, GtkWidget* attach, const GdkEvent* event)
{
    SfeActionTarget t = sfe_action_target_for(window);
    g_autoptr(GMenuModel) model = build_menu(&t);
    GtkWidget* menu = gtk_menu_new_from_model(model);

    sfe_action_target_clear(&t);
    style_menu(menu);
    gtk_menu_attach_to_widget(GTK_MENU(menu), attach, NULL);
    g_signal_connect(menu, "deactivate", G_CALLBACK(destroy_menu_later), NULL);

    if (event != NULL)
        gtk_menu_popup_at_pointer(GTK_MENU(menu), event);
    else
        gtk_menu_popup_at_widget(GTK_MENU(menu), attach, GDK_GRAVITY_CENTER, GDK_GRAVITY_NORTH_WEST, NULL);
}
