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

#include "window.h"
#include "connectionviewer.h"

#include <glib/gi18n.h>

struct _ChimeWindow
{
    GtkApplicationWindow parent_instance;

    GMenuModel *main_menu;

    ChimeConnectionViewer *viewer;

    GtkMenuButton *main_menu_button;

    GSettings *window_settings;
    GdkWindowState window_state;
    gboolean dispose_has_run;
};

enum
{
    PROP_0,
    PROP_MAIN_MENU,
    LAST_PROP
};

static GParamSpec *props[LAST_PROP];

G_DEFINE_TYPE(ChimeWindow, chime_window, GTK_TYPE_APPLICATION_WINDOW)

static void
chime_window_dispose(GObject *object)
{
    ChimeWindow *window = CHIME_WINDOW(object);

    if (!window->dispose_has_run) {
        g_settings_apply(window->window_settings);
        window->dispose_has_run = TRUE;
    }

    g_clear_object(&window->window_settings);
    g_clear_object(&window->main_menu);

    G_OBJECT_CLASS(chime_window_parent_class)->dispose(object);
}

static void
chime_window_get_property(GObject    *object,
                          guint       prop_id,
                          GValue     *value,
                          GParamSpec *pspec)
{
    ChimeWindow *window = CHIME_WINDOW(object);

    switch (prop_id) {
    case PROP_MAIN_MENU:
        g_value_set_object(value, window->main_menu);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void
chime_window_set_property(GObject      *object,
                          guint         prop_id,
                          const GValue *value,
                          GParamSpec   *pspec)
{
    ChimeWindow *window = CHIME_WINDOW(object);

    switch (prop_id) {
    case PROP_MAIN_MENU:
        window->main_menu = g_value_dup_object(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void
chime_window_constructed(GObject *object)
{
    ChimeWindow *window = CHIME_WINDOW(object);

    if (window->main_menu != NULL) {
        gtk_menu_button_set_menu_model(window->main_menu_button, window->main_menu);
    }

    G_OBJECT_CLASS(chime_window_parent_class)->constructed(object);
}

static gboolean
chime_window_window_state_event(GtkWidget           *widget,
                             GdkEventWindowState *event)
{
    ChimeWindow *window = CHIME_WINDOW(widget);

    window->window_state = event->new_window_state;

    g_settings_set_int(window->window_settings, "state",
                       window->window_state);

    return GTK_WIDGET_CLASS(chime_window_parent_class)->window_state_event(widget, event);
}

static void
save_window_state(GtkWidget *widget)
{
    ChimeWindow *window = CHIME_WINDOW(widget);

    if ((window->window_state &
        (GDK_WINDOW_STATE_MAXIMIZED | GDK_WINDOW_STATE_FULLSCREEN)) == 0) {
        gint width, height;

        gtk_window_get_size(GTK_WINDOW (widget), &width, &height);

        g_settings_set(window->window_settings, "size",
                       "(ii)", width, height);
    }
}

static gboolean
chime_window_configure_event(GtkWidget         *widget,
                          GdkEventConfigure *event)
{
    if (gtk_widget_get_realized(widget)) {
        save_window_state(widget);
    }

    return GTK_WIDGET_CLASS(chime_window_parent_class)->configure_event(widget, event);
}

static void
chime_window_class_init(ChimeWindowClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = chime_window_dispose;
    object_class->get_property = chime_window_get_property;
    object_class->set_property = chime_window_set_property;
    object_class->constructed = chime_window_constructed;

    widget_class->window_state_event = chime_window_window_state_event;
    widget_class->configure_event = chime_window_configure_event;

    props[PROP_MAIN_MENU] =
        g_param_spec_object("main-menu",
                            "main menu",
                            "main menu",
                            G_TYPE_MENU_MODEL,
                            G_PARAM_READWRITE |
                            G_PARAM_CONSTRUCT_ONLY |
                            G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, LAST_PROP, props);

    /* Bind class to template */
    gtk_widget_class_set_template_from_resource(widget_class,
                                                "/org/gnome/Chime/ui/window.ui");
    gtk_widget_class_bind_template_child(widget_class, ChimeWindow, main_menu_button);
    gtk_widget_class_bind_template_child(widget_class, ChimeWindow, viewer);
}

static GActionEntry win_entries[] = {

};

static void
chime_window_init(ChimeWindow *window)
{
    gtk_widget_init_template(GTK_WIDGET(window));

    /* window settings are applied only once the window is closed. We do not
       want to keep writing to disk when the window is dragged around */
    window->window_settings = g_settings_new("org.gnome.Chime.state.window");
    g_settings_delay(window->window_settings);

    g_action_map_add_action_entries(G_ACTION_MAP(window),
                                    win_entries,
                                    G_N_ELEMENTS(win_entries),
                                    window);
}

/* ex:set ts=4 et: */
