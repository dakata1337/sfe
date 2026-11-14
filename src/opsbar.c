#include "opsbar.h"

#include "dialogs.h"

#define SHOW_DELAY_MS 400 /* quick jobs never show the bar */

struct _SfeOpsBar {
    GtkRevealer parent;
    GtkWidget* rows; /* GtkBox of job rows */
    GList* jobs; /* Job*, running */
};

G_DEFINE_TYPE(SfeOpsBar, sfe_ops_bar, GTK_TYPE_REVEALER)

/* One row per running job. */
typedef struct {
    SfeOpsBar* bar;
    SfeFileOp* op;
    GtkWidget* row;
    GtkWidget* label;
    GtkWidget* progress;
    guint show_source;
} Job;

static const char* verb_for(SfeOpKind kind)
{
    switch (kind) {
    case SFE_OP_COPY: return "copy";
    case SFE_OP_MOVE: return "move";
    case SFE_OP_TRASH: return "move to the trash";
    case SFE_OP_DELETE: return "delete";
    }
    return "process";
}

static void update_visibility(SfeOpsBar* self)
{
    bool any_visible = false;
    GList* rows = gtk_container_get_children(GTK_CONTAINER(self->rows));
    for (GList* l = rows; l != NULL; l = l->next)
        any_visible |= gtk_widget_get_visible(l->data);
    g_list_free(rows);
    gtk_revealer_set_reveal_child(GTK_REVEALER(self), any_visible);
}

/* ------------------------------------------------------------------------ */
/* Questions                                                                 */
/* ------------------------------------------------------------------------ */

static char* describe(GFile* file)
{
    g_autoptr(GFileInfo) info = g_file_query_info(file, "standard::size,standard::type,time::modified",
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
    if (info == NULL)
        return g_strdup("");
    g_autoptr(GDateTime) utc = g_file_info_get_modification_date_time(info);
    g_autoptr(GDateTime) local = utc ? g_date_time_to_local(utc) : NULL;
    g_autofree char* when = local ? g_date_time_format(local, "%Y-%m-%d %H:%M") : g_strdup("?");
    if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY)
        return g_strdup_printf("folder, modified %s", when);
    g_autofree char* size = g_format_size(g_file_info_get_size(info));
    return g_strdup_printf("%s, modified %s", size, when);
}

static void on_question_response(GtkDialog* dialog, int response, gpointer data)
{
    SfeFileOp* op = data;
    GtkWidget* all = g_object_get_data(G_OBJECT(dialog), "sfe-all");
    bool apply = all && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(all));

    if (response < 0) /* closed */
        response = SFE_ANSWER_CANCEL;
    gtk_widget_destroy(GTK_WIDGET(dialog));
    sfe_file_op_answer(op, response, apply);
    g_object_unref(op);
}

static void on_question(SfeFileOp* op, int kind, GFile* src, GFile* dest, const char* error, gpointer data)
{
    Job* job = data;
    g_autofree char* name = g_file_get_basename(kind == SFE_QUESTION_CONFLICT ? dest : src);
    g_autofree char* title = NULL;
    g_autofree char* detail = NULL;
    GtkWidget* dialog;

    if (kind == SFE_QUESTION_CONFLICT) {
        g_autofree char* old_desc = describe(dest);
        g_autofree char* new_desc = describe(src);
        title = g_strdup_printf("“%s” already exists", name);
        detail = g_strdup_printf("Existing: %s\nNew: %s", old_desc, new_desc);
    } else {
        title = g_strdup_printf("Cannot %s “%s”", verb_for(sfe_file_op_get_kind(op)), name);
        detail = g_strdup(error);
    }

    dialog = gtk_message_dialog_new(GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(job->bar))),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        kind == SFE_QUESTION_CONFLICT ? GTK_MESSAGE_QUESTION : GTK_MESSAGE_ERROR, GTK_BUTTONS_NONE, "%s", title);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", detail);
    gtk_style_context_add_class(gtk_widget_get_style_context(dialog), "sfe-window");
    gtk_style_context_add_class(gtk_widget_get_style_context(dialog), "sfe-dialog");

    gtk_dialog_add_button(GTK_DIALOG(dialog), "Cancel", SFE_ANSWER_CANCEL);
    gtk_dialog_add_button(GTK_DIALOG(dialog), "Skip", SFE_ANSWER_SKIP);
    if (kind == SFE_QUESTION_CONFLICT) {
        gtk_dialog_add_button(GTK_DIALOG(dialog), "Keep Both", SFE_ANSWER_RENAME);
        GtkWidget* replace = gtk_dialog_add_button(GTK_DIALOG(dialog), "Replace", SFE_ANSWER_REPLACE);
        gtk_style_context_add_class(gtk_widget_get_style_context(replace), "destructive-action");
        gtk_dialog_set_default_response(GTK_DIALOG(dialog), SFE_ANSWER_RENAME);
    } else {
        GtkWidget* retry = gtk_dialog_add_button(GTK_DIALOG(dialog), "Retry", SFE_ANSWER_RETRY);
        gtk_style_context_add_class(gtk_widget_get_style_context(retry), "suggested-action");
        gtk_dialog_set_default_response(GTK_DIALOG(dialog), SFE_ANSWER_RETRY);
    }

    GtkWidget* all = gtk_check_button_new_with_label(
        kind == SFE_QUESTION_CONFLICT ? "Do this for all conflicts" : "Do this for all errors");
    gtk_box_pack_start(GTK_BOX(gtk_message_dialog_get_message_area(GTK_MESSAGE_DIALOG(dialog))), all, FALSE, FALSE, 0);
    g_object_set_data(G_OBJECT(dialog), "sfe-all", all);

    g_signal_connect(dialog, "response", G_CALLBACK(on_question_response), g_object_ref(op));
    gtk_widget_show_all(dialog);
}

