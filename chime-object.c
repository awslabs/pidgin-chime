/*
 * Pidgin/libpurple Chime client plugin
 *
 * Copyright © 2017 Amazon.com, Inc. or its affiliates.
 *
 * Authors: David Woodhouse <dwmw2@infradead.org>
 *          Ignacio Casal Quinteiro <qignacio@amazon.com>
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


#include "chime-connection-private.h"
#include "chime-object.h"

#include <glib/gi18n.h>

enum
{
	PROP_0,
	PROP_ID,
	PROP_NAME,
	PROP_DEAD,
	LAST_PROP,
};

static GParamSpec *props[LAST_PROP];

G_DEFINE_TYPE(ChimeObject, chime_object, G_TYPE_OBJECT)

static void
chime_object_dispose(GObject *object)
{
	ChimeObject *self = CHIME_OBJECT(object);

	if (self->collection) {
		g_hash_table_remove(self->collection->by_name, self->name);
		g_hash_table_remove(self->collection->by_id, self->id);
	}

	printf("Object disposed: %p\n", self);

	G_OBJECT_CLASS(chime_object_parent_class)->dispose(object);
}

static void
chime_object_finalize(GObject *object)
{
	ChimeObject *self = CHIME_OBJECT(object);

	g_free(self->id);
	g_free(self->name);

	G_OBJECT_CLASS(chime_object_parent_class)->finalize(object);
}

static void chime_object_get_property(GObject *object, guint prop_id,
				    GValue *value, GParamSpec *pspec)
{
	ChimeObject *self = CHIME_OBJECT(object);

	switch (prop_id) {
	case PROP_ID:
		g_value_set_string(value, self->id);
		break;
	case PROP_NAME:
		g_value_set_string(value, self->name);
		break;
	case PROP_DEAD:
		g_value_set_boolean(value, self->is_dead);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void chime_object_set_property(GObject *object, guint prop_id,
				      const GValue *value, GParamSpec *pspec)
{
	ChimeObject *self = CHIME_OBJECT(object);

	switch (prop_id) {
	case PROP_ID:
		g_free(self->id);
		self->id = g_value_dup_string(value);
		break;
	case PROP_NAME:
		chime_object_rename(self, g_value_get_string(value));
		break;
	case PROP_DEAD:
		self->is_dead = g_value_get_boolean(value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

void chime_object_rename(ChimeObject *self, const gchar *name)
{
	if (!g_strcmp0(self->name, name))
		return;

	if (self->collection) {
		if (g_hash_table_lookup(self->collection->by_name, self->name) == self)
			g_hash_table_remove(self->collection->by_name, self->name);
	}
	g_free(self->name);
	self->name = g_strdup(name);
	if (self->collection)
		g_hash_table_insert(self->collection->by_name, self->name, self);
}

static void chime_object_class_init(ChimeObjectClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->finalize = chime_object_finalize;
	object_class->dispose = chime_object_dispose;
	object_class->get_property = chime_object_get_property;
	object_class->set_property = chime_object_set_property;

	props[PROP_ID] =
		g_param_spec_string("id",
				    "id",
				    "id",
				    NULL,
				    G_PARAM_READWRITE |
				    G_PARAM_CONSTRUCT_ONLY |
				    G_PARAM_STATIC_STRINGS);

	props[PROP_NAME] =
		g_param_spec_string("name",
				    "name",
				    "name",
				    NULL,
				    G_PARAM_READWRITE |
				    G_PARAM_CONSTRUCT_ONLY |
				    G_PARAM_STATIC_STRINGS);

	props[PROP_DEAD] =
		g_param_spec_boolean("dead",
				     "dead",
				     "dead",
				     FALSE,
				     G_PARAM_READWRITE |
				     G_PARAM_CONSTRUCT |
				     G_PARAM_STATIC_STRINGS);

	g_object_class_install_properties(object_class, LAST_PROP, props);
}

static void chime_object_init(ChimeObject *self)
{
}

const gchar *chime_object_get_id(ChimeObject *self)
{
	g_return_val_if_fail(CHIME_IS_OBJECT(self), NULL);

	return self->id;
}

const gchar *chime_object_get_name(ChimeObject *self)
{
	g_return_val_if_fail(CHIME_IS_OBJECT(self), NULL);

	return self->name;
}


void chime_object_collection_hash_object(ChimeObjectCollection *collection, ChimeObject *object,
					 gboolean live)
{
	object->generation = collection->generation;

	if (live && object->is_dead) {
		g_object_ref(object);
		object->is_dead = FALSE;
		g_object_notify(G_OBJECT(object), "dead");
	} else if (!live && !object->is_dead) {
		object->is_dead = TRUE;
		g_object_notify(G_OBJECT(object), "dead");
		g_object_unref(object);
	}

	if (!object->collection) {
		object->collection = collection;
		g_hash_table_insert(collection->by_id, object->id, object);
		g_hash_table_insert(collection->by_name, object->name, object);
	}
}

void chime_object_collection_expire_outdated(ChimeObjectCollection *coll)
{
	GList *objects = g_hash_table_get_values(coll->by_id);
	while (objects) {
		ChimeObject *object = CHIME_OBJECT(objects->data);

		if (!object->is_dead && object->generation != coll->generation) {
			object->is_dead = TRUE;
			g_object_notify(G_OBJECT(object), "dead");
			g_object_unref(object);
		}
		objects = g_list_remove(objects, object);
	}
}

static void unhash_object(gpointer _object)
{
	ChimeObject *object = CHIME_OBJECT(_object);

	/* Now it's unhashed, it doesn't need to unhash itself on dispose() */
	object->collection = NULL;

	if (!object->is_dead) {
		object->is_dead = TRUE;
		g_object_unref(object);
	}
}

void chime_object_collection_init(ChimeObjectCollection *coll)
{
	coll->by_id = g_hash_table_new_full(g_str_hash, g_str_equal,
						    NULL, unhash_object);
	coll->by_name = g_hash_table_new(g_str_hash, g_str_equal);
	coll->generation = 0;
}

void chime_object_collection_destroy(ChimeObjectCollection *coll)
{
	g_clear_pointer(&coll->by_name, g_hash_table_unref);
	g_clear_pointer(&coll->by_id, g_hash_table_unref);
}

struct foreach_object_st {
	ChimeConnection *cxn;
	ChimeObjectCB cb;
	gpointer cbdata;
};

static void foreach_object_cb(gpointer key, gpointer value, gpointer _data)
{
	struct foreach_object_st *data = _data;
	ChimeObject *object = CHIME_OBJECT(value);

	if (!object->is_dead)
		data->cb(data->cxn, object, data->cbdata);
}

void chime_object_collection_foreach_object(ChimeConnection *cxn, ChimeObjectCollection *coll,
					    ChimeObjectCB cb, gpointer cbdata)
{
	struct foreach_object_st data = {
		.cxn = cxn,
		.cb = cb,
		.cbdata = cbdata,
	};

	g_hash_table_foreach(coll->by_id, foreach_object_cb, &data);
}
