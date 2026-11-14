#include "window.h"

#include "bookmarks.h"
#include "pane.h"
#include "sidebar.h"
#include "split.h"
#include "util.h"

#define SIDEBAR_WIDTH 220 /* initial width */
#define SIDEBAR_MIN_WIDTH 150

struct _SfeWindow {
    GtkApplicationWindow parent;

    SfeSidebar* sidebar;
    SfeSplit* split; /* the directory panes */
    gulong config_handler;
};

G_DEFINE_TYPE(SfeWindow, sfe_window, GTK_TYPE_APPLICATION_WINDOW)

static SfePane* active_pane(SfeWindow* win)
{
    return sfe_split_get_active(win->split);
}

static void update_title(SfeWindow* win)
{
    GFile* dir = sfe_pane_get_dir(active_pane(win));
    g_autofree char* name = g_file_get_basename(dir);
    g_autofree char* title = g_strdup_printf("%s — sfe", name);
    gtk_window_set_title(GTK_WINDOW(win), title);
}

/* The title and the sidebar always follow the active pane. */
static void follow_active_pane(SfeWindow* win)
{
    update_title(win);
    sfe_sidebar_set_location(win->sidebar, sfe_pane_get_dir(active_pane(win)));
}

static void on_directory_changed(SfeSplit* split, SfePane* pane, gpointer data)
{
    (void)split;
    SfeWindow* win = SFE_WINDOW(data);
    if (pane == active_pane(win))
        follow_active_pane(win);
}

static void on_active_changed(SfeSplit* split, gpointer data)
{
    (void)split;
    follow_active_pane(SFE_WINDOW(data));
}

static void on_set_focus(GtkWindow* window, GtkWidget* widget, gpointer data)
{
    (void)window;
    if (widget != NULL)
        sfe_split_track_focus(SFE_WINDOW(data)->split, widget);
}

static void on_sidebar_open(SfeSidebar* sidebar, GFile* dir, gboolean new_window, gpointer data)
{
    (void)sidebar;
    SfeWindow* win = SFE_WINDOW(data);

    if (new_window)
        gtk_window_present(GTK_WINDOW(sfe_window_new(sfe_app(), dir)));
    else
        sfe_pane_navigate(active_pane(win), dir);
}

static void apply_config(SfeWindow* win)
{
    gtk_window_set_decorated(GTK_WINDOW(win), sfe_config()->decorations);
}

static void on_config_changed(SfeApplication* app, gpointer data)
{
    (void)app;
    apply_config(SFE_WINDOW(data));
}

/* ------------------------------------------------------------------------ */
/* Window actions: they act on the active pane                               */
/* ------------------------------------------------------------------------ */

#define PANE_ACTION(action_name, call)                                         \
    static void action_name(GSimpleAction* a, GVariant* p, gpointer data)      \
    {                                                                          \
        (void)a, (void)p;                                                      \
        call(active_pane(SFE_WINDOW(data)));                                   \
    }

PANE_ACTION(action_back, sfe_pane_go_back)
PANE_ACTION(action_forward, sfe_pane_go_forward)
PANE_ACTION(action_up, sfe_pane_go_up)
PANE_ACTION(action_reload, sfe_pane_reload)
PANE_ACTION(action_location, sfe_pane_focus_location)

static void action_view_grid(GSimpleAction* a, GVariant* p, gpointer data)
{
    (void)a, (void)p;
    sfe_pane_set_view(active_pane(SFE_WINDOW(data)), SFE_VIEW_GRID);
}

static void action_view_list(GSimpleAction* a, GVariant* p, gpointer data)
{
    (void)a, (void)p;
    sfe_pane_set_view(active_pane(SFE_WINDOW(data)), SFE_VIEW_LIST);
}

/* Hidden files are a global, persisted setting. */
static void action_toggle_hidden(GSimpleAction* a, GVariant* p, gpointer data)
{
    (void)a, (void)p, (void)data;
    SfeConfig* cfg = sfe_application_dup_config(sfe_app());
    cfg->show_hidden = !cfg->show_hidden;
    sfe_application_set_config(sfe_app(), cfg);
}

/* Ctrl+D: bookmark the active directory, or remove its bookmark. */
static void action_bookmark(GSimpleAction* a, GVariant* p, gpointer data)
{
    (void)a, (void)p;
    GFile* dir = sfe_pane_get_dir(active_pane(SFE_WINDOW(data)));
    SfeConfig* cfg = sfe_application_dup_config(sfe_app());
    int index = sfe_bookmarks_find(cfg, dir);

    if (index >= 0)
        sfe_bookmarks_remove(cfg, index);
    else
        sfe_bookmarks_add(cfg, dir);
    sfe_application_set_config(sfe_app(), cfg);
}

