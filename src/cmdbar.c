#include "cmdbar.h"

#include "commands.h"
#include "fuzzy.h"
#include <string.h>

#define MAX_ROWS 200 /* candidates listed (Tab still cycles through all) */

struct _SfeCommandBar {
    GtkBox parent;

    SfeWindow* window; /* owns us */
    GtkWidget* list_scroll;
    GtkWidget* list; /* GtkListBox of candidates */
    GtkWidget* prompt;
    GtkWidget* entry;
    GtkWidget* message; /* errors, in place of the entry */

    GPtrArray* candidates; /* SfeCandidate*, for the text in `base` */
    char* base; /* the text the candidates were computed for */
    char* rank; /* what the candidates were matched against (highlighting) */
    int start, end; /* the part of `base` a candidate replaces (bytes) */
    int selected; /* the candidate inserted by Tab, or -1 */
    bool inserting; /* our own change of the entry text */
    guint refresh_source; /* a refresh after typing */
    bool active;
    bool showing_error;
};

G_DEFINE_TYPE(SfeCommandBar, sfe_command_bar, GTK_TYPE_BOX)

enum {
    SIGNAL_DONE,
    N_SIGNALS,
};
static guint signals[N_SIGNALS];

/* The session's command history, shared by all windows. */
static GPtrArray* history;
static guint history_pos;
static char* history_draft;

/* ------------------------------------------------------------------------ */
/* Completion list                                                           */
/* ------------------------------------------------------------------------ */

/* Where `c->match` sits in `text`: the chosen name in a "cp | copy" label,
 * else its last occurrence (a file name at the end of a path); -1 if absent. */
static int match_offset(const SfeCandidate* c, const char* text)
{
    if (c->names != NULL && text == c->label) {
        int offset = 0;
        for (int k = 0; c->names[k] != NULL; k++) {
            if (strcmp(c->names[k], c->insert) == 0)
                return offset;
            offset += (int)strlen(c->names[k]) + 3; /* " | " */
        }
        return -1;
    }
    const char* at = g_strrstr(text, c->match);
    return at ? (int)(at - text) : -1;
}

/* `text` as markup with the letters that matched `query` in the accent
 * colour (bold and underlined in the selected row, which is accent itself). */
static char* highlight(const SfeCandidate* c, const char* text, const char* query, const GdkRGBA* accent,
    bool selected)
{
    int offset = query && *query ? match_offset(c, text) : -1;
    g_autoptr(GArray) at = g_array_new(FALSE, FALSE, sizeof(int));
    if (offset >= 0)
        sfe_fuzzy_match(query, c->match, at);
    g_autofree char* color = g_strdup_printf("#%02x%02x%02x", (int)(accent->red * 255 + 0.5),
        (int)(accent->green * 255 + 0.5), (int)(accent->blue * 255 + 0.5)); /* Pango wants #rrggbb */
    GString* out = g_string_new(NULL);
    guint k = 0;
    for (const char* p = text; *p != '\0'; p = g_utf8_next_char(p)) {
        bool hit = k < at->len && g_array_index(at, int, k) + offset == (int)(p - text);
        g_autofree char* ch = g_markup_escape_text(p, g_utf8_next_char(p) - p);
        if (!hit) {
            g_string_append(out, ch);
            continue;
        }
        k++;
        if (selected)
            g_string_append_printf(out, "<b><u>%s</u></b>", ch);
        else
            g_string_append_printf(out, "<span foreground=\"%s\"><b>%s</b></span>", color, ch);
    }
    return g_string_free(out, FALSE);
}

