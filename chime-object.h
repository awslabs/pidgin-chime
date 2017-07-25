/*
 * Pidgin/libpurple Chime client plugin
 *
 * Copyright © 2017 Amazon.com, Inc. or its affiliates.
 *
 * Author: David Woodhouse <dwmw2@infradead.org>
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

#ifndef __CHIME_OBJECT_H__
#define __CHIME_OBJECT_H__

#include <glib-object.h>

#include <json-glib/json-glib.h>

#include "chime-connection.h"

G_BEGIN_DECLS

GType chime_object_get_type(void) G_GNUC_CONST;
#define CHIME_TYPE_OBJECT (chime_object_get_type ())
#define CHIME_OBJECT(obj) \
        (G_TYPE_CHECK_INSTANCE_CAST ((obj), CHIME_TYPE_OBJECT, ChimeObject))
#define CHIME_OBJECT_CLASS(cls) \
        (G_TYPE_CHECK_CLASS_CAST ((cls), CHIME_TYPE_OBJECT, ChimeObjectClass))
#define CHIME_IS_OBJECT(obj) \
        (G_TYPE_CHECK_INSTANCE_TYPE ((obj), CHIME_TYPE_OBJECT))
#define CHIME_IS_OBJECT_CLASS(cls) \
        (G_TYPE_CHECK_CLASS_TYPE ((cls), CHIME_TYPE_OBJECT))
#define CHIME_OBJECT_GET_CLASS(obj) \
        (G_TYPE_INSTANCE_GET_CLASS ((obj), CHIME_TYPE_OBJECT, ChimeObjectClass))

typedef struct {
	GHashTable *by_id;
	GHashTable *by_name;
	gint64 generation;
} ChimeObjectCollection;

typedef struct {
	GObjectClass parent_class;
} ChimeObjectClass;

typedef struct {
	GObject parent_instance;

	gchar *id;
	gchar *name;

	gint64 generation;

	/* While the obiect is live and discoverable, we hold a refcount to it
	 * But once it's dead, it remains in the hash table to avoid duplicates
	 * of rooms which we're added back to, or contacts who appear in some
	 * other room or conversation, etc.). It'll remove itself from the
	 * hash table in its ->dispose()  */
	gboolean is_dead;
	ChimeObjectCollection *collection;
} ChimeObject;

_GLIB_DEFINE_AUTOPTR_CHAINUP (ChimeObject, GObject);

const gchar *chime_object_get_id(ChimeObject *self);
const gchar *chime_object_get_name(ChimeObject *self);

void chime_object_rename(ChimeObject *self, const gchar *name);

void chime_object_collection_init(ChimeObjectCollection *coll);
void chime_object_collection_destroy(ChimeObjectCollection *coll);

ChimeObject *chime_connection_object_by_name(ChimeObjectCollection *coll,
					     const gchar *name);
ChimeObject *chime_connection_object_by_id(ChimeObjectCollection *coll,
					   const gchar *id);

void chime_object_collection_hash_object(ChimeObjectCollection *coll, ChimeObject *obj, gboolean live);

//void chime_object_obsolete(ChimeObject *self);

typedef void (*ChimeObjectCB) (ChimeConnection *, ChimeObject *, gpointer);
void chime_object_collection_foreach_object(ChimeConnection *cxn, ChimeObjectCollection *coll,
					    ChimeObjectCB cb, gpointer cbdata);

void chime_object_collection_expire_outdated(ChimeObjectCollection *coll);

G_END_DECLS

#endif /* __CHIME_OBJECT_H__ */
