#include "window.h"

#include "util.h"

#define SIDEBAR_WIDTH 200

struct _SfeWindow {
    GtkApplicationWindow parent;

    GtkWidget* sidebar; /* placeholder until milestone 3 */
    GtkWidget* content; /* placeholder until milestones 2 and 4 */
    GtkWidget* content_label;
    GFile* dir;
    gulong config_handler;
};

G_DEFINE_TYPE(SfeWindow, sfe_window, GTK_TYPE_APPLICATION_WINDOW)

static void set_directory(SfeWindow* win, GFile* dir)
{
    g_set_object(&win->dir, dir);

    g_autofree char* path = g_file_get_path(dir);
    g_autofree char* name = g_file_get_basename(dir);
    g_autofree char* title = g_strdup_printf("%s — sfe", name);
    gtk_window_set_title(GTK_WINDOW(win), title);
    gtk_label_set_text(GTK_LABEL(win->content_label), path);
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

static GtkWidget* build_sidebar(void)
{
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget* label = gtk_label_new("Sidebar");

    gtk_style_context_add_class(gtk_widget_get_style_context(box), "sfe-sidebar");
    gtk_style_context_add_class(gtk_widget_get_style_context(label), "sfe-placeholder");
    gtk_widget_set_size_request(box, SIDEBAR_WIDTH, -1);
    gtk_box_pack_start(GTK_BOX(box), label, TRUE, TRUE, 0);
    return box;
}

static void sfe_window_dispose(GObject* object)
{
    SfeWindow* win = SFE_WINDOW(object);

    if (win->config_handler != 0) {
        g_signal_handler_disconnect(sfe_app(), win->config_handler);
        win->config_handler = 0;
    }
    g_clear_object(&win->dir);
    G_OBJECT_CLASS(sfe_window_parent_class)->dispose(object);
}

static void sfe_window_class_init(SfeWindowClass* klass)
{
    G_OBJECT_CLASS(klass)->dispose = sfe_window_dispose;
}

static void sfe_window_init(SfeWindow* win)
{
    GtkWidget* paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);

    win->sidebar = build_sidebar();
    win->content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    win->content_label = gtk_label_new(NULL);
    gtk_style_context_add_class(gtk_widget_get_style_context(win->content_label), "sfe-placeholder");
    gtk_box_pack_start(GTK_BOX(win->content), win->content_label, TRUE, TRUE, 0);

    gtk_paned_pack1(GTK_PANED(paned), win->sidebar, FALSE, FALSE);
    gtk_paned_pack2(GTK_PANED(paned), win->content, TRUE, FALSE);
    gtk_container_add(GTK_CONTAINER(win), paned);

    gtk_window_set_default_size(GTK_WINDOW(win), 1100, 700);
    gtk_widget_show_all(paned);
}

SfeWindow* sfe_window_new(SfeApplication* app, GFile* dir)
{
    SfeWindow* win = g_object_new(SFE_TYPE_WINDOW, "application", app, NULL);

    if (dir != NULL) {
        set_directory(win, dir);
    } else {
        g_autofree char* path = sfe_expand_home(sfe_config()->start_path);
        g_autoptr(GFile) start = g_file_new_for_path(path);
        set_directory(win, start);
    }

    apply_config(win);
    win->config_handler = g_signal_connect(app, "config-changed", G_CALLBACK(on_config_changed), win);
    return win;
}
