#include "sidebar.h"

#include "app.h"
#include "bookmarks.h"
#include "devices.h"
#include "dialogs.h"
#include "dirtree.h"
#include <stdlib.h>
#include <string.h>

#define DND_TARGET "SFE_BOOKMARK_ROW"

typedef enum {
    SECTION_PLACES,
    SECTION_BOOKMARKS,
    SECTION_DEVICES,
} Section;

static const char* const section_titles[] = { "Places", "Bookmarks", "Devices" };

/* What a list row stands for. */
typedef struct {
    Section section;
    GFile* file; /* NULL for an unmounted device */
    char* device_id; /* devices only */
    bool removable; /* devices only */
    int bookmark_index; /* index in the config's bookmarks, or -1 */
} RowData;

struct _SfeSidebar {
    GtkPaned parent;

    GtkWidget* list; /* GtkListBox */
    SfeDirTree* tree;
    GFile* location;
    gulong config_handler;
    gulong devices_handler;
    bool positioned; /* divider placed at 50% once */
};

G_DEFINE_TYPE(SfeSidebar, sfe_sidebar, GTK_TYPE_PANED)

enum {
    SIGNAL_OPEN_LOCATION,
    N_SIGNALS,
};
static guint signals[N_SIGNALS];

static void rebuild(SfeSidebar* self);

/* ------------------------------------------------------------------------ */
/* Rows                                                                      */
/* ------------------------------------------------------------------------ */

static void row_data_free(gpointer p)
{
    RowData* d = p;
    g_clear_object(&d->file);
    g_free(d->device_id);
    g_free(d);
}

static RowData* row_data(GtkListBoxRow* row)
{
    return row ? g_object_get_data(G_OBJECT(row), "sfe-row") : NULL;
}

static void open_location(SfeSidebar* self, GFile* file, bool new_window)
{
    g_signal_emit(self, signals[SIGNAL_OPEN_LOCATION], 0, file, (gboolean)new_window);
}

static GtkWidget* add_row(SfeSidebar* self, Section section, const char* icon, const char* label, GFile* file)
{
    GtkWidget* row = gtk_list_box_row_new();
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget* text = gtk_label_new(label);
    RowData* d = g_new0(RowData, 1);

    d->section = section;
    d->file = file ? g_object_ref(file) : NULL;
    d->bookmark_index = -1;
    g_object_set_data_full(G_OBJECT(row), "sfe-row", d, row_data_free);

    gtk_label_set_xalign(GTK_LABEL(text), 0);
    gtk_label_set_ellipsize(GTK_LABEL(text), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(box), gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_MENU), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), text, TRUE, TRUE, 0);
    gtk_widget_set_tooltip_text(row, file ? g_file_peek_path(file) : label);

    /* An event box makes the row usable as a drag source. */
    GtkWidget* events = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(events), FALSE);
    gtk_container_add(GTK_CONTAINER(events), box);
    gtk_container_add(GTK_CONTAINER(row), events);
    gtk_container_add(GTK_CONTAINER(self->list), row);
    return row;
}

static GtkWidget* row_box(GtkWidget* row)
{
    return gtk_bin_get_child(GTK_BIN(gtk_bin_get_child(GTK_BIN(row))));
}

/* Section titles above the first row of each section. */
static void update_header(GtkListBoxRow* row, GtkListBoxRow* before, gpointer data)
{
    (void)data;
    RowData* d = row_data(row);
    RowData* prev = row_data(before);

    if (prev != NULL && prev->section == d->section) {
        gtk_list_box_row_set_header(row, NULL);
        return;
    }
    if (gtk_list_box_row_get_header(row) == NULL) {
        GtkWidget* title = gtk_label_new(section_titles[d->section]);
        gtk_label_set_xalign(GTK_LABEL(title), 0);
        gtk_style_context_add_class(gtk_widget_get_style_context(title), "sfe-sidebar-title");
        gtk_list_box_row_set_header(row, title);
    }
}