/* Fills a row's labels for its candidate (again when the selection moves). */
static void mark_row(SfeCommandBar* self, GtkListBoxRow* row, bool selected)
{
    const SfeCandidate* c = g_object_get_data(G_OBJECT(row), "sfe-candidate");
    GtkWidget* text = g_object_get_data(G_OBJECT(row), "sfe-text");
    GtkWidget* tag = g_object_get_data(G_OBJECT(row), "sfe-tag");
    GdkRGBA accent = { 0.3, 0.5, 0.9, 1 };
    gtk_style_context_lookup_color(gtk_widget_get_style_context(GTK_WIDGET(self)), "sfe_accent", &accent);

    const char* shown = c->label ? c->label : c->insert;
    bool in_text = match_offset(c, shown) >= 0;
    g_autofree char* markup = highlight(c, shown, in_text ? self->rank : NULL, &accent, selected);
    gtk_label_set_markup(GTK_LABEL(text), markup);
    if (tag != NULL) { /* the undo history matches its descriptions */
        g_autofree char* tag_markup = highlight(c, c->tag, in_text ? NULL : self->rank, &accent, selected);
        gtk_label_set_markup(GTK_LABEL(tag), tag_markup);
    }
}

static GtkWidget* candidate_row(SfeCommandBar* self, SfeCandidate* c)
{
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget* text = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(text), 0);
    gtk_label_set_ellipsize(GTK_LABEL(text), PANGO_ELLIPSIZE_START);
    gtk_box_pack_start(GTK_BOX(box), text, c->args == NULL, TRUE, 0);
    if (c->args != NULL) { /* "mkdir | new_directory  [<name>…]" */
        GtkWidget* args = gtk_label_new(c->args);
        gtk_label_set_xalign(GTK_LABEL(args), 0);
        gtk_label_set_ellipsize(GTK_LABEL(args), PANGO_ELLIPSIZE_END);
        gtk_style_context_add_class(gtk_widget_get_style_context(args), "sfe-cmdbar-args");
        gtk_box_pack_start(GTK_BOX(box), args, TRUE, TRUE, 0);
    }
    GtkWidget* tag = NULL;
    if (c->tag != NULL) {
        tag = gtk_label_new(NULL);
        gtk_style_context_add_class(gtk_widget_get_style_context(tag), "sfe-cmdbar-tag");
        gtk_label_set_ellipsize(GTK_LABEL(tag), PANGO_ELLIPSIZE_MIDDLE); /* long undo descriptions */
        gtk_box_pack_end(GTK_BOX(box), tag, FALSE, FALSE, 0);
    }
    GtkWidget* row = gtk_list_box_row_new();
    if (c->dim)
        gtk_style_context_add_class(gtk_widget_get_style_context(row), "sfe-cmdbar-dim");
    gtk_container_add(GTK_CONTAINER(row), box);
    g_object_set_data(G_OBJECT(row), "sfe-candidate", c);
    g_object_set_data(G_OBJECT(row), "sfe-text", text);
    g_object_set_data(G_OBJECT(row), "sfe-tag", tag);
    mark_row(self, GTK_LIST_BOX_ROW(row), false);
    return row;
}

static void on_row_selected(GtkListBox* list, GtkListBoxRow* selected, gpointer data)
{
    g_autoptr(GList) rows = gtk_container_get_children(GTK_CONTAINER(list));
    for (GList* l = rows; l != NULL; l = l->next)
        mark_row(data, l->data, l->data == (gpointer)selected);
}

static void fill_list(SfeCommandBar* self)
{
    GList* rows = gtk_container_get_children(GTK_CONTAINER(self->list));
    for (GList* l = rows; l != NULL; l = l->next)
        gtk_widget_destroy(l->data);
    g_list_free(rows);

    guint n = self->candidates ? MIN(self->candidates->len, MAX_ROWS) : 0;
    for (guint i = 0; i < n; i++)
        gtk_container_add(GTK_CONTAINER(self->list), candidate_row(self, g_ptr_array_index(self->candidates, i)));
    gtk_widget_show_all(self->list);
    gtk_widget_set_visible(self->list_scroll, n > 0);
}

