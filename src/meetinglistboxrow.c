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

#include "meetinglistboxrow.h"

#include <glib.h>
#include <glib/gi18n.h>


struct _ChimeMeetingListBoxRow
{
    GtkListBoxRow parent_instance;

    ChimeMeeting *meeting;

    GtkLabel *label;
    GtkLabel *organiser;
};

enum
{
    PROP_0,
    PROP_MEETING,
    LAST_PROP
};

static GParamSpec *props[LAST_PROP];

G_DEFINE_TYPE(ChimeMeetingListBoxRow, chime_meeting_list_box_row, GTK_TYPE_LIST_BOX_ROW)

static void
chime_meeting_list_box_row_dispose(GObject *object)
{
    ChimeMeetingListBoxRow *row = CHIME_MEETING_LIST_BOX_ROW(object);

    g_clear_object(&row->meeting);

    G_OBJECT_CLASS(chime_meeting_list_box_row_parent_class)->dispose(object);
}

static void
chime_meeting_list_box_row_get_property(GObject    *object,
                                        guint       prop_id,
                                        GValue     *value,
                                        GParamSpec *pspec)
{
    ChimeMeetingListBoxRow *row = CHIME_MEETING_LIST_BOX_ROW(object);

    switch (prop_id) {
    case PROP_MEETING:
        g_value_set_object(value, row->meeting);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void
chime_meeting_list_box_row_set_property(GObject      *object,
                                        guint         prop_id,
                                        const GValue *value,
                                        GParamSpec   *pspec)
{
    ChimeMeetingListBoxRow *row = CHIME_MEETING_LIST_BOX_ROW(object);

    switch (prop_id) {
    case PROP_MEETING:
        row->meeting = g_value_dup_object(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static gboolean
contact_to_organiser_label(GBinding     *binding,
                           const GValue *from_value,
                           GValue       *to_value,
                           gpointer      user_data)
{
    ChimeContact *organiser;
    gchar *organiser_str;

    organiser = g_value_get_object(from_value);
    organiser_str = g_strdup_printf(_("Organised by: %s <%s>"),
                                    chime_contact_get_display_name(organiser),
                                    chime_contact_get_email(organiser));

    g_value_take_string(to_value, organiser_str);

    return TRUE;
}

static void
chime_meeting_list_box_row_constructed(GObject *object)
{
    ChimeMeetingListBoxRow *row = CHIME_MEETING_LIST_BOX_ROW(object);

    g_object_bind_property(row->meeting,
                           "name",
                           row->label,
                           "label",
                           G_BINDING_DEFAULT | G_BINDING_SYNC_CREATE);

    g_object_bind_property_full(row->meeting,
                                "organiser",
                                row->organiser,
                                "label",
                                G_BINDING_DEFAULT | G_BINDING_SYNC_CREATE,
                                contact_to_organiser_label,
                                NULL, NULL, NULL);

    G_OBJECT_CLASS(chime_meeting_list_box_row_parent_class)->constructed(object);
}

static void
chime_meeting_list_box_row_class_init(ChimeMeetingListBoxRowClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = chime_meeting_list_box_row_dispose;
    object_class->get_property = chime_meeting_list_box_row_get_property;
    object_class->set_property = chime_meeting_list_box_row_set_property;
    object_class->constructed = chime_meeting_list_box_row_constructed;

    props[PROP_MEETING] =
        g_param_spec_object("meeting",
                            "meeting",
                            "meeting",
                            CHIME_TYPE_MEETING,
                            G_PARAM_READWRITE |
                            G_PARAM_CONSTRUCT_ONLY |
                            G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, LAST_PROP, props);

    /* Bind class to template */
    gtk_widget_class_set_template_from_resource(widget_class,
                                                "/org/gnome/Chime/ui/meetinglistboxrow.ui");
    gtk_widget_class_bind_template_child(widget_class, ChimeMeetingListBoxRow, label);
    gtk_widget_class_bind_template_child(widget_class, ChimeMeetingListBoxRow, organiser);
}

static void
chime_meeting_list_box_row_init(ChimeMeetingListBoxRow *row)
{
    gtk_widget_init_template(GTK_WIDGET(row));
}

GtkWidget *
chime_meeting_list_box_row_new(ChimeMeeting *meeting)
{
    return g_object_new(CHIME_TYPE_MEETING_LIST_BOX_ROW,
                        "meeting", meeting,
                        NULL);
}

ChimeMeeting *
chime_meeting_list_box_row_get_meeting(ChimeMeetingListBoxRow *self)
{
    g_return_val_if_fail(CHIME_IS_MEETING_LIST_BOX_ROW(self), NULL);

    return self->meeting;
}

/* ex:set ts=4 et: */
