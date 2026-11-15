#include "app.h"

#include "actions.h"
#include "defaultfm.h"
#include "icons.h"

#include "sfe-build.h"
#include "theme.h"
#include "window.h"

#define CONFIG_RELOAD_DELAY_MS 200

struct _SfeApplication {
    GtkApplication parent;

    SfeConfig* config;
    char* config_path;
    char* last_written; /* text of our own last save, to ignore its echo */
    GFileMonitor* config_monitor;
    guint reload_source;
};

G_DEFINE_TYPE(SfeApplication, sfe_application, GTK_TYPE_APPLICATION)

enum {
    SIGNAL_CONFIG_CHANGED,
    N_SIGNALS,
};
static guint signals[N_SIGNALS];

SfeApplication* sfe_app(void)
{
    return SFE_APPLICATION(g_application_get_default());
}

const SfeConfig* sfe_config(void)
{
    return sfe_app()->config;
}

SfeConfig* sfe_application_dup_config(SfeApplication* app)
{
    return sfe_config_copy(app->config);
}

/* Settings that are global rather than per window. */
static void apply_global_config(SfeApplication* app)
{
    static int file_icons = -1;
    sfe_theme_set_scale(app->config->ui_scale);
    if (file_icons >= 0 && file_icons != app->config->file_icons)
        sfe_icons_reset();
    file_icons = app->config->file_icons;
}

static void replace_config(SfeApplication* app, SfeConfig* cfg)
{
    SfeConfig* old = app->config;
    app->config = cfg;
    apply_global_config(app);
    g_signal_emit(app, signals[SIGNAL_CONFIG_CHANGED], 0);
    sfe_config_free(old);
}

void sfe_application_set_config(SfeApplication* app, SfeConfig* cfg)
{
    g_autoptr(GError) error = NULL;

    g_free(app->last_written);
    app->last_written = sfe_config_to_string(cfg);
    if (!sfe_config_save(cfg, app->config_path, &error))
        g_warning("cannot save %s: %s", app->config_path, error->message);
    replace_config(app, cfg);
}

/* ------------------------------------------------------------------------ */
/* Config file loading and hot reload                                        */
/* ------------------------------------------------------------------------ */

/* A desktop notification through notify-send (libnotify), or GNotification
 * without it. Each one replaces the previous one where the notification
 * daemon supports it, so saving a broken file twice does not stack them. */
static void notify(SfeApplication* app, const char* title, const char* body)
{
    g_autofree char* notify_send = g_find_program_in_path("notify-send");
    if (notify_send != NULL) {
        const char* argv[] = { notify_send, "--app-name=sfe", "--icon=sfe", "--urgency=normal",
            "--hint=string:x-canonical-private-synchronous:sfe-config",
            "--hint=string:x-dunst-stack-tag:sfe-config", "--", title, body, NULL };
        g_autoptr(GError) error = NULL;
        if (g_spawn_async(NULL, (char**)argv, NULL, G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                NULL, NULL, NULL, &error))
            return;
        g_debug("notify-send: %s", error->message);
    }
    g_autoptr(GNotification) n = g_notification_new(title);
    g_notification_set_body(n, body);
    g_application_send_notification(G_APPLICATION(app), "config", n);
}

/* Tells the user about a config file that does not parse, or about the
 * entries that were skipped; `fallback` says what sfe uses meanwhile. */
static void report_config(SfeApplication* app, const SfeConfig* cfg, const GError* error, const char* fallback)
{
    g_autofree char* name = g_path_get_basename(app->config_path);

    if (error != NULL) {
        g_autofree char* title = g_strdup_printf("sfe: %s has an error", name);
        g_autofree char* body = g_strdup_printf("%s\n%s", error->message, fallback);
        notify(app, title, body);
        return;
    }
    if (cfg == NULL || cfg->problems->len == 0)
        return;

    g_autoptr(GString) body = g_string_new(NULL);
    guint shown = MIN(cfg->problems->len, 5);
    for (guint i = 0; i < shown; i++)
        g_string_append_printf(body, "%s%s", i ? "\n" : "", (char*)g_ptr_array_index(cfg->problems, i));
    if (cfg->problems->len > shown)
        g_string_append_printf(body, "\n… and %u more", cfg->problems->len - shown);
    g_string_append(body, "\nThese entries are ignored.");
    g_autofree char* title = cfg->problems->len == 1
        ? g_strdup_printf("sfe: a problem in %s", name)
        : g_strdup_printf("sfe: %u problems in %s", cfg->problems->len, name);
    notify(app, title, body->str);
}

