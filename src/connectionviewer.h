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

#ifndef CHIME_CONNECTION_VIEWER_H
#define CHIME_CONNECTION_VIEWER_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define CHIME_TYPE_CONNECTION_VIEWER (chime_connection_viewer_get_type())
G_DECLARE_FINAL_TYPE(ChimeConnectionViewer, chime_connection_viewer, CHIME, CONNECTION_VIEWER, GtkBin)

ChimeConnectionViewer       *chime_connection_viewer_new();

G_END_DECLS

#endif /* CHIME_CONNECTION_VIEWER_H */

/* ex:set ts=4 et: */
