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

#define CHIME_LOG_DOMAIN "viewer"

#include "connectionviewer.h"
#include "meetinglistview.h"
#include "meetingview.h"

#include "chime-connection.h"
#include "chime-contact.h"
#include "chime-conversation.h"
#include "chime-room.h"
#include "chime-meeting.h"

#include <glib/gi18n.h>

struct _ChimeConnectionViewer
{
    GtkBin parent_instance;

    GCancellable *cancellable;

    GSettings *account_settings;
    ChimeConnection *connection;

    GtkStack *stack;
    GtkFrame *main_frame;
    GtkStack *connect_stack;
    GtkGrid *connect_grid;
    GtkLabel *connect_explanation;
    GtkEntry *email_entry;
    GtkButton *connect_button;
    GtkGrid *connect_spinner_grid;
    GtkSpinner *connect_spinner;
    GtkGrid *login_grid;
    GtkLabel *login_explanation;
    GtkEntry *login_username_entry;
    GtkEntry *login_password_entry;
    GtkButton *login_button;
    GtkButton *cancel_login_button;
    GtkGrid *disconnecting_spinner_grid;
    GtkSpinner *disconnecting_spinner;
    GtkFrame *spinner_frame;
    GtkSpinner *spinner;
    GtkStack *connected_stack;
    ChimeMeetingListView *meeting_list_view;
    GtkFrame *join_meeting_spinner_frame;
    GtkSpinner *join_meeting_spinner;
    ChimeMeetingView *meeting_view;
};

enum
{
    PROP_0,
    LAST_PROP
};

static GParamSpec *props[LAST_PROP];

G_DEFINE_TYPE(ChimeConnectionViewer, chime_connection_viewer, GTK_TYPE_BIN)

static void on_request_authentication(GObject      *source,
                                      GAsyncResult *result,
                                      gpointer      user_data);

static void
chime_connection_viewer_dispose(GObject *object)
{
    ChimeConnectionViewer *viewer = CHIME_CONNECTION_VIEWER(object);

    g_debug("Disposing viewer");

    g_clear_object(&viewer->cancellable);
    g_clear_object(&viewer->account_settings);

    G_OBJECT_CLASS(chime_connection_viewer_parent_class)->dispose(object);
}