static void load_initial_config(SfeApplication* app)
{
    g_autoptr(GError) error = NULL;

    app->config_path = sfe_config_default_path();
    app->config = sfe_config_load(app->config_path, &error);
    if (app->config == NULL) {
        /* Keep the broken file untouched so the user can fix it. */
        g_warning("%s: %s; using defaults", app->config_path, error->message);
        report_config(app, NULL, error, "Using the default settings until it is fixed.");
        app->config = sfe_config_new();
        return;
    }
    report_config(app, app->config, NULL, NULL);
    if (!g_file_test(app->config_path, G_FILE_TEST_EXISTS)) {
        /* First run: write a commented default config for discoverability. */
        if (!sfe_config_save(app->config, app->config_path, &error))
            g_warning("cannot create %s: %s", app->config_path, error->message);
    }
}

static gboolean reload_config(gpointer data)
{
    SfeApplication* app = data;
    g_autoptr(GError) error = NULL;
    g_autofree char* text = NULL;
    gsize len = 0;

    app->reload_source = 0;
    if (!g_file_get_contents(app->config_path, &text, &len, NULL))
        return G_SOURCE_REMOVE; /* deleted or mid-rename: keep the current config */
    if (g_strcmp0(text, app->last_written) == 0)
        return G_SOURCE_REMOVE;

    SfeConfig* cfg = sfe_config_parse(text, len, &error);
    if (cfg == NULL) {
        g_warning("%s: %s; keeping the previous config", app->config_path, error->message);
        report_config(app, NULL, error, "The previous settings stay in use until it is fixed.");
        return G_SOURCE_REMOVE;
    }
    report_config(app, cfg, NULL, NULL);
    g_debug("reloaded %s", app->config_path);
    replace_config(app, cfg);
    return G_SOURCE_REMOVE;
}

static void on_config_file_changed(GFileMonitor* monitor, GFile* file, GFile* other,
    GFileMonitorEvent event, gpointer data)
{
    (void)monitor, (void)file, (void)other, (void)event;
    SfeApplication* app = data;

    if (app->reload_source != 0)
        g_source_remove(app->reload_source);
    app->reload_source = g_timeout_add(CONFIG_RELOAD_DELAY_MS, reload_config, app);
}

static void watch_config(SfeApplication* app)
{
    g_autoptr(GFile) file = g_file_new_for_path(app->config_path);
    g_autoptr(GError) error = NULL;

    app->config_monitor = g_file_monitor_file(file, G_FILE_MONITOR_WATCH_MOVES, NULL, &error);
    if (app->config_monitor == NULL) {
        g_warning("cannot watch %s: %s", app->config_path, error->message);
        return;
    }
    g_signal_connect(app->config_monitor, "changed", G_CALLBACK(on_config_file_changed), app);
}

/* ------------------------------------------------------------------------ */
/* Application actions                                                       */
/* ------------------------------------------------------------------------ */

static void action_new_window(GSimpleAction* action, GVariant* param, gpointer data)
{
    (void)action, (void)param;
    gtk_window_present(GTK_WINDOW(sfe_window_new(data, NULL)));
}

static void action_quit(GSimpleAction* action, GVariant* param, gpointer data)
{
    (void)action, (void)param;
    g_application_quit(data);
}

static const GActionEntry app_actions[] = {
    { .name = "new-window", .activate = action_new_window },
    { .name = "quit", .activate = action_quit },
};

/* Application-wide shortcuts; window actions get theirs from actions.c. */
static const struct {
    const char* action;
    const char* accels[3]; /* NULL-terminated */
} app_accels[] = {
    { "app.new-window", { "<Control>n", NULL } },
    { "app.quit", { "<Control>q", NULL } },
};

/* ------------------------------------------------------------------------ */
/* GApplication vfuncs                                                       */
/* ------------------------------------------------------------------------ */

