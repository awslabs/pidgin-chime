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

#ifndef CHIME_DIRS_H
#define CHIME_DIRS_H

#include <glib.h>

G_BEGIN_DECLS

gchar       *chime_dirs_get_data_dir        (void);

gchar       *chime_dirs_get_lib_dir         (void);

G_END_DECLS

#endif /* CHIME_DIRS_H */

/* ex:set ts=4 et: */