/* ------------------------------------------------------------------------ */
/* Rows                                                                      */
/* ------------------------------------------------------------------------ */

static void on_progress(SfeFileOp* op, gpointer data)
{
    Job* job = data;
    g_autofree char* status = sfe_file_op_get_status(op);
    gtk_label_set_text(GTK_LABEL(job->label), status);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(job->progress), sfe_file_op_get_fraction(op));
}

static gboolean show_row(gpointer data)
{
    Job* job = data;
    job->show_source = 0;
    gtk_widget_show(job->row);
    update_visibility(job->bar);
    return G_SOURCE_REMOVE;
}

static void on_finished(SfeFileOp* op, GError* error, gpointer data)
{
    Job* job = data;
    SfeOpsBar* self = job->bar;

    g_clear_handle_id(&job->show_source, g_source_remove);
    g_signal_handlers_disconnect_by_data(op, job);
    self->jobs = g_list_remove(self->jobs, job);
    gtk_widget_destroy(job->row);
    update_visibility(self);

    if (error != NULL && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        sfe_show_error(GTK_WIDGET(self), "The operation failed", error->message);
    g_object_unref(job->op);
    g_free(job);
}

static void on_cancel_clicked(GtkButton* button, gpointer data)
{
    (void)button;
    sfe_file_op_cancel(((Job*)data)->op);
}

void sfe_ops_bar_run(SfeOpsBar* self, SfeFileOp* op)
{
    Job* job = g_new0(Job, 1);
    GtkWidget* cancel = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_BUTTON);

    job->bar = self;
    job->op = g_object_ref(op);
    job->row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    job->label = gtk_label_new("Preparing…");
    job->progress = gtk_progress_bar_new();

    gtk_label_set_xalign(GTK_LABEL(job->label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(job->label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_size_request(job->progress, 160, -1);
    gtk_widget_set_valign(job->progress, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(cancel, "Cancel");
    g_signal_connect(cancel, "clicked", G_CALLBACK(on_cancel_clicked), job);

    gtk_box_pack_start(GTK_BOX(job->row), job->label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(job->row), job->progress, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(job->row), cancel, FALSE, FALSE, 0);
    gtk_widget_show_all(job->row);
    gtk_widget_hide(job->row);
    gtk_container_add(GTK_CONTAINER(self->rows), job->row);

    g_signal_connect(op, "progress", G_CALLBACK(on_progress), job);
    g_signal_connect(op, "question", G_CALLBACK(on_question), job);
    g_signal_connect(op, "finished", G_CALLBACK(on_finished), job);
    job->show_source = g_timeout_add(SHOW_DELAY_MS, show_row, job);
    self->jobs = g_list_prepend(self->jobs, job);
    sfe_file_op_start(op);
}

/* ------------------------------------------------------------------------ */
/* GObject                                                                   */
/* ------------------------------------------------------------------------ */

/* Closing the window stops its jobs. */
static void sfe_ops_bar_dispose(GObject* object)
{
    SfeOpsBar* self = SFE_OPS_BAR(object);

    for (GList* l = self->jobs; l != NULL; l = l->next) {
        Job* job = l->data;
        g_clear_handle_id(&job->show_source, g_source_remove);
        g_signal_handlers_disconnect_by_data(job->op, job);
        sfe_file_op_cancel(job->op);
        g_object_unref(job->op);
        g_free(job);
    }
    g_clear_pointer(&self->jobs, g_list_free);
    G_OBJECT_CLASS(sfe_ops_bar_parent_class)->dispose(object);
}

static void sfe_ops_bar_class_init(SfeOpsBarClass* klass)
{
    G_OBJECT_CLASS(klass)->dispose = sfe_ops_bar_dispose;
}

static void sfe_ops_bar_init(SfeOpsBar* self)
{
    self->rows = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_style_context_add_class(gtk_widget_get_style_context(self->rows), "sfe-opsbar");
    gtk_revealer_set_transition_type(GTK_REVEALER(self), GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
    gtk_container_add(GTK_CONTAINER(self), self->rows);
    gtk_widget_show(self->rows);
}

GtkWidget* sfe_ops_bar_new(void)
{
    return g_object_new(SFE_TYPE_OPS_BAR, NULL);
}
