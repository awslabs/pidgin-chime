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

#ifndef CHIME_APPLICATION_H
#define CHIME_APPLICATION_H

#include <gtk/gtk.h>
#include <glib.h>

G_BEGIN_DECLS

#define CHIME_TYPE_APPLICATION (chime_application_get_type ())
G_DECLARE_DERIVABLE_TYPE(ChimeApplication, chime_application, CHIME, APPLICATION, GtkApplication)

struct _ChimeApplicationClass
{
    GtkApplicationClass parent_class;
};

ChimeApplication *chime_application_new (void);

G_END_DECLS

#endif /* CHIME_APPLICATION_H */

/* ex:set ts=4 et: */