static void action_split_vertical(GSimpleAction* a, GVariant* p, gpointer data)
{
    (void)a, (void)p;
    SfeWindow* win = SFE_WINDOW(data);
    sfe_split_split(win->split, active_pane(win), SFE_SPLIT_VERTICAL);
}

/* From the left pane the right pane is the one that can be stacked, so
 * splitting "horizontally" there splits the right pane. */
static void action_split_horizontal(GSimpleAction* a, GVariant* p, gpointer data)
{
    (void)a, (void)p;
    SfeWindow* win = SFE_WINDOW(data);
    SfePane* right = sfe_split_get_pane(win->split, 1);
    if (right != NULL)
        sfe_split_split(win->split, right, SFE_SPLIT_HORIZONTAL);
}

/* Ctrl+W: close the active pane, or the window when it is the last one. */
static void action_close_pane(GSimpleAction* a, GVariant* p, gpointer data)
{
    (void)a, (void)p;
    SfeWindow* win = SFE_WINDOW(data);
    if (sfe_split_get_n_panes(win->split) > 1)
        sfe_split_close(win->split, active_pane(win));
    else
        gtk_window_close(GTK_WINDOW(win));
}

static void action_next_pane(GSimpleAction* a, GVariant* p, gpointer data)
{
    (void)a, (void)p;
    sfe_split_focus_next(SFE_WINDOW(data)->split);
}

static const GActionEntry win_actions[] = {
    { .name = "bookmark", .activate = action_bookmark },
    { .name = "split-vertical", .activate = action_split_vertical },
    { .name = "split-horizontal", .activate = action_split_horizontal },
    { .name = "close-pane", .activate = action_close_pane },
    { .name = "next-pane", .activate = action_next_pane },
    { .name = "back", .activate = action_back },
    { .name = "forward", .activate = action_forward },
    { .name = "up", .activate = action_up },
    { .name = "reload", .activate = action_reload },
    { .name = "location", .activate = action_location },
    { .name = "view-grid", .activate = action_view_grid },
    { .name = "view-list", .activate = action_view_list },
    { .name = "toggle-hidden", .activate = action_toggle_hidden },
};

/* ------------------------------------------------------------------------ */
/* Construction                                                              */
/* ------------------------------------------------------------------------ */

static void sfe_window_dispose(GObject* object)
{
    SfeWindow* win = SFE_WINDOW(object);

    if (win->config_handler != 0) {
        g_signal_handler_disconnect(sfe_app(), win->config_handler);
        win->config_handler = 0;
    }
    G_OBJECT_CLASS(sfe_window_parent_class)->dispose(object);
}

static void sfe_window_class_init(SfeWindowClass* klass)
{
    G_OBJECT_CLASS(klass)->dispose = sfe_window_dispose;
}

static void sfe_window_init(SfeWindow* win)
{
    gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(win)), "sfe-window");
    g_action_map_add_action_entries(G_ACTION_MAP(win), win_actions, G_N_ELEMENTS(win_actions), win);
    gtk_window_set_default_size(GTK_WINDOW(win), 1100, 700);
}

SfeWindow* sfe_window_new(SfeApplication* app, GFile* dir)
{
    SfeWindow* win = g_object_new(SFE_TYPE_WINDOW, "application", app, NULL);
    g_autoptr(GFile) start = NULL;

    if (dir == NULL) {
        g_autofree char* path = sfe_expand_home(sfe_config()->start_path);
        dir = start = g_file_new_for_path(path);
    }

    GtkWidget* paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    win->sidebar = SFE_SIDEBAR(sfe_sidebar_new());
    gtk_widget_set_size_request(GTK_WIDGET(win->sidebar), SIDEBAR_MIN_WIDTH, -1);
    gtk_paned_set_position(GTK_PANED(paned), SIDEBAR_WIDTH);
    g_signal_connect(win->sidebar, "open-location", G_CALLBACK(on_sidebar_open), win);
    win->split = sfe_split_new(dir);
    g_signal_connect(win->split, "directory-changed", G_CALLBACK(on_directory_changed), win);
    g_signal_connect(win->split, "active-changed", G_CALLBACK(on_active_changed), win);
    g_signal_connect_after(win, "set-focus", G_CALLBACK(on_set_focus), win);
    sfe_sidebar_set_location(win->sidebar, dir);

    gtk_paned_pack1(GTK_PANED(paned), GTK_WIDGET(win->sidebar), FALSE, FALSE);
    gtk_paned_pack2(GTK_PANED(paned), GTK_WIDGET(win->split), TRUE, FALSE);
    gtk_container_add(GTK_CONTAINER(win), paned);
    gtk_widget_show_all(paned);

    update_title(win);
    apply_config(win);
    win->config_handler = g_signal_connect(app, "config-changed", G_CALLBACK(on_config_changed), win);
    sfe_pane_focus_view(active_pane(win));
    return win;
}
