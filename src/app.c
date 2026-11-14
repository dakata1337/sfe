#include "app.h"

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
    sfe_theme_set_scale(app->config->ui_scale);
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

static void load_initial_config(SfeApplication* app)
{
    g_autoptr(GError) error = NULL;

    app->config_path = sfe_config_default_path();
    app->config = sfe_config_load(app->config_path, &error);
    if (app->config == NULL) {
        /* Keep the broken file untouched so the user can fix it. */
        g_warning("%s: %s; using defaults", app->config_path, error->message);
        app->config = sfe_config_new();
        return;
    }
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
        return G_SOURCE_REMOVE;
    }
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

static const struct {
    const char* action;
    const char* accels[3]; /* NULL-terminated */
} app_accels[] = {
    { "app.new-window", { "<Control><Shift>n", NULL } },
    { "app.quit", { "<Control>q", NULL } },
    { "win.back", { "<Alt>Left", NULL } },
    { "win.forward", { "<Alt>Right", NULL } },
    { "win.up", { "<Alt>Up", NULL } },
    { "win.reload", { "F5", "<Control>r" } },
    { "win.location", { "<Control>l", NULL } },
    { "win.view-grid", { "<Control>1", NULL } },
    { "win.view-list", { "<Control>2", NULL } },
    { "win.toggle-hidden", { "<Control>h", NULL } },
    { "win.bookmark", { "<Control>d", NULL } },
};

/* ------------------------------------------------------------------------ */
/* GApplication vfuncs                                                       */
/* ------------------------------------------------------------------------ */

static void sfe_application_startup(GApplication* gapp)
{
    SfeApplication* app = SFE_APPLICATION(gapp);

    G_APPLICATION_CLASS(sfe_application_parent_class)->startup(gapp);

    sfe_theme_init();
    load_initial_config(app);
    watch_config(app);
    apply_global_config(app);

    g_action_map_add_action_entries(G_ACTION_MAP(app), app_actions,
        G_N_ELEMENTS(app_actions), app);
    for (size_t i = 0; i < G_N_ELEMENTS(app_accels); i++)
        gtk_application_set_accels_for_action(GTK_APPLICATION(app),
            app_accels[i].action, app_accels[i].accels);
}

static void sfe_application_activate(GApplication* gapp)
{
    gtk_window_present(GTK_WINDOW(sfe_window_new(SFE_APPLICATION(gapp), NULL)));
}

/* `sfe DIR...` opens one window per directory. */
static void sfe_application_open(GApplication* gapp, GFile** files, int n_files, const char* hint)
{
    (void)hint;
    for (int i = 0; i < n_files; i++)
        gtk_window_present(GTK_WINDOW(sfe_window_new(SFE_APPLICATION(gapp), files[i])));
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

static void sfe_application_class_init(SfeApplicationClass* klass)
{
    GApplicationClass* app_class = G_APPLICATION_CLASS(klass);

    app_class->startup = sfe_application_startup;
    app_class->activate = sfe_application_activate;
    app_class->open = sfe_application_open;
    app_class->shutdown = sfe_application_shutdown;
    G_OBJECT_CLASS(klass)->finalize = sfe_application_finalize;

    /* Emitted after the configuration was replaced (edited or reloaded). */
    signals[SIGNAL_CONFIG_CHANGED] = g_signal_new("config-changed",
        G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
        G_TYPE_NONE, 0);
}

static void sfe_application_init(SfeApplication* app)
{
    (void)app;
}

SfeApplication* sfe_application_new(void)
{
    return g_object_new(SFE_TYPE_APPLICATION,
        "application-id", SFE_APP_ID,
        "flags", G_APPLICATION_HANDLES_OPEN,
        NULL);
}