static void
chime_connection_viewer_get_property(GObject    *object,
                                  guint       prop_id,
                                  GValue     *value,
                                  GParamSpec *pspec)
{
    ChimeConnectionViewer *viewer = CHIME_CONNECTION_VIEWER(object);

    switch (prop_id) {
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void
chime_connection_viewer_set_property(GObject      *object,
                                  guint         prop_id,
                                  const GValue *value,
                                  GParamSpec   *pspec)
{
    ChimeConnectionViewer *viewer = CHIME_CONNECTION_VIEWER(object);

    switch (prop_id) {
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void
go_back_to_initial_state(ChimeConnectionViewer *viewer)
{
    g_clear_object(&viewer->connection);

    /* For security reasons do not keep the password around */
    gtk_entry_set_text(viewer->login_password_entry, "");

    gtk_stack_set_visible_child(viewer->connect_stack, GTK_WIDGET(viewer->connect_grid));
    gtk_stack_set_visible_child(viewer->stack, GTK_WIDGET(viewer->main_frame));
    gtk_widget_grab_focus(GTK_WIDGET(viewer->email_entry));
}

static void
chime_connection_viewer_class_init(ChimeConnectionViewerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = chime_connection_viewer_dispose;
    object_class->get_property = chime_connection_viewer_get_property;
    object_class->set_property = chime_connection_viewer_set_property;

    //g_object_class_install_properties(object_class, LAST_PROP, props);

    /* Bind class to template */
    gtk_widget_class_set_template_from_resource(widget_class,
                                                "/org/gnome/Chime/ui/connectionviewer.ui");
    gtk_widget_class_bind_template_child(widget_class, ChimeConnectionViewer, stack);
    gtk_widget_class_bind_template_child(widget_class, ChimeConnectionViewer, main_frame);
    gtk_widget_class_bind_template_child(widget_class, ChimeConnectionViewer, connect_stack);
    gtk_widget_class_bind_template_child(widget_class, ChimeConnectionViewer, connect_grid);
    gtk_widget_class_bind_template_child(widget_class, ChimeConnectionViewer, connect_explanation);
    gtk_widget_class_bind_template_child(widget_class, ChimeConnectionViewer, email_entry);
    gtk_widget_class_bind_template_child(widget_class, ChimeConnectionViewer, connect_button);
    gtk_widget_class_bind_template_child(widget_class, ChimeConnectionViewer, connect_spinner_grid);
    gtk_widget_class_bind_template_child(widget_class, ChimeConnectionViewer, connect_spinner);
    gtk_widget_class_bind_template_child(widget_class, ChimeConnectionViewer, login_grid);
    gtk_widget_class_bind_template_child(widget_class, ChimeConnectionViewer, login_explanation);
    gtk_widget_class_bind_template_child(widget_class, ChimeConnectionViewer, login_username_entry);
    gtk_widget_class_bind_template_child(widget_class, ChimeConnectionViewer, login_password_entry);
    gtk_widget_class_bind_template_child(widget_class, ChimeConnectionViewer, login_button);
    gtk_widget_class_bind_template_child(widget_class, ChimeConnectionViewer, cancel_login_button);
    gtk_widget_class_bind_template_child(widget_class, ChimeConnectionViewer, disconnecting_spinner_grid);
    gtk_widget_class_bind_template_child(widget_class, ChimeConnectionViewer, disconnecting_spinner);
    gtk_widget_class_bind_template_child(widget_class, ChimeConnectionViewer, spinner_frame);
    gtk_widget_class_bind_template_child(widget_class, ChimeConnectionViewer, spinner);
    gtk_widget_class_bind_template_child(widget_class, ChimeConnectionViewer, connected_stack);
    gtk_widget_class_bind_template_child(widget_class, ChimeConnectionViewer, meeting_list_view);
    gtk_widget_class_bind_template_child(widget_class, ChimeConnectionViewer, join_meeting_spinner_frame);
    gtk_widget_class_bind_template_child(widget_class, ChimeConnectionViewer, join_meeting_spinner);
    gtk_widget_class_bind_template_child(widget_class, ChimeConnectionViewer, meeting_view);
}

static void
on_connection_session_token_changed(GObject               *object,
                                    GParamSpec            *pspec,
                                    ChimeConnectionViewer *viewer)
{
    g_debug("Session token changed: %s", chime_connection_get_session_token(viewer->connection));

    g_settings_set_string(viewer->account_settings, "session-token",
                          chime_connection_get_session_token(viewer->connection) ? chime_connection_get_session_token(viewer->connection) : "");
}

static void
on_connection_authenticate(ChimeConnection       *connection,
                           gboolean               user_required,
                           ChimeConnectionViewer *viewer)
{
    g_debug("Authenticate");

    gtk_spinner_stop(viewer->connect_spinner);
    gtk_stack_set_visible_child(viewer->connect_stack, GTK_WIDGET(viewer->login_grid));
}

static void
on_connection_connected(ChimeConnection       *connection,
                        const gchar           *display_name,
                        ChimeConnectionViewer *viewer)
{
    g_info("Connected as %s", display_name);

    gtk_stack_set_visible_child(viewer->stack, GTK_WIDGET(viewer->connected_stack));
}

static void
on_connection_disconnected(ChimeConnection       *connection,
                           GError                *error,
                           ChimeConnectionViewer *viewer)
{
    g_info("Disconnected");
}

static void
on_connection_log_message(ChimeConnection       *connection,
                          ChimeLogLevel          level,
                          const gchar           *str,
                          ChimeConnectionViewer *viewer)
{
    g_debug("%s", str);
}

static void
on_connection_progress(ChimeConnection       *connection,
                       gint                   percent,
                       const gchar           *message,
                       ChimeConnectionViewer *viewer)
{
    g_info("%d - %s", percent, message);
}

static void
on_connection_new_meeting(ChimeConnection       *connection,
                          ChimeMeeting          *meeting,
                          ChimeConnectionViewer *viewer)
{
    g_info("New meeting: %s", chime_meeting_get_name(meeting));
}

#define SIGNIN_DEFAULT "https://signin.id.ue1.app.chime.aws/"

static void
try_login(ChimeConnectionViewer *viewer)
{
    const gchar *email;
    const gchar *device_token;
    const gchar *session_token;

    email = g_settings_get_string(viewer->account_settings, "email");
    device_token = g_settings_get_string(viewer->account_settings, "device-token");

    if (*email == '\0' || *device_token == '\0') {
        g_info("Requires login");
        return;
    }

    session_token = g_settings_get_string(viewer->account_settings, "session-token");

    g_clear_object(&viewer->connection);
    viewer->connection = chime_connection_new(email, SIGNIN_DEFAULT, device_token, session_token);
    chime_meeting_list_view_set_connection(viewer->meeting_list_view, viewer->connection);

    g_signal_connect(viewer->connection, "notify::session-token",
                     G_CALLBACK(on_connection_session_token_changed), viewer);
    g_signal_connect(viewer->connection, "authenticate",
                     G_CALLBACK(on_connection_authenticate), viewer);
    g_signal_connect(viewer->connection, "connected",
                     G_CALLBACK(on_connection_connected), viewer);
    g_signal_connect(viewer->connection, "disconnected",
                     G_CALLBACK(on_connection_disconnected), viewer);
    g_signal_connect(viewer->connection, "log-message",
                     G_CALLBACK(on_connection_log_message), viewer);
    g_signal_connect(viewer->connection, "progress",
                     G_CALLBACK(on_connection_progress), viewer);
    g_signal_connect(viewer->connection, "new-meeting",
                     G_CALLBACK(on_connection_new_meeting), viewer);

    if (*session_token == '\0') {
        gtk_spinner_start(viewer->connect_spinner);
        gtk_stack_set_visible_child(viewer->connect_stack, GTK_WIDGET(viewer->connect_spinner_grid));
    } else {
        gtk_spinner_start(viewer->spinner);
        gtk_stack_set_visible_child(viewer->stack, GTK_WIDGET(viewer->spinner_frame));
    }

    chime_connection_connect(viewer->connection);
}

static void
on_email_entry_activated(GtkEntry              *entry,
                         ChimeConnectionViewer *viewer)
{
    try_login(viewer);
}

static void
on_connect_button_clicked(GtkButton          *button,
                          ChimeConnectionViewer *viewer)
{
    try_login(viewer);
}

static void
authenticate(ChimeConnectionViewer *viewer)
{
    const gchar *username;
    const gchar *password;

    username = gtk_entry_get_text(viewer->login_username_entry);
    password = gtk_entry_get_text(viewer->login_password_entry);

    if (username != NULL && *username != '\0') {
        g_info("Authenticating '%s'", username);

        gtk_spinner_start(viewer->spinner);
        gtk_stack_set_visible_child(viewer->stack, GTK_WIDGET(viewer->spinner_frame));

        chime_connection_authenticate(viewer->connection, username, password);
    }
}

static void
on_login_entry_activated(GtkEntry              *entry,
                         ChimeConnectionViewer *viewer)
{
    authenticate(viewer);
}

static void
on_login_button_clicked(GtkButton          *button,
                        ChimeConnectionViewer *viewer)
{
    authenticate(viewer);
}

static void
on_cancel_login_button_clicked(GtkButton          *button,
                               ChimeConnectionViewer *viewer)
{
    go_back_to_initial_state(viewer);
}

static void
join_meeting_ready(GObject      *source,
                   GAsyncResult *result,
                   gpointer      user_data)
{
    ChimeConnectionViewer *viewer = user_data;
    ChimeMeeting *meeting;
    GError *error = NULL;

    meeting = chime_connection_join_meeting_finish(CHIME_CONNECTION(source), result, &error);
    if (meeting == NULL) {
        g_warning("Could not join the meeting: %s", error->message);
        g_error_free(error);
        /* FIXME: emit a signal */
        return;
    }

    g_message("Joined meeting: %s", chime_meeting_get_name(meeting));

    chime_meeting_view_set_meeting(viewer->meeting_view, meeting);
    g_object_unref(meeting);

    gtk_spinner_stop(viewer->join_meeting_spinner);
    gtk_stack_set_visible_child(viewer->connected_stack, GTK_WIDGET(viewer->meeting_view));
}

static void
on_join_meeting(ChimeMeetingListView  *view,
                ChimeMeeting          *meeting,
                ChimeConnectionViewer *viewer)
{
    g_message("on join meeting");
    gtk_spinner_start(viewer->join_meeting_spinner);
    gtk_stack_set_visible_child(viewer->connected_stack, GTK_WIDGET(viewer->join_meeting_spinner_frame));

    chime_connection_join_meeting_async(viewer->connection, meeting, FALSE,
                                        NULL, join_meeting_ready, viewer);
}

static void
chime_connection_viewer_init(ChimeConnectionViewer *viewer)
{
    gtk_widget_init_template(GTK_WIDGET(viewer));

    viewer->cancellable = g_cancellable_new();

    viewer->account_settings = g_settings_new("org.gnome.Chime.Account");

    g_settings_bind(viewer->account_settings,
                    "email",
                    viewer->email_entry,
                    "text",
                    G_SETTINGS_BIND_DEFAULT);

    g_signal_connect(viewer->email_entry, "activate",
                     G_CALLBACK(on_email_entry_activated), viewer);

    g_signal_connect(viewer->connect_button, "clicked",
                     G_CALLBACK(on_connect_button_clicked), viewer);

    g_signal_connect(viewer->login_username_entry,
                     "activate",
                     G_CALLBACK(on_login_entry_activated), viewer);
    g_signal_connect(viewer->login_password_entry, "activate",
                     G_CALLBACK(on_login_entry_activated), viewer);

    g_signal_connect(viewer->login_button, "clicked",
                     G_CALLBACK(on_login_button_clicked), viewer);
    g_signal_connect(viewer->cancel_login_button, "clicked",
                     G_CALLBACK(on_cancel_login_button_clicked), viewer);

    g_signal_connect(viewer->meeting_list_view, "join-meeting",
                     G_CALLBACK(on_join_meeting), viewer);

    try_login(viewer);
}

ChimeConnectionViewer *
chime_connection_viewer_new(void)
{
    return g_object_new(CHIME_TYPE_CONNECTION_VIEWER, NULL);
}

/* ex:set ts=4 et: */
