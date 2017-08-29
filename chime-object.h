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

#define CHIME_TYPE_OBJECT (chime_object_get_type ())
G_DECLARE_DERIVABLE_TYPE (ChimeObject, chime_object, CHIME, OBJECT, GObject)

typedef struct {
	GHashTable *by_id;
	GHashTable *by_name;
	gint64 generation;
} ChimeObjectCollection;

struct _ChimeObjectClass {
	GObjectClass parent_class;
};

const gchar *chime_object_get_id(ChimeObject *self);
const gchar *chime_object_get_name(ChimeObject *self);
gboolean chime_object_is_dead(ChimeObject *self);

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

void             chime_connection_send_message_async         (ChimeConnection    *self,
                                                              ChimeObject        *obj,
                                                              const gchar        *message,
                                                              GCancellable       *cancellable,
                                                              GAsyncReadyCallback callback,
                                                              gpointer            user_data);

JsonNode        *chime_connection_send_message_finish        (ChimeConnection  *self,
                                                              GAsyncResult     *result,
                                                              GError          **error);

void             chime_connection_fetch_messages_async       (ChimeConnection    *self,
                                                              ChimeObject        *obj,
                                                              const gchar        *before,
                                                              const gchar        *after,
                                                              GCancellable       *cancellable,
                                                              GAsyncReadyCallback callback,
                                                              gpointer            user_data);

gboolean         chime_connection_fetch_messages_finish      (ChimeConnection  *self,
                                                              GAsyncResult     *result,
                                                              GError          **error);

void             chime_connection_update_last_read_async     (ChimeConnection    *self,
                                                              ChimeObject        *obj,
                                                              const gchar        *msg_id,
                                                              GCancellable       *cancellable,
                                                              GAsyncReadyCallback callback,
                                                              gpointer            user_data);

gboolean         chime_connection_update_last_read_finish    (ChimeConnection  *self,
                                                              GAsyncResult     *result,
                                                              GError          **error);
G_END_DECLS

#endif /* __CHIME_OBJECT_H__ */