/* ------------------------------------------------------------------------ */
/* Places                                                                    */
/* ------------------------------------------------------------------------ */

static void add_places(SfeSidebar* self)
{
    /* `fallback` is used when xdg-user-dirs is not set up. */
    static const struct {
        GUserDirectory dir;
        const char* fallback;
        const char* icon;
    } special[] = {
        { G_USER_DIRECTORY_DESKTOP, "Desktop", "user-desktop-symbolic" },
        { G_USER_DIRECTORY_DOCUMENTS, "Documents", "folder-documents-symbolic" },
        { G_USER_DIRECTORY_DOWNLOAD, "Downloads", "folder-download-symbolic" },
        { G_USER_DIRECTORY_MUSIC, "Music", "folder-music-symbolic" },
        { G_USER_DIRECTORY_PICTURES, "Pictures", "folder-pictures-symbolic" },
        { G_USER_DIRECTORY_VIDEOS, "Videos", "folder-videos-symbolic" },
    };
    const char* home = g_get_home_dir();
    g_autoptr(GFile) home_file = g_file_new_for_path(home);
    g_autoptr(GFile) root = g_file_new_for_path("/");

    add_row(self, SECTION_PLACES, "user-home-symbolic", "Home", home_file);
    for (size_t i = 0; i < G_N_ELEMENTS(special); i++) {
        const char* xdg = g_get_user_special_dir(special[i].dir);
        g_autofree char* path = xdg ? g_strdup(xdg) : g_build_filename(home, special[i].fallback, NULL);
        if (g_strcmp0(path, home) == 0 || !g_file_test(path, G_FILE_TEST_IS_DIR))
            continue;
        g_autoptr(GFile) file = g_file_new_for_path(path);
        g_autofree char* name = g_filename_display_basename(path);
        add_row(self, SECTION_PLACES, special[i].icon, name, file);
    }
    add_row(self, SECTION_PLACES, "drive-harddisk-symbolic", "File System", root);
}

/* ------------------------------------------------------------------------ */
/* Bookmarks (drag a row onto another to reorder)                           */
/* ------------------------------------------------------------------------ */

static const GtkTargetEntry dnd_targets[] = {
    { DND_TARGET, GTK_TARGET_SAME_APP, 0 },
};

static void on_drag_data_get(GtkWidget* widget, GdkDragContext* ctx, GtkSelectionData* sel,
    guint info, guint time, gpointer data)
{
    (void)ctx, (void)info, (void)time, (void)data;
    RowData* d = row_data(GTK_LIST_BOX_ROW(gtk_widget_get_ancestor(widget, GTK_TYPE_LIST_BOX_ROW)));
    g_autofree char* index = g_strdup_printf("%d", d->bookmark_index);
    gtk_selection_data_set(sel, gdk_atom_intern_static_string(DND_TARGET), 8, (guchar*)index, (int)strlen(index));
}

static void on_drag_data_received(GtkWidget* row, GdkDragContext* ctx, int x, int y,
    GtkSelectionData* sel, guint info, guint time, gpointer data)
{
    (void)x, (void)y, (void)info, (void)data;
    RowData* target = row_data(GTK_LIST_BOX_ROW(row));
    g_autofree char* text = g_strndup((const char*)gtk_selection_data_get_data(sel),
        gtk_selection_data_get_length(sel));
    int from = atoi(text);

    if (target->bookmark_index >= 0 && from != target->bookmark_index) {
        SfeConfig* cfg = sfe_application_dup_config(sfe_app());
        sfe_bookmarks_move(cfg, from, target->bookmark_index);
        sfe_application_set_config(sfe_app(), cfg); /* rebuilds via config-changed */
    }
    gtk_drag_finish(ctx, TRUE, FALSE, time);
}

