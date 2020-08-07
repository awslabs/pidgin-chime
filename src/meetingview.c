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

#include "meetingview.h"


struct _ChimeMeetingView
{
    GtkGrid parent_instance;

    gboolean muted;

    GtkLabel *meeting_name_label;
    GtkLabel *meeting_organiser_label;
    GtkListBox *user_list_box;

    ChimeConnection  *connection;
    ChimeMeeting *meeting;
};

enum
{
    PROP_0,
    PROP_MUTED,
    LAST_PROP
};

enum
{
    LAST_SIGNAL
};

static GParamSpec *props[LAST_PROP];
static guint signals[LAST_SIGNAL];

G_DEFINE_TYPE(ChimeMeetingView, chime_meeting_view, GTK_TYPE_GRID)

static void
chime_meeting_view_dispose(GObject *object)
{
    ChimeMeetingView *self = CHIME_MEETING_VIEW(object);

    g_clear_object(&self->meeting);

    G_OBJECT_CLASS(chime_meeting_view_parent_class)->dispose(object);
}

static void
chime_meeting_view_get_property(GObject    *object,
                                guint       prop_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
    ChimeMeetingView *self = CHIME_MEETING_VIEW(object);

    switch (prop_id) {
    case PROP_MUTED:
        g_value_set_boolean(value, self->muted);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void
chime_meeting_view_set_property(GObject      *object,
                                guint         prop_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
    ChimeMeetingView *self = CHIME_MEETING_VIEW(object);

    switch (prop_id) {
    case PROP_MUTED:
        self->muted = g_value_get_boolean(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void
chime_meeting_view_constructed(GObject *object)
{
    ChimeMeetingView *self = CHIME_MEETING_VIEW(object);


    G_OBJECT_CLASS(chime_meeting_view_parent_class)->constructed(object);
}

static void
chime_meeting_view_class_init(ChimeMeetingViewClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = chime_meeting_view_dispose;
    object_class->get_property = chime_meeting_view_get_property;
    object_class->set_property = chime_meeting_view_set_property;
    object_class->constructed = chime_meeting_view_constructed;

    props[PROP_MUTED] =
        g_param_spec_boolean("muted",
                             "muted",
                             "muted",
                             FALSE,
                             G_PARAM_READWRITE |
                             G_PARAM_CONSTRUCT |
                             G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, LAST_PROP, props);

    /* Bind class to template */
    gtk_widget_class_set_template_from_resource(widget_class,
                                                "/org/gnome/Chime/ui/meetingview.ui");
    gtk_widget_class_bind_template_child(widget_class, ChimeMeetingView, meeting_name_label);
    gtk_widget_class_bind_template_child(widget_class, ChimeMeetingView, meeting_organiser_label);
    gtk_widget_class_bind_template_child(widget_class, ChimeMeetingView, user_list_box);
}

static void
chime_meeting_view_init(ChimeMeetingView *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));
}

GtkWidget *
chime_meeting_view_new(void)
{
    return g_object_new(CHIME_TYPE_MEETING_VIEW, NULL);
}

void
chime_meeting_view_set_meeting(ChimeMeetingView *self,
                               ChimeConnection  *connection,
                               ChimeMeeting     *meeting)
{
    g_return_if_fail(CHIME_IS_MEETING_VIEW(self));
    g_return_if_fail(CHIME_IS_MEETING(meeting));

    (void)g_set_object(&self->connection, connection);
    (void)g_set_object(&self->meeting, meeting);

    if (meeting != NULL) {
        ChimeContact *contact;

        contact = chime_meeting_get_organiser(meeting);

        g_object_bind_property(meeting,
                               "name",
                               self->meeting_name_label,
                               "label",
                               G_BINDING_DEFAULT | G_BINDING_SYNC_CREATE);

        g_object_bind_property(contact,
                               "display-name",
                               self->meeting_organiser_label,
                               "label",
                               G_BINDING_DEFAULT | G_BINDING_SYNC_CREATE);
    }
}

/* ex:set ts=4 et: */