/* Recomputes the candidates for the text and cursor in the entry. */
static void refresh(SfeCommandBar* self)
{
    g_clear_handle_id(&self->refresh_source, g_source_remove);
    const char* text = gtk_entry_get_text(GTK_ENTRY(self->entry));
    int pos = gtk_editable_get_position(GTK_EDITABLE(self->entry));
    int cursor = (int)(g_utf8_offset_to_pointer(text, pos) - text);

    g_clear_pointer(&self->candidates, g_ptr_array_unref);
    g_free(self->base);
    self->base = g_strdup(text);
    g_clear_pointer(&self->rank, g_free);
    self->candidates = sfe_command_complete(self->window, text, cursor, &self->start, &self->end, &self->rank);
    self->selected = -1;
    fill_list(self);
}

static void scroll_to_row(SfeCommandBar* self, GtkWidget* row)
{
    GtkAdjustment* adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self->list_scroll));
    GtkAllocation a;
    gtk_widget_get_allocation(row, &a);
    double top = gtk_adjustment_get_value(adj);
    double page = gtk_adjustment_get_page_size(adj);
    if (a.y < top)
        gtk_adjustment_set_value(adj, a.y);
    else if (a.y + a.height > top + page)
        gtk_adjustment_set_value(adj, a.y + a.height - page);
}

/* Puts candidate `i` in place of the argument it completes. */
static void insert_candidate(SfeCommandBar* self, int i)
{
    const SfeCandidate* c = g_ptr_array_index(self->candidates, i);
    g_autofree char* quoted = sfe_cmdline_quote(c->insert);
    g_autofree char* head = g_strndup(self->base, self->start);
    g_autofree char* text = g_strconcat(head, quoted, self->base + self->end, NULL);

    self->inserting = true;
    gtk_entry_set_text(GTK_ENTRY(self->entry), text);
    gtk_editable_set_position(GTK_EDITABLE(self->entry), (int)g_utf8_strlen(text, self->start + (int)strlen(quoted)));
    self->inserting = false;

    self->selected = i;
    GtkListBoxRow* row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(self->list), i);
    if (row != NULL) {
        gtk_list_box_select_row(GTK_LIST_BOX(self->list), row);
        scroll_to_row(self, GTK_WIDGET(row));
    }
}

/* Tab / Shift+Tab. A single candidate is taken right away and the list
 * recomputed, so Tab after "Projects/" goes on inside Projects. */
static void cycle(SfeCommandBar* self, int step)
{
    if (self->candidates == NULL)
        refresh(self);
    int n = (int)self->candidates->len;
    if (n == 0) {
        gtk_widget_error_bell(self->entry);
        return;
    }
    if (n == 1) {
        insert_candidate(self, 0);
        refresh(self);
        return;
    }
    int next = self->selected < 0 ? (step > 0 ? 0 : n - 1) : (self->selected + step + n) % n;
    insert_candidate(self, next);
}

static void on_row_activated(GtkListBox* list, GtkListBoxRow* row, gpointer data)
{
    (void)list;
    SfeCommandBar* self = data;
    insert_candidate(self, gtk_list_box_row_get_index(row));
    refresh(self);
    gtk_widget_grab_focus(self->entry);
    gtk_editable_set_position(GTK_EDITABLE(self->entry), -1);
}

/* ------------------------------------------------------------------------ */
/* History                                                                   */
/* ------------------------------------------------------------------------ */

static void remember(const char* line)
{
    if (history == NULL)
        history = g_ptr_array_new_with_free_func(g_free);
    if (history->len == 0 || strcmp(g_ptr_array_index(history, history->len - 1), line) != 0)
        g_ptr_array_add(history, g_strdup(line));
}

static void walk_history(SfeCommandBar* self, int step)
{
    if (history == NULL || history->len == 0)
        return;
    if (history_pos == history->len) { /* leaving the line being typed */
        g_free(history_draft);
        history_draft = g_strdup(gtk_entry_get_text(GTK_ENTRY(self->entry)));
    }
    int pos = (int)history_pos + step;
    if (pos < 0 || pos > (int)history->len)
        return;
    history_pos = (guint)pos;
    const char* text = history_pos == history->len ? history_draft : g_ptr_array_index(history, history_pos);
    gtk_entry_set_text(GTK_ENTRY(self->entry), text ? text : "");
    gtk_editable_set_position(GTK_EDITABLE(self->entry), -1);
}