static void on_drag_begin(GtkWidget* widget, GdkDragContext* ctx, gpointer data)
{
    (void)widget, (void)data;
    gtk_drag_set_icon_name(ctx, "folder-symbolic", 0, 0);
}

static void make_reorderable(GtkWidget* row)
{
    GtkWidget* events = gtk_bin_get_child(GTK_BIN(row));

    gtk_drag_source_set(events, GDK_BUTTON1_MASK, dnd_targets, G_N_ELEMENTS(dnd_targets), GDK_ACTION_MOVE);
    g_signal_connect(events, "drag-begin", G_CALLBACK(on_drag_begin), NULL);
    g_signal_connect(events, "drag-data-get", G_CALLBACK(on_drag_data_get), NULL);
    gtk_drag_dest_set(row, GTK_DEST_DEFAULT_ALL, dnd_targets, G_N_ELEMENTS(dnd_targets), GDK_ACTION_MOVE);
    g_signal_connect(row, "drag-data-received", G_CALLBACK(on_drag_data_received), NULL);
}

static void add_bookmarks(SfeSidebar* self)
{
    g_autoptr(GPtrArray) bookmarks = sfe_bookmarks_list(sfe_config(), NULL);

    for (guint i = 0; i < bookmarks->len; i++) {
        SfeBookmark* b = g_ptr_array_index(bookmarks, i);
        GtkWidget* row = add_row(self, SECTION_BOOKMARKS, "folder-symbolic", b->label, b->file);
        row_data(GTK_LIST_BOX_ROW(row))->bookmark_index = b->config_index;
        if (b->config_index >= 0)
            make_reorderable(row);
    }
}

/* ------------------------------------------------------------------------ */
/* Devices                                                                   */
/* ------------------------------------------------------------------------ */

static void on_unmounted(GObject* source, GAsyncResult* res, gpointer data)
{
    g_autoptr(SfeSidebar) self = data;
    g_autoptr(GError) error = NULL;

    if (!sfe_devices_unmount_finish(SFE_DEVICES(source), res, &error))
        sfe_show_error(GTK_WIDGET(self), "Cannot unmount the device", error->message);
}

static void unmount_device(SfeSidebar* self, RowData* d, bool eject)
{
    sfe_devices_unmount(sfe_devices_get_default(), d->device_id, eject, NULL, on_unmounted, g_object_ref(self));
}

static void on_mounted(GObject* source, GAsyncResult* res, gpointer data)
{
    g_autoptr(SfeSidebar) self = data;
    g_autoptr(GError) error = NULL;
    g_autofree char* path = sfe_devices_mount_finish(SFE_DEVICES(source), res, &error);

    if (path == NULL) {
        sfe_show_error(GTK_WIDGET(self), "Cannot mount the device", error->message);
        return;
    }
    g_autoptr(GFile) file = g_file_new_for_path(path);
    open_location(self, file, false);
}

static void mount_device(SfeSidebar* self, RowData* d)
{
    sfe_devices_mount(sfe_devices_get_default(), d->device_id, NULL, on_mounted, g_object_ref(self));
}

static void on_eject_clicked(GtkButton* button, gpointer data)
{
    SfeSidebar* self = data;
    RowData* d = row_data(GTK_LIST_BOX_ROW(gtk_widget_get_ancestor(GTK_WIDGET(button), GTK_TYPE_LIST_BOX_ROW)));
    unmount_device(self, d, d->removable);
}

