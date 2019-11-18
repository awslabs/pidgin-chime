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

#ifndef __CHIME_ROOM_H__
#define __CHIME_ROOM_H__

#include <glib-object.h>

#include <json-glib/json-glib.h>

#include "chime-connection.h"
#include "chime-object.h"

G_BEGIN_DECLS

#define CHIME_TYPE_ROOM (chime_room_get_type ())
G_DECLARE_FINAL_TYPE (ChimeRoom, chime_room, CHIME, ROOM, ChimeObject)

typedef enum {
	CHIME_ROOM_TYPE_STANDARD,
	CHIME_ROOM_TYPE_MEETING,
	CHIME_ROOM_TYPE_ORGANIZATION
} ChimeRoomType;

#define CHIME_TYPE_ROOM_TYPE (chime_room_type_get_type ())
GType chime_room_type_get_type (void) G_GNUC_CONST;

const gchar *chime_room_get_id(ChimeRoom *self);

const gchar *chime_room_get_name(ChimeRoom *self);

gboolean chime_room_get_privacy(ChimeRoom *self);

gboolean chime_room_get_visibility(ChimeRoom *self);

const gchar *chime_room_get_channel(ChimeRoom *self);

const gchar *chime_room_get_last_mentioned(ChimeRoom *self);
const gchar *chime_room_get_last_read(ChimeRoom *self);
const gchar *chime_room_get_last_sent(ChimeRoom *self);
const gchar *chime_room_get_created_on(ChimeRoom *self);

gboolean chime_room_has_mention(ChimeRoom *self);
gboolean chime_room_has_unread(ChimeRoom *self);

ChimeRoom *chime_connection_room_by_name(ChimeConnection *cxn,
					 const gchar *name);
ChimeRoom *chime_connection_room_by_id(ChimeConnection *cxn,
				       const gchar *id);

/* Designed to match the NEW_ROOM signal handler */
typedef void (*ChimeRoomCB) (ChimeConnection *, ChimeRoom *, gpointer);
void chime_connection_foreach_room(ChimeConnection *cxn, ChimeRoomCB cb,
				   gpointer cbdata);

typedef struct {
	ChimeContact *contact;
	gboolean admin;
	gboolean present;
	gboolean active;
	char *last_read;
	char *last_delivered;
} ChimeRoomMember;

gboolean chime_connection_open_room(ChimeConnection *cxn, ChimeRoom *room);
void chime_connection_close_room(ChimeConnection *cxn, ChimeRoom *room);

GList *chime_room_get_members(ChimeRoom *room);

void chime_connection_add_room_member_async(ChimeConnection *cxn,
					    ChimeRoom *room,
					    ChimeContact *contact,
					    GCancellable *cancellable,
					    GAsyncReadyCallback callback,
					    gpointer user_data);

gboolean chime_connection_add_room_member_finish(ChimeConnection *self,
						 GAsyncResult *result,
						 GError **error);

void chime_connection_remove_room_member_async(ChimeConnection *cxn,
					       ChimeRoom *room,
					       ChimeContact *contact,
					       GCancellable *cancellable,
					       GAsyncReadyCallback callback,
					       gpointer user_data);

gboolean chime_connection_remove_room_member_finish(ChimeConnection *self,
						    GAsyncResult *result,
						    GError **error);


void chime_connection_fetch_room_async(ChimeConnection *cxn, const gchar *room_id,
				       GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data);
ChimeRoom *chime_connection_fetch_room_finish(ChimeConnection *cxn, GAsyncResult *result, GError **error);

G_END_DECLS

#endif /* __CHIME_ROOM_H__ */
