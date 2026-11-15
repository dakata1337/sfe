#include "split.h"

#define MAX_PANES 3

/* Below this width side-by-side views are too cramped: the views stack top
 * and bottom instead. Two thresholds so the layout cannot flicker. */
#define NARROW_BELOW 700
#define WIDE_ABOVE 780

struct _SfeSplit {
    GtkBox parent;

    /* Always packed: panes[0] is the left pane, panes[1] the right (top)
     * pane, panes[2] the right-bottom pane. Removing one and shifting the
     * rest down gives exactly the "slide left" behaviour of closing. */
    SfePane* panes[MAX_PANES];
    int n_panes;
    SfePane* active;

    GtkWidget* outer; /* horizontal: left | right column (vertical when stacked) */
    GtkWidget* inner; /* vertical: right-top / right-bottom */
    bool stacked; /* two panes split top / bottom rather than side by side */
    bool narrow; /* stacked layout for narrow windows */
    guint relayout_source;
};

G_DEFINE_TYPE(SfeSplit, sfe_split, GTK_TYPE_BOX)

enum {
    SIGNAL_ACTIVE_CHANGED,
    SIGNAL_DIRECTORY_CHANGED,
    N_SIGNALS,
};
static guint signals[N_SIGNALS];

static int index_of(SfeSplit* self, SfePane* pane)
{
    for (int i = 0; i < self->n_panes; i++) {
        if (self->panes[i] == pane)
            return i;
    }
    return -1;
}

/* ------------------------------------------------------------------------ */
/* Layout                                                                    */
/* ------------------------------------------------------------------------ */

static void detach(GtkWidget* widget)
{
    GtkWidget* parent = gtk_widget_get_parent(widget);
    if (parent != NULL)
        gtk_container_remove(GTK_CONTAINER(parent), widget);
}

#define CENTER_HANDLER_KEY "sfe-center-handler"

/* Puts the divider in the middle once the paned has its size. */
static void on_center_allocate(GtkWidget* paned, GdkRectangle* alloc, gpointer data)
{
    (void)data;
    bool horizontal = gtk_orientable_get_orientation(GTK_ORIENTABLE(paned)) == GTK_ORIENTATION_HORIZONTAL;
    int size = horizontal ? alloc->width : alloc->height;
    gulong handler = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(paned), CENTER_HANDLER_KEY));

    if (size <= 1)
        return;
    g_signal_handler_disconnect(paned, handler);
    g_object_set_data(G_OBJECT(paned), CENTER_HANDLER_KEY, NULL);
    gtk_paned_set_position(GTK_PANED(paned), size / 2);
}

static void center_divider(GtkWidget* paned)
{
    if (g_object_get_data(G_OBJECT(paned), CENTER_HANDLER_KEY) != NULL)
        return; /* already pending */
    gulong handler = g_signal_connect(paned, "size-allocate", G_CALLBACK(on_center_allocate), NULL);
    g_object_set_data(G_OBJECT(paned), CENTER_HANDLER_KEY, GSIZE_TO_POINTER(handler));
}

/* What splitting allows (any pane may split; the result is always one of
 * the layouts above):
 *   1 pane     side by side or top / bottom (narrow: top / bottom only)
 *   2 panes    side by side: top / bottom;  top / bottom: side by side
 *              (the pane split becomes right-top, the new one right-bottom)
 *   3 panes    nothing */
static bool can_split(SfeSplit* self, SfeSplitKind kind)
{
    switch (self->n_panes) {
    case 1: return kind == SFE_SPLIT_HORIZONTAL || !self->narrow;
    case 2: return self->stacked ? kind == SFE_SPLIT_VERTICAL : kind == SFE_SPLIT_HORIZONTAL;
    default: return false;
    }
}

static void update_buttons(SfeSplit* self)
{
    bool vertical = can_split(self, SFE_SPLIT_VERTICAL);
    bool horizontal = can_split(self, SFE_SPLIT_HORIZONTAL);
    for (int i = 0; i < self->n_panes; i++)
        sfe_pane_set_split_buttons(self->panes[i], vertical, horizontal, self->n_panes > 1);
}

/* The outer paned stacks for two top / bottom panes and in narrow windows. */
static bool apply_orientation(SfeSplit* self)
{
    GtkOrientation want = self->narrow || (self->n_panes == 2 && self->stacked)
        ? GTK_ORIENTATION_VERTICAL
        : GTK_ORIENTATION_HORIZONTAL;
    if (gtk_orientable_get_orientation(GTK_ORIENTABLE(self->outer)) == want)
        return false;
    gtk_orientable_set_orientation(GTK_ORIENTABLE(self->outer), want);
    return true;
}