/* ------------------------------------------------------------------------ */
/* Opening, running, closing                                                 */
/* ------------------------------------------------------------------------ */

static void close_bar(SfeCommandBar* self)
{
    self->active = false;
    self->showing_error = false;
    g_clear_handle_id(&self->refresh_source, g_source_remove);
    g_clear_pointer(&self->candidates, g_ptr_array_unref);
    fill_list(self);
    gtk_widget_hide(GTK_WIDGET(self));
    g_signal_emit(self, signals[SIGNAL_DONE], 0);
}

static void on_activate(GtkEntry* entry, gpointer data)
{
    SfeCommandBar* self = data;
    g_autofree char* line = g_strdup(gtk_entry_get_text(entry));
    g_autoptr(GError) error = NULL;
    g_autoptr(SfeWindow) window = g_object_ref(self->window); /* the command may close it */
    g_autoptr(SfeCommandBar) keep = g_object_ref(self);

    g_strstrip(line);
    if (*line != '\0')
        remember(line);
    close_bar(self);
    if (*line != '\0' && !sfe_command_run(window, line, &error))
        sfe_command_bar_show_error(self, error->message);
}

static gboolean on_entry_key(GtkWidget* entry, GdkEventKey* event, gpointer data)
{
    (void)entry;
    SfeCommandBar* self = data;
    switch (event->keyval) {
    case GDK_KEY_Tab:
    case GDK_KEY_KP_Tab:
        cycle(self, (event->state & GDK_SHIFT_MASK) ? -1 : 1);
        return TRUE;
    case GDK_KEY_ISO_Left_Tab:
        cycle(self, -1);
        return TRUE;
    case GDK_KEY_Up:
        walk_history(self, -1);
        return TRUE;
    case GDK_KEY_Down:
        walk_history(self, 1);
        return TRUE;
    case GDK_KEY_Escape:
        close_bar(self);
        return TRUE;
    default:
        return FALSE;
    }
}

static gboolean refresh_later(gpointer data)
{
    SfeCommandBar* self = data;
    self->refresh_source = 0;
    refresh(self);
    return G_SOURCE_REMOVE;
}

/* "changed" comes before the entry moves its cursor past the new text:
 * completing then would look at the wrong argument ("cd|" instead of
 * "cd |"), so the list is recomputed once the entry has settled. */
static void on_changed(GtkEditable* editable, gpointer data)
{
    (void)editable;
    SfeCommandBar* self = data;
    if (!self->inserting && self->refresh_source == 0)
        self->refresh_source = g_idle_add(refresh_later, self);
}

void sfe_command_bar_start(SfeCommandBar* self, const char* text)
{
    self->active = true;
    self->showing_error = false;
    gtk_style_context_remove_class(gtk_widget_get_style_context(GTK_WIDGET(self)), "sfe-cmdbar-failed");
    gtk_widget_hide(self->message);
    gtk_widget_show(self->prompt);
    gtk_widget_show(self->entry);
    gtk_widget_show(GTK_WIDGET(self));
    history_pos = history ? history->len : 0;

    self->inserting = true;
    gtk_entry_set_text(GTK_ENTRY(self->entry), text ? text : "");
    self->inserting = false;
    gtk_widget_grab_focus(self->entry);
    gtk_editable_set_position(GTK_EDITABLE(self->entry), -1);
    refresh(self);
}

bool sfe_command_bar_is_active(SfeCommandBar* self)
{
    return self->active;
}

static void show_text(SfeCommandBar* self, const char* message, bool failed)
{
    self->active = false;
    self->showing_error = true;
    gtk_label_set_text(GTK_LABEL(self->message), message);
    gtk_widget_hide(self->prompt);
    gtk_widget_hide(self->entry);
    gtk_widget_hide(self->list_scroll);
    gtk_widget_show(self->message);
    if (failed)
        gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(self)), "sfe-cmdbar-failed");
    else
        gtk_style_context_remove_class(gtk_widget_get_style_context(GTK_WIDGET(self)), "sfe-cmdbar-failed");
    gtk_widget_show(GTK_WIDGET(self));
}

