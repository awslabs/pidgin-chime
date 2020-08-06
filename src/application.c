/*
 * Copyright © 2020 Amazon.com, Inc. or its affiliates.
 *
 * Authors: Ignacio Casal Quinteiro <icq@gnome.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2.1, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

#include "application.h"
#include "window.h"
#include "connectionviewer.h"
#include "meetinglistview.h"

#include <stdlib.h>
#include <glib/gi18n.h>

typedef struct
{
    GSettings *window_settings;

    gchar *log_level;
} ChimeApplicationPrivate;

G_DEFINE_TYPE_WITH_PRIVATE(ChimeApplication, chime_application, GTK_TYPE_APPLICATION)

static void
chime_application_finalize(GObject *object)
{
    ChimeApplication *app = CHIME_APPLICATION(object);
    ChimeApplicationPrivate *priv;

    priv = chime_application_get_instance_private(app);

    g_free(priv->log_level);

    G_OBJECT_CLASS(chime_application_parent_class)->finalize(object);
}

static void
chime_application_dispose(GObject *object)
{
    ChimeApplication *app = CHIME_APPLICATION(object);
    ChimeApplicationPrivate *priv;

    priv = chime_application_get_instance_private(app);

    g_debug("Disposing application");

    g_clear_object(&priv->window_settings);

    g_debug("Application disposed");

    G_OBJECT_CLASS(chime_application_parent_class)->dispose(object);
}

static GtkCssProvider *
load_css_from_resource(const gchar *filename,
                       gboolean     required)
{
    GError *error = NULL;
    GFile *css_file;
    GtkCssProvider *provider;
    gchar *resource_name;

    resource_name = g_strdup_printf("resource:///org/gnome/Chime/css/%s", filename);
    css_file = g_file_new_for_uri(resource_name);
    g_free (resource_name);

    if (!required && !g_file_query_exists(css_file, NULL)) {
        g_object_unref(css_file);
        return NULL;
    }

    provider = gtk_css_provider_new();

    if (gtk_css_provider_load_from_file (provider, css_file, &error)) {
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
                                                  GTK_STYLE_PROVIDER(provider),
                                                  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    } else {
        g_warning("Could not load css provider: %s", error->message);
        g_error_free (error);
    }

    g_object_unref(css_file);

    return provider;
}

static ChimeWindow *
create_window(ChimeApplication *app)
{
    ChimeApplicationPrivate *priv;
    ChimeWindow *window;
    GdkWindowState state;
    gint w, h;

    priv = chime_application_get_instance_private(app);

    window = g_object_new(CHIME_TYPE_WINDOW,
                          "application", app,
                          NULL);

    state = g_settings_get_int(priv->window_settings, "state");

    g_settings_get(priv->window_settings, "size",
                   "(ii)", &w, &h);

    gtk_window_set_default_size(GTK_WINDOW(window),
                                w, h);

    if ((state & GDK_WINDOW_STATE_MAXIMIZED) != 0) {
        gtk_window_maximize(GTK_WINDOW(window));
    } else {
        gtk_window_unmaximize(GTK_WINDOW(window));
    }

    if ((state & GDK_WINDOW_STATE_STICKY) != 0) {
        gtk_window_stick(GTK_WINDOW(window));
    } else {
        gtk_window_unstick(GTK_WINDOW(window));
    }

    gtk_window_present(GTK_WINDOW(window));

    return window;
}

static ChimeWindow *
find_window_or_create(ChimeApplication *app)
{
    GList *windows;
    ChimeWindow *ret = NULL;

    windows = gtk_application_get_windows(GTK_APPLICATION(app));

    while (windows) {
        ChimeWindow *window;

        window = windows->data;
        windows = windows->next;

        if (!gtk_widget_get_realized(GTK_WIDGET(window)) ||
            !CHIME_IS_WINDOW(window)) {
            continue;
        }

        ret = window;
        break;
    }

    if (ret == NULL) {
        ret = create_window(app);
    } else {
        gtk_window_present(GTK_WINDOW(ret));
    }

    return ret;
}

static void
chime_application_activate(GApplication *application)
{
    ChimeApplication *app = CHIME_APPLICATION(application);

    G_APPLICATION_CLASS(chime_application_parent_class)->activate(application);

    find_window_or_create(app);
}

static void
about_activated(GSimpleAction *action,
                GVariant      *parameter,
                gpointer       user_data)
{
    /* FIXME: Implement */
}

static void
quit_activated(GSimpleAction *action,
               GVariant      *parameter,
               gpointer       user_data)
{
    g_application_quit(G_APPLICATION(user_data));
}

static GActionEntry app_entries[] = {
    { "about", about_activated, NULL, NULL, NULL },
    { "quit", quit_activated, NULL, NULL, NULL },
};

static void
chime_application_startup(GApplication *application)
{
    ChimeApplication *app = CHIME_APPLICATION(application);
    ChimeApplicationPrivate *priv;
    GError *error = NULL;

    priv = chime_application_get_instance_private(app);

    G_APPLICATION_CLASS(chime_application_parent_class)->startup(application);

    g_debug("Starting up application");

    g_object_unref(load_css_from_resource("style.css", TRUE));

    g_action_map_add_action_entries(G_ACTION_MAP(application),
                                    app_entries,
                                    G_N_ELEMENTS(app_entries),
                                    application);
}

static void
chime_application_shutdown(GApplication *application)
{
    ChimeApplication *app = CHIME_APPLICATION(application);
    ChimeApplicationPrivate *priv;

    priv = chime_application_get_instance_private(app);

    g_debug("Shutting down application");

    g_debug("Chime application shut down");

    G_APPLICATION_CLASS(chime_application_parent_class)->shutdown(application);
}

static gint
chime_application_handle_local_options(GApplication *application,
                                       GVariantDict *options)
{
    if (g_variant_dict_contains(options, "version")) {
        /* FIXME: take it from a define */
        g_print(_("Chime - version %s\n"), "0.0.1");
        return 0;
    }

    return -1;
}

static void
chime_application_class_init(ChimeApplicationClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GApplicationClass *application_class = G_APPLICATION_CLASS(klass);

    object_class->finalize = chime_application_finalize;
    object_class->dispose = chime_application_dispose;

    application_class->activate = chime_application_activate;
    application_class->startup = chime_application_startup;
    application_class->shutdown = chime_application_shutdown;
    application_class->handle_local_options = chime_application_handle_local_options;
}

static void
chime_application_init(ChimeApplication *app)
{
    ChimeApplicationPrivate *priv = chime_application_get_instance_private(app);
    const GOptionEntry main_entries[] = {
        { "version", 'v', 0, G_OPTION_ARG_NONE, NULL, N_("Print version and exit"), NULL },
        { "log-level", '\0', 0, G_OPTION_ARG_STRING, &priv->log_level, N_("Control verbosity of the logs"), "error|warning|info|debug" },
        { NULL }
    };

    priv->window_settings = g_settings_new("org.gnome.Chime.state.window");

    g_set_application_name("chime");

    g_application_add_main_option_entries(G_APPLICATION(app), main_entries);

    g_type_ensure(CHIME_TYPE_CONNECTION_VIEWER);
    g_type_ensure(CHIME_TYPE_MEETING_LIST_VIEW);

    /* NOTE: workaround the case where the themed icon is not registered when creating the window */
    g_type_ensure(G_TYPE_THEMED_ICON);
}

ChimeApplication *
chime_application_new(void)
{
    return g_object_new(CHIME_TYPE_APPLICATION,
                        "application-id", "org.gnome.Chime",
                        NULL);
}

/* ex:set ts=4 et: */