/* Rebuilds the widget tree for the current panes. The panes (and the two
 * paneds) are kept alive by our own references while they are moved. */
static void relayout(SfeSplit* self)
{
    for (int i = 0; i < self->n_panes; i++)
        detach(GTK_WIDGET(self->panes[i]));
    detach(self->inner);
    detach(self->outer);

    switch (self->n_panes) {
    case 1:
        gtk_box_pack_start(GTK_BOX(self), GTK_WIDGET(self->panes[0]), TRUE, TRUE, 0);
        break;
    case 2:
        gtk_paned_pack1(GTK_PANED(self->outer), GTK_WIDGET(self->panes[0]), TRUE, FALSE);
        gtk_paned_pack2(GTK_PANED(self->outer), GTK_WIDGET(self->panes[1]), TRUE, FALSE);
        gtk_box_pack_start(GTK_BOX(self), self->outer, TRUE, TRUE, 0);
        break;
    case 3:
        gtk_paned_pack1(GTK_PANED(self->inner), GTK_WIDGET(self->panes[1]), TRUE, FALSE);
        gtk_paned_pack2(GTK_PANED(self->inner), GTK_WIDGET(self->panes[2]), TRUE, FALSE);
        gtk_paned_pack1(GTK_PANED(self->outer), GTK_WIDGET(self->panes[0]), TRUE, FALSE);
        gtk_paned_pack2(GTK_PANED(self->outer), self->inner, TRUE, FALSE);
        gtk_box_pack_start(GTK_BOX(self), self->outer, TRUE, TRUE, 0);
        break;
    }

    if (self->n_panes == 1)
        self->stacked = false;
    if (apply_orientation(self) && self->n_panes > 1)
        center_divider(self->outer);

    GtkStyleContext* style = gtk_widget_get_style_context(GTK_WIDGET(self));
    if (self->n_panes > 1)
        gtk_style_context_add_class(style, "sfe-multi");
    else
        gtk_style_context_remove_class(style, "sfe-multi");
    update_buttons(self);
}

/* ------------------------------------------------------------------------ */
/* Active pane                                                               */
/* ------------------------------------------------------------------------ */

static void set_active(SfeSplit* self, SfePane* pane)
{
    if (pane == self->active)
        return;
    if (self->active != NULL)
        sfe_pane_set_active(self->active, false);
    self->active = pane;
    sfe_pane_set_active(pane, true);
    g_signal_emit(self, signals[SIGNAL_ACTIVE_CHANGED], 0);
}

SfePane* sfe_split_get_active(SfeSplit* self)
{
    return self->active;
}

int sfe_split_get_n_panes(SfeSplit* self)
{
    return self->n_panes;
}

SfePane* sfe_split_get_pane(SfeSplit* self, int index)
{
    return index >= 0 && index < self->n_panes ? self->panes[index] : NULL;
}

void sfe_split_track_focus(SfeSplit* self, GtkWidget* widget)
{
    GtkWidget* pane = widget ? gtk_widget_get_ancestor(widget, SFE_TYPE_PANE) : NULL;
    if (pane != NULL && index_of(self, SFE_PANE(pane)) >= 0)
        set_active(self, SFE_PANE(pane));
}

void sfe_split_focus_next(SfeSplit* self)
{
    int next = (index_of(self, self->active) + 1) % self->n_panes;
    set_active(self, self->panes[next]);
    sfe_pane_focus_view(self->panes[next]);
}

void sfe_split_focus(SfeSplit* self, SfePane* pane)
{
    if (index_of(self, pane) < 0)
        return;
    set_active(self, pane);
    sfe_pane_focus_view(pane);
}

/* The pane's rectangle in the split's coordinates. */
static GdkRectangle pane_rect(SfeSplit* self, SfePane* pane)
{
    GtkAllocation a;
    GdkRectangle r = { 0, 0, 0, 0 };
    gtk_widget_get_allocation(GTK_WIDGET(pane), &a);
    gtk_widget_translate_coordinates(GTK_WIDGET(pane), GTK_WIDGET(self), 0, 0, &r.x, &r.y);
    r.width = a.width;
    r.height = a.height;
    return r;
}

static int overlap(int a0, int a1, int b0, int b1)
{
    return MIN(a1, b1) - MAX(a0, b0);
}

