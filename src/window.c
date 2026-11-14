#include "window.h"

#include "actions.h"
#include "opsbar.h"
#include "sidebar.h"
#include "split.h"
#include "util.h"

#define SIDEBAR_WIDTH 220 /* initial width */
#define SIDEBAR_MIN_WIDTH 150

struct _SfeWindow {
    GtkApplicationWindow parent;

    SfeSidebar* sidebar;
    SfeSplit* split; /* the directory panes */
    SfeOpsBar* ops_bar; /* running file operations */
    gulong config_handler;
};

G_DEFINE_TYPE(SfeWindow, sfe_window, GTK_TYPE_APPLICATION_WINDOW)

SfeSplit* sfe_window_get_split(SfeWindow* win)
{
    return win->split;
}

SfePane* sfe_window_get_active_pane(SfeWindow* win)
{
    return sfe_split_get_active(win->split);
}

void sfe_window_run_op(SfeWindow* win, SfeFileOp* op)
{
    sfe_ops_bar_run(win->ops_bar, op);
}

/* ------------------------------------------------------------------------ */
/* Following the active pane                                                 */
/* ------------------------------------------------------------------------ */

static void update_title(SfeWindow* win)
{
    GFile* dir = sfe_pane_get_dir(sfe_window_get_active_pane(win));
    g_autofree char* name = g_file_get_basename(dir);
    g_autofree char* title = g_strdup_printf("%s — sfe", name);
    gtk_window_set_title(GTK_WINDOW(win), title);
}

/* The title and the sidebar always follow the active pane. */
static void follow_active_pane(SfeWindow* win)
{
    update_title(win);
    sfe_sidebar_set_location(win->sidebar, sfe_pane_get_dir(sfe_window_get_active_pane(win)));
}

static void on_directory_changed(SfeSplit* split, SfePane* pane, gpointer data)
{
    (void)split;
    SfeWindow* win = SFE_WINDOW(data);
    if (pane == sfe_window_get_active_pane(win))
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
        sfe_pane_navigate(sfe_window_get_active_pane(win), dir);
}

static void apply_config(SfeWindow* win)
{
    gtk_window_set_decorated(GTK_WINDOW(win), sfe_config()->decorations);
    sfe_actions_sync_state(win);
}

static void on_config_changed(SfeApplication* app, gpointer data)
{
    (void)app;
    apply_config(SFE_WINDOW(data));
}

/* ------------------------------------------------------------------------ */
/* Keys                                                                      */
/* ------------------------------------------------------------------------ */

/* GTK3 runs window shortcuts before the focused widget sees the key, so
 * Delete or Ctrl+C in the path entry would act on files. Text fields get
 * the key first; shortcuts apply only if they do not use it. */
static gboolean sfe_window_key_press(GtkWidget* widget, GdkEventKey* event)
{
    GtkWidget* focus = gtk_window_get_focus(GTK_WINDOW(widget));

    if (GTK_IS_EDITABLE(focus) && gtk_window_propagate_key_event(GTK_WINDOW(widget), event))
        return TRUE;
    return GTK_WIDGET_CLASS(sfe_window_parent_class)->key_press_event(widget, event);
}

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
    GTK_WIDGET_CLASS(klass)->key_press_event = sfe_window_key_press;
}

static void sfe_window_init(SfeWindow* win)
{
    gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(win)), "sfe-window");
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

    win->sidebar = SFE_SIDEBAR(sfe_sidebar_new());
    gtk_widget_set_size_request(GTK_WIDGET(win->sidebar), SIDEBAR_MIN_WIDTH, -1);
    g_signal_connect(win->sidebar, "open-location", G_CALLBACK(on_sidebar_open), win);

    win->split = sfe_split_new(dir);
    g_signal_connect(win->split, "directory-changed", G_CALLBACK(on_directory_changed), win);
    g_signal_connect(win->split, "active-changed", G_CALLBACK(on_active_changed), win);
    g_signal_connect_after(win, "set-focus", G_CALLBACK(on_set_focus), win);

    win->ops_bar = SFE_OPS_BAR(sfe_ops_bar_new());
    sfe_actions_add_to_window(win);

    /* sidebar | (panes over the operations bar) */
    GtkWidget* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(content), GTK_WIDGET(win->split), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(content), GTK_WIDGET(win->ops_bar), FALSE, FALSE, 0);

    GtkWidget* paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_pack1(GTK_PANED(paned), GTK_WIDGET(win->sidebar), FALSE, FALSE);
    gtk_paned_pack2(GTK_PANED(paned), content, TRUE, FALSE);
    gtk_paned_set_position(GTK_PANED(paned), SIDEBAR_WIDTH);
    gtk_container_add(GTK_CONTAINER(win), paned);
    gtk_widget_show_all(paned);

    sfe_sidebar_set_location(win->sidebar, dir);
    update_title(win);
    apply_config(win);
    win->config_handler = g_signal_connect(app, "config-changed", G_CALLBACK(on_config_changed), win);
    sfe_pane_focus_view(sfe_window_get_active_pane(win));
    return win;
}