static void add_devices(SfeSidebar* self)
{
    GPtrArray* devices = sfe_devices_get_list(sfe_devices_get_default());

    for (guint i = 0; i < devices->len; i++) {
        SfeDevice* dev = g_ptr_array_index(devices, i);
        g_autoptr(GFile) file = dev->mount_path ? g_file_new_for_path(dev->mount_path) : NULL;
        GtkWidget* row = add_row(self, SECTION_DEVICES, dev->icon_name, dev->name, file);
        RowData* d = row_data(GTK_LIST_BOX_ROW(row));

        d->device_id = g_strdup(dev->id);
        d->removable = dev->removable;
        if (!dev->mount_path)
            gtk_style_context_add_class(gtk_widget_get_style_context(row), "sfe-unmounted");
        if (dev->can_unmount) {
            GtkWidget* eject = gtk_button_new_from_icon_name("media-eject-symbolic", GTK_ICON_SIZE_MENU);
            gtk_widget_set_tooltip_text(eject, dev->removable ? "Eject" : "Unmount");
            gtk_widget_set_focus_on_click(eject, FALSE);
            gtk_style_context_add_class(gtk_widget_get_style_context(eject), "sfe-eject");
            g_signal_connect(eject, "clicked", G_CALLBACK(on_eject_clicked), self);
            gtk_box_pack_end(GTK_BOX(row_box(row)), eject, FALSE, FALSE, 0);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Activation and context menu                                               */
/* ------------------------------------------------------------------------ */

static void on_row_activated(GtkListBox* list, GtkListBoxRow* row, gpointer data)
{
    (void)list;
    SfeSidebar* self = data;
    RowData* d = row_data(row);

    if (d->file != NULL)
        open_location(self, d->file, false);
    else if (d->device_id != NULL)
        mount_device(self, d);
}

typedef enum {
    MENU_OPEN_NEW_WINDOW,
    MENU_REMOVE_BOOKMARK,
    MENU_MOUNT,
    MENU_UNMOUNT,
    MENU_EJECT,
} MenuCommand;

static void remove_bookmark(int index)
{
    SfeConfig* cfg = sfe_application_dup_config(sfe_app());
    sfe_bookmarks_remove(cfg, index);
    sfe_application_set_config(sfe_app(), cfg);
}

static void on_menu_item(GtkMenuItem* item, gpointer data)
{
    SfeSidebar* self = data;
    MenuCommand command = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "sfe-command"));
    RowData* d = row_data(g_object_get_data(G_OBJECT(item), "sfe-row"));

    switch (command) {
    case MENU_OPEN_NEW_WINDOW: open_location(self, d->file, true); break;
    case MENU_REMOVE_BOOKMARK: remove_bookmark(d->bookmark_index); break;
    case MENU_MOUNT: mount_device(self, d); break;
    case MENU_UNMOUNT: unmount_device(self, d, false); break;
    case MENU_EJECT: unmount_device(self, d, true); break;
    }
}

static void add_menu_item(SfeSidebar* self, GtkWidget* menu, const char* label, MenuCommand command,
    GtkListBoxRow* row)
{
    GtkWidget* item = gtk_menu_item_new_with_label(label);
    g_object_set_data(G_OBJECT(item), "sfe-command", GINT_TO_POINTER(command));
    g_object_set_data_full(G_OBJECT(item), "sfe-row", g_object_ref(row), g_object_unref);
    g_signal_connect(item, "activate", G_CALLBACK(on_menu_item), self);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
}

static gboolean destroy_menu(gpointer menu)
{
    gtk_widget_destroy(menu);
    return G_SOURCE_REMOVE;
}

/* GTK emits "deactivate" before activating the chosen item, so destroying
 * the menu right away would destroy the item before it runs. */
static void destroy_menu_later(GtkMenuShell* menu, gpointer data)
{
    (void)data;
    g_idle_add(destroy_menu, menu);
}

/* Shows the row's menu at the pointer (`event`) or, from the keyboard, below the row. */
static void show_row_menu(SfeSidebar* self, GtkListBoxRow* row, GdkEvent* event)
{
    RowData* d = row_data(row);
    GtkWidget* menu = gtk_menu_new();

    if (d->file != NULL)
        add_menu_item(self, menu, "Open in New Window", MENU_OPEN_NEW_WINDOW, row);
    if (d->bookmark_index >= 0)
        add_menu_item(self, menu, "Remove Bookmark", MENU_REMOVE_BOOKMARK, row);
    if (d->device_id != NULL && d->file == NULL)
        add_menu_item(self, menu, "Mount", MENU_MOUNT, row);
    if (d->device_id != NULL && d->file != NULL)
        add_menu_item(self, menu, "Unmount", MENU_UNMOUNT, row);
    if (d->device_id != NULL && d->removable)
        add_menu_item(self, menu, "Eject", MENU_EJECT, row);

    gtk_widget_show_all(menu);
    gtk_menu_attach_to_widget(GTK_MENU(menu), self->list, NULL);
    g_signal_connect(menu, "deactivate", G_CALLBACK(destroy_menu_later), NULL);
    if (event != NULL)
        gtk_menu_popup_at_pointer(GTK_MENU(menu), event);
    else
        gtk_menu_popup_at_widget(GTK_MENU(menu), GTK_WIDGET(row), GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST, NULL);
}

static gboolean on_list_button_press(GtkWidget* list, GdkEventButton* event, gpointer data)
{
    SfeSidebar* self = data;
    /* The event may come from a row's event box, whose coordinates differ
     * from the list's: find the row from the widget that got the click. */
    GtkWidget* target = gtk_get_event_widget((GdkEvent*)event);
    GtkWidget* ancestor = target ? gtk_widget_get_ancestor(target, GTK_TYPE_LIST_BOX_ROW) : NULL;
    GtkListBoxRow* row = ancestor ? GTK_LIST_BOX_ROW(ancestor)
                                  : gtk_list_box_get_row_at_y(GTK_LIST_BOX(list), (int)event->y);
    RowData* d = row_data(row);

    if (d == NULL || event->type != GDK_BUTTON_PRESS)
        return FALSE;
    if (event->button == GDK_BUTTON_MIDDLE && d->file != NULL) {
        open_location(self, d->file, true);
        return TRUE;
    }
    if (event->button != GDK_BUTTON_SECONDARY)
        return FALSE;
    show_row_menu(self, row, (GdkEvent*)event);
    return TRUE;
}

/* Menu key / Shift+F10 on the focused row. */
static gboolean on_list_popup_menu(GtkWidget* list, gpointer data)
{
    GtkWidget* focus = gtk_window_get_focus(GTK_WINDOW(gtk_widget_get_toplevel(list)));
    GtkWidget* row = focus ? gtk_widget_get_ancestor(focus, GTK_TYPE_LIST_BOX_ROW) : NULL;

    if (row == NULL)
        return FALSE;
    show_row_menu(SFE_SIDEBAR(data), GTK_LIST_BOX_ROW(row), NULL);
    return TRUE;
}

/* ------------------------------------------------------------------------ */
/* Building / highlighting                                                   */
/* ------------------------------------------------------------------------ */

static void highlight_location(SfeSidebar* self)
{
    GtkListBox* list = GTK_LIST_BOX(self->list);
    GList* rows = gtk_container_get_children(GTK_CONTAINER(list));

    gtk_list_box_unselect_all(list);
    for (GList* l = rows; l != NULL && self->location != NULL; l = l->next) {
        RowData* d = row_data(l->data);
        if (d->file != NULL && g_file_equal(d->file, self->location)) {
            gtk_list_box_select_row(list, l->data);
            break;
        }
    }
    g_list_free(rows);
}

static void rebuild(SfeSidebar* self)
{
    GList* rows = gtk_container_get_children(GTK_CONTAINER(self->list));
    for (GList* l = rows; l != NULL; l = l->next)
        gtk_widget_destroy(l->data);
    g_list_free(rows);

    add_places(self);
    add_bookmarks(self);
    add_devices(self);
    gtk_widget_show_all(self->list);
    highlight_location(self);
}

void sfe_sidebar_set_location(SfeSidebar* self, GFile* dir)
{
    g_set_object(&self->location, dir);
    highlight_location(self);
    sfe_dir_tree_set_location(self->tree, dir);
}

static void on_config_changed(SfeApplication* app, gpointer data)
{
    (void)app;
    SfeSidebar* self = data;
    sfe_dir_tree_set_show_hidden(self->tree, sfe_config()->show_hidden);
    rebuild(self);
}

static void on_devices_changed(SfeDevices* devices, gpointer data)
{
    (void)devices;
    rebuild(SFE_SIDEBAR(data));
}

static void on_tree_open(SfeDirTree* tree, GFile* dir, gpointer data)
{
    (void)tree;
    open_location(SFE_SIDEBAR(data), dir, false);
}

/* Starts with the divider in the middle; after that the user decides. */
static void on_size_allocate(GtkWidget* widget, GdkRectangle* alloc, gpointer data)
{
    (void)data;
    SfeSidebar* self = SFE_SIDEBAR(widget);
    if (!self->positioned && alloc->height > 1) {
        self->positioned = true;
        gtk_paned_set_position(GTK_PANED(self), alloc->height / 2);
    }
}

/* ------------------------------------------------------------------------ */
/* GObject                                                                   */
/* ------------------------------------------------------------------------ */

static void sfe_sidebar_dispose(GObject* object)
{
    SfeSidebar* self = SFE_SIDEBAR(object);

    if (self->config_handler != 0) {
        g_signal_handler_disconnect(sfe_app(), self->config_handler);
        self->config_handler = 0;
    }
    if (self->devices_handler != 0) {
        g_signal_handler_disconnect(sfe_devices_get_default(), self->devices_handler);
        self->devices_handler = 0;
    }
    g_clear_object(&self->location);
    G_OBJECT_CLASS(sfe_sidebar_parent_class)->dispose(object);
}

static void sfe_sidebar_class_init(SfeSidebarClass* klass)
{
    G_OBJECT_CLASS(klass)->dispose = sfe_sidebar_dispose;
    signals[SIGNAL_OPEN_LOCATION] = g_signal_new("open-location", G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_FILE, G_TYPE_BOOLEAN);
}

static GtkWidget* scrolled(GtkWidget* child)
{
    GtkWidget* sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(sw), child);
    return sw;
}