SfePane* sfe_split_neighbor(SfeSplit* self, SfePane* pane, SfeDirection direction)
{
    if (index_of(self, pane) < 0)
        return NULL;
    GdkRectangle from = pane_rect(self, pane);
    SfePane* best = NULL;
    int best_distance = G_MAXINT;

    /* Beside it in that direction (paned handles leave a small gap) and
     * overlapping it across; the nearest wins, the first pane on a tie. */
    for (int i = 0; i < self->n_panes; i++) {
        SfePane* other = self->panes[i];
        if (other == pane)
            continue;
        GdkRectangle to = pane_rect(self, other);
        int distance;
        bool across;
        switch (direction) {
        case SFE_DIRECTION_LEFT:
            distance = from.x - (to.x + to.width);
            across = overlap(from.y, from.y + from.height, to.y, to.y + to.height) > 0;
            break;
        case SFE_DIRECTION_RIGHT:
            distance = to.x - (from.x + from.width);
            across = overlap(from.y, from.y + from.height, to.y, to.y + to.height) > 0;
            break;
        case SFE_DIRECTION_UP:
            distance = from.y - (to.y + to.height);
            across = overlap(from.x, from.x + from.width, to.x, to.x + to.width) > 0;
            break;
        default:
            distance = to.y - (from.y + from.height);
            across = overlap(from.x, from.x + from.width, to.x, to.x + to.width) > 0;
            break;
        }
        if (across && distance > -8 && distance < best_distance) {
            best = other;
            best_distance = distance;
        }
    }
    return best;
}

bool sfe_split_swap(SfeSplit* self, SfeDirection direction)
{
    SfePane* other = sfe_split_neighbor(self, self->active, direction);
    if (other == NULL)
        return false;
    int a = index_of(self, self->active);
    int b = index_of(self, other);
    self->panes[a] = other;
    self->panes[b] = self->active;
    relayout(self);
    sfe_pane_focus_view(self->active);
    return true;
}

const char* sfe_split_position_name(SfeSplit* self, int index)
{
    static const char* const one[] = { "left" };
    static const char* const side[] = { "left", "right" };
    static const char* const stacked[] = { "top", "bottom" };
    static const char* const three[] = { "left", "right-top", "right-bottom" };

    if (index < 0 || index >= self->n_panes)
        return NULL;
    switch (self->n_panes) {
    case 1: return one[index];
    case 2: return self->stacked ? stacked[index] : side[index];
    default: return three[index];
    }
}