void sfe_command_bar_show_error(SfeCommandBar* self, const char* message)
{
    show_text(self, message, true);
}

void sfe_command_bar_show_message(SfeCommandBar* self, const char* message)
{
    show_text(self, message, false);
}

void sfe_command_bar_dismiss(SfeCommandBar* self)
{
    if (!self->showing_error)
        return;
    self->showing_error = false;
    gtk_widget_hide(GTK_WIDGET(self));
}

/* ------------------------------------------------------------------------ */
/* GObject                                                                   */
/* ------------------------------------------------------------------------ */

static void sfe_command_bar_finalize(GObject* object)
{
    SfeCommandBar* self = SFE_COMMAND_BAR(object);
    g_clear_handle_id(&self->refresh_source, g_source_remove);
    g_clear_pointer(&self->candidates, g_ptr_array_unref);
    g_free(self->base);
    g_free(self->rank);
    G_OBJECT_CLASS(sfe_command_bar_parent_class)->finalize(object);
}

static void sfe_command_bar_class_init(SfeCommandBarClass* klass)
{
    G_OBJECT_CLASS(klass)->finalize = sfe_command_bar_finalize;
    signals[SIGNAL_DONE] = g_signal_new("done", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
        G_TYPE_NONE, 0);
}

static void sfe_command_bar_init(SfeCommandBar* self)
{
    gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
    gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(self)), "sfe-cmdbar");
    gtk_widget_set_valign(GTK_WIDGET(self), GTK_ALIGN_END);
    gtk_widget_set_no_show_all(GTK_WIDGET(self), TRUE);

    self->list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(self->list), GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(self->list), TRUE);
    g_signal_connect(self->list, "row-activated", G_CALLBACK(on_row_activated), self);
    g_signal_connect(self->list, "row-selected", G_CALLBACK(on_row_selected), self);
    self->list_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(self->list_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(self->list_scroll), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(self->list_scroll), 260);
    gtk_container_add(GTK_CONTAINER(self->list_scroll), self->list);
    gtk_style_context_add_class(gtk_widget_get_style_context(self->list_scroll), "sfe-cmdbar-list");
    gtk_widget_set_no_show_all(self->list_scroll, TRUE);
    gtk_widget_show(self->list);

    self->prompt = gtk_label_new(":");
    gtk_style_context_add_class(gtk_widget_get_style_context(self->prompt), "sfe-cmdbar-prompt");
    self->entry = gtk_entry_new();
    gtk_entry_set_has_frame(GTK_ENTRY(self->entry), FALSE);
    gtk_widget_set_hexpand(self->entry, TRUE);
    g_signal_connect(self->entry, "activate", G_CALLBACK(on_activate), self);
    g_signal_connect(self->entry, "changed", G_CALLBACK(on_changed), self);
    g_signal_connect(self->entry, "key-press-event", G_CALLBACK(on_entry_key), self);
    self->message = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(self->message), 0);
    gtk_label_set_ellipsize(GTK_LABEL(self->message), PANGO_ELLIPSIZE_END);
    gtk_style_context_add_class(gtk_widget_get_style_context(self->message), "sfe-cmdbar-message");
    gtk_widget_set_no_show_all(self->message, TRUE);

    GtkWidget* line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_style_context_add_class(gtk_widget_get_style_context(line), "sfe-cmdbar-line");
    gtk_box_pack_start(GTK_BOX(line), self->prompt, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(line), self->entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(line), self->message, TRUE, TRUE, 0);
    gtk_widget_show(self->prompt);
    gtk_widget_show(self->entry);
    gtk_widget_show(line);

    gtk_box_pack_start(GTK_BOX(self), self->list_scroll, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(self), line, FALSE, FALSE, 0);
}

SfeCommandBar* sfe_command_bar_new(SfeWindow* window)
{
    SfeCommandBar* self = g_object_new(SFE_TYPE_COMMAND_BAR, NULL);
    self->window = window;
    return self;
}