static void sfe_sidebar_init(SfeSidebar* self)
{
    gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
    gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(self)), "sfe-sidebar");

    self->list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(self->list), GTK_SELECTION_SINGLE);
    gtk_list_box_set_header_func(GTK_LIST_BOX(self->list), update_header, NULL, NULL);
    g_signal_connect(self->list, "row-activated", G_CALLBACK(on_row_activated), self);
    g_signal_connect(self->list, "button-press-event", G_CALLBACK(on_list_button_press), self);
    g_signal_connect(self->list, "popup-menu", G_CALLBACK(on_list_popup_menu), self);

    self->tree = SFE_DIR_TREE(sfe_dir_tree_new());
    sfe_dir_tree_set_show_hidden(self->tree, sfe_config()->show_hidden);
    g_signal_connect(self->tree, "open-location", G_CALLBACK(on_tree_open), self);

    GtkWidget* tree_sw = scrolled(GTK_WIDGET(self->tree));
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tree_sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_paned_pack1(GTK_PANED(self), scrolled(self->list), TRUE, FALSE);
    gtk_paned_pack2(GTK_PANED(self), tree_sw, TRUE, FALSE);
    g_signal_connect(self, "size-allocate", G_CALLBACK(on_size_allocate), NULL);

    self->config_handler = g_signal_connect(sfe_app(), "config-changed", G_CALLBACK(on_config_changed), self);
    self->devices_handler = g_signal_connect(sfe_devices_get_default(), "changed",
        G_CALLBACK(on_devices_changed), self);
    rebuild(self);
}

GtkWidget* sfe_sidebar_new(void)
{
    return g_object_new(SFE_TYPE_SIDEBAR, NULL);
}