SfePane* sfe_split_find_position(SfeSplit* self, const char* name)
{
    for (int i = 0; i < self->n_panes; i++) {
        if (g_strcmp0(sfe_split_position_name(self, i), name) == 0)
            return self->panes[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* Adding and removing panes                                                 */
/* ------------------------------------------------------------------------ */

static void on_pane_split(SfePane* pane, int kind, gpointer data)
{
    sfe_split_split(SFE_SPLIT(data), pane, kind);
}

static void on_pane_close(SfePane* pane, gpointer data)
{
    sfe_split_close(SFE_SPLIT(data), pane);
}

static void on_pane_directory_changed(SfePane* pane, gpointer data)
{
    g_signal_emit(data, signals[SIGNAL_DIRECTORY_CHANGED], 0, pane);
}

static SfePane* add_pane(SfeSplit* self, GFile* dir)
{
    SfePane* pane = g_object_ref_sink(sfe_pane_new(dir));

    g_signal_connect(pane, "split", G_CALLBACK(on_pane_split), self);
    g_signal_connect(pane, "close", G_CALLBACK(on_pane_close), self);
    g_signal_connect(pane, "directory-changed", G_CALLBACK(on_pane_directory_changed), self);
    self->panes[self->n_panes++] = pane;
    return pane;
}

/* Adds a pane showing `dir`, in the view mode of `like`. Two panes then sit
 * side by side, or top / bottom when `stacked`; with three, `right_top`
 * moves to right-top, the other pane to the left and the new one goes
 * right-bottom. Makes the new pane active. */
static SfePane* insert_pane(SfeSplit* self, SfePane* like, SfePane* right_top, GFile* dir, bool stacked)
{
    SfePane* added = add_pane(self, dir);

    sfe_pane_set_view(added, sfe_pane_get_view(like));
    if (self->n_panes == 2) {
        self->stacked = stacked;
    } else {
        SfePane* other = self->panes[0] == right_top ? self->panes[1] : self->panes[0];
        self->panes[0] = other;
        self->panes[1] = right_top;
        self->stacked = false;
    }
    relayout(self);
    center_divider(self->n_panes == 2 ? self->outer : self->inner);

    set_active(self, added);
    sfe_pane_focus_view(added);
    return added;
}

void sfe_split_split(SfeSplit* self, SfePane* pane, SfeSplitKind kind)
{
    if (index_of(self, pane) < 0 || !can_split(self, kind))
        return;
    /* The new pane opens the same directory. */
    insert_pane(self, pane, pane, sfe_pane_get_dir(pane), kind == SFE_SPLIT_HORIZONTAL);
}

SfePane* sfe_split_open(SfeSplit* self, GFile* dir)
{
    switch (self->n_panes) {
    case 1: /* a new right pane */
        return insert_pane(self, self->panes[0], NULL, dir, false);
    case 2: /* a new right-bottom pane under the right (or bottom) one */
        return insert_pane(self, self->active, self->panes[1], dir, false);
    default: /* the right-bottom pane goes there */
        sfe_pane_navigate(self->panes[2], dir);
        set_active(self, self->panes[2]);
        sfe_pane_focus_view(self->panes[2]);
        return self->panes[2];
    }
}

void sfe_split_close(SfeSplit* self, SfePane* pane)
{
    int index = index_of(self, pane);
    if (index < 0 || self->n_panes == 1)
        return;

    g_signal_handlers_disconnect_by_data(pane, self);
    detach(GTK_WIDGET(pane));
    for (int i = index; i < self->n_panes - 1; i++)
        self->panes[i] = self->panes[i + 1];
    self->panes[--self->n_panes] = NULL;
    relayout(self);

    if (self->active == pane) {
        self->active = NULL;
        set_active(self, self->panes[MIN(index, self->n_panes - 1)]);
    }
    sfe_pane_focus_view(self->active);
    gtk_widget_destroy(GTK_WIDGET(pane));
    g_object_unref(pane);
}

/* ------------------------------------------------------------------------ */
/* Narrow windows                                                            */
/* ------------------------------------------------------------------------ */

static gboolean apply_narrow(gpointer data)
{
    SfeSplit* self = data;
    self->relayout_source = 0;
    if (apply_orientation(self) && self->n_panes > 1)
        center_divider(self->outer);
    update_buttons(self);
    return G_SOURCE_REMOVE;
}

static void on_size_allocate(GtkWidget* widget, GdkRectangle* alloc, gpointer data)
{
    (void)data;
    SfeSplit* self = SFE_SPLIT(widget);
    bool narrow = self->narrow ? alloc->width < WIDE_ABOVE : alloc->width < NARROW_BELOW;

    if (narrow == self->narrow)
        return;
    self->narrow = narrow;
    /* Changing the layout while being allocated is not allowed: next idle. */
    if (self->relayout_source == 0)
        self->relayout_source = g_idle_add(apply_narrow, self);
}

/* ------------------------------------------------------------------------ */
/* GObject                                                                   */
/* ------------------------------------------------------------------------ */

static void sfe_split_dispose(GObject* object)
{
    SfeSplit* self = SFE_SPLIT(object);

    g_clear_handle_id(&self->relayout_source, g_source_remove);
    for (int i = 0; i < self->n_panes; i++) {
        g_signal_handlers_disconnect_by_data(self->panes[i], self);
        g_clear_object(&self->panes[i]);
    }
    self->n_panes = 0;
    self->active = NULL;
    g_clear_object(&self->inner);
    g_clear_object(&self->outer);
    G_OBJECT_CLASS(sfe_split_parent_class)->dispose(object);
}

static void sfe_split_class_init(SfeSplitClass* klass)
{
    G_OBJECT_CLASS(klass)->dispose = sfe_split_dispose;

    signals[SIGNAL_ACTIVE_CHANGED] = g_signal_new("active-changed", G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    signals[SIGNAL_DIRECTORY_CHANGED] = g_signal_new("directory-changed", G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, SFE_TYPE_PANE);
}

static void sfe_split_init(SfeSplit* self)
{
    gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(self)), "sfe-split");
    self->outer = g_object_ref_sink(gtk_paned_new(GTK_ORIENTATION_HORIZONTAL));
    self->inner = g_object_ref_sink(gtk_paned_new(GTK_ORIENTATION_VERTICAL));
    gtk_widget_show(self->outer);
    gtk_widget_show(self->inner);
    g_signal_connect(self, "size-allocate", G_CALLBACK(on_size_allocate), NULL);
}

SfeSplit* sfe_split_new(GFile* dir)
{
    SfeSplit* self = g_object_new(SFE_TYPE_SPLIT, NULL);
    set_active(self, add_pane(self, dir));
    relayout(self);
    return self;
}