static void sfe_application_startup(GApplication* gapp)
{
    SfeApplication* app = SFE_APPLICATION(gapp);

    G_APPLICATION_CLASS(sfe_application_parent_class)->startup(gapp);

    sfe_theme_init();
    gtk_window_set_default_icon_name("sfe"); /* also bundled, for uninstalled builds */
    load_initial_config(app);
    watch_config(app);
    apply_global_config(app);

    g_action_map_add_action_entries(G_ACTION_MAP(app), app_actions,
        G_N_ELEMENTS(app_actions), app);
    for (size_t i = 0; i < G_N_ELEMENTS(app_accels); i++)
        gtk_application_set_accels_for_action(GTK_APPLICATION(app),
            app_accels[i].action, app_accels[i].accels);
    sfe_actions_set_accels(GTK_APPLICATION(app));
}

static void sfe_application_activate(GApplication* gapp)
{
    GtkWindow* window = GTK_WINDOW(sfe_window_new(SFE_APPLICATION(gapp), NULL));
    gtk_window_present(window);
    sfe_default_fm_maybe_ask(window); /* first start only */
}

/* `sfe DIR...` opens one window per directory. */
static void sfe_application_open(GApplication* gapp, GFile** files, int n_files, const char* hint)
{
    (void)hint;
    for (int i = 0; i < n_files; i++) {
        GtkWindow* window = GTK_WINDOW(sfe_window_new(SFE_APPLICATION(gapp), files[i]));
        gtk_window_present(window);
        if (i == 0)
            sfe_default_fm_maybe_ask(window); /* first start only */
    }
}

static void sfe_application_shutdown(GApplication* gapp)
{
    SfeApplication* app = SFE_APPLICATION(gapp);

    g_clear_handle_id(&app->reload_source, g_source_remove);
    g_clear_object(&app->config_monitor);
    G_APPLICATION_CLASS(sfe_application_parent_class)->shutdown(gapp);
}

static void sfe_application_finalize(GObject* object)
{
    SfeApplication* app = SFE_APPLICATION(object);

    sfe_config_free(app->config);
    g_free(app->config_path);
    g_free(app->last_written);
    G_OBJECT_CLASS(sfe_application_parent_class)->finalize(object);
}

/* --version is answered here, without starting the UI. */
static int sfe_application_handle_local_options(GApplication* gapp, GVariantDict* options)
{
    (void)gapp;
    if (g_variant_dict_contains(options, "version")) {
        g_print("sfe %s\n", SFE_VERSION);
        return 0;
    }
    return -1; /* go on */
}

static void sfe_application_class_init(SfeApplicationClass* klass)
{
    GApplicationClass* app_class = G_APPLICATION_CLASS(klass);

    app_class->startup = sfe_application_startup;
    app_class->activate = sfe_application_activate;
    app_class->open = sfe_application_open;
    app_class->shutdown = sfe_application_shutdown;
    app_class->handle_local_options = sfe_application_handle_local_options;
    G_OBJECT_CLASS(klass)->finalize = sfe_application_finalize;

    /* Emitted after the configuration was replaced (edited or reloaded). */
    signals[SIGNAL_CONFIG_CHANGED] = g_signal_new("config-changed",
        G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
        G_TYPE_NONE, 0);
}

static void sfe_application_init(SfeApplication* app)
{
    static const GOptionEntry entries[] = {
        { "version", 'v', 0, G_OPTION_ARG_NONE, NULL, "Show the version and quit", NULL },
        { NULL, 0, 0, 0, NULL, NULL, NULL },
    };
    g_application_add_main_option_entries(G_APPLICATION(app), entries);
    g_application_set_option_context_parameter_string(G_APPLICATION(app), "[DIRECTORY…]");
    g_application_set_option_context_summary(G_APPLICATION(app),
        "Simple File Explorer: opens a window for each DIRECTORY (the start folder without one).");
}

SfeApplication* sfe_application_new(void)
{
    return g_object_new(SFE_TYPE_APPLICATION,
        "application-id", SFE_APP_ID,
        "flags", G_APPLICATION_HANDLES_OPEN,
        NULL);
}
