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

#include <string.h>

#include <glib/gi18n.h>
#include <glib/glist.h>

#include <prpl.h>
#include <blist.h>
#include <roomlist.h>

#include "chime.h"

#include <libsoup/soup.h>

struct chime_room {
	gchar *channel;
	gchar *id;
	gchar *type;
	gchar *name;
	gchar *privacy;
	gchar *visibility;
};

static void one_room_cb(JsonArray *array, guint index_,
			JsonNode *node, gpointer _cxn)
{
	struct chime_connection *cxn = _cxn;
	struct chime_room *room;
	const gchar *channel, *id, *type, *name, *privacy, *visibility;

	if (!parse_string(node, "Channel", &channel) ||
	    !parse_string(node, "RoomId", &id) ||
	    !parse_string(node, "Type", &type) ||
	    !parse_string(node, "Name", &name) ||
	    !parse_string(node, "Privacy", &privacy) ||
	    !parse_string(node, "Visibility", &visibility))
		return;

	room = g_new0(struct chime_room, 1);
	room->channel = g_strdup(channel);
	room->id = g_strdup(id);
	room->type = g_strdup(type);
	room->name = g_strdup(name);
	room->privacy = g_strdup(privacy);
	room->visibility = g_strdup(visibility);

	g_hash_table_insert(cxn->rooms_by_id, room->id, room);
	g_hash_table_insert(cxn->rooms_by_name, room->name, room);
}

static void roomlist_cb(struct chime_connection *cxn, SoupMessage *msg,
			JsonNode *node, gpointer _roomlist)
{
	if (SOUP_STATUS_IS_SUCCESSFUL(msg->status_code) && node) {
		JsonNode *rooms_node;
		JsonObject *obj;
		JsonArray *rooms_arr;

		obj = json_node_get_object(node);
		rooms_node = json_object_get_member(obj, "Rooms");
		if (rooms_node) {
			rooms_arr = json_node_get_array(rooms_node);
			json_array_foreach_element(rooms_arr, one_room_cb, cxn);
		}
	}
}


void fetch_rooms(struct chime_connection *cxn)
{
	SoupURI *uri = soup_uri_new_printf(cxn->messaging_url, "/rooms");
	chime_queue_http_request(cxn, NULL, uri, roomlist_cb, NULL, TRUE);
}

void chime_init_rooms(struct chime_connection *cxn)
{
	cxn->rooms_by_name = g_hash_table_new(g_str_hash, g_str_equal);
	cxn->rooms_by_id = g_hash_table_new(g_str_hash, g_str_equal);

	fetch_rooms(cxn);
}
static void destroy_room(gpointer _id, gpointer _room, gpointer _unused)
{
	struct chime_room *room;

	g_free(room->channel);
	g_free(room->id);
	g_free(room->type);
	g_free(room->name);
	g_free(room->privacy);
	g_free(room->visibility);
	g_free(room);
}
void chime_destroy_rooms(struct chime_connection *cxn)
{
	g_hash_table_foreach(cxn->rooms_by_name, destroy_room, NULL);
	g_hash_table_destroy(cxn->rooms_by_name);
	g_hash_table_destroy(cxn->rooms_by_id);
	cxn->rooms_by_name = cxn->rooms_by_id = NULL;
}

static void get_room(gpointer _id, gpointer _room, gpointer _roomlist)
{
	struct chime_room *room = _room;
	PurpleRoomlist *roomlist = _roomlist;
	PurpleRoomlistRoom *proom = purple_roomlist_room_new(PURPLE_ROOMLIST_ROOMTYPE_ROOM, room->name, NULL);

	purple_roomlist_room_add_field(roomlist, proom, room->id);
	purple_roomlist_room_add_field(roomlist, proom,
				       GUINT_TO_POINTER(!strcmp(room->visibility, "visible")));
	purple_roomlist_room_add_field(roomlist, proom,
				       GUINT_TO_POINTER(!strcmp(room->privacy, "private")));
	purple_roomlist_room_add(roomlist, proom);
	printf("Added room %s to %p\n", room->name, roomlist);
}

PurpleRoomlist *chime_purple_roomlist_get_list(PurpleConnection *conn)
{
	struct chime_connection *cxn = purple_connection_get_protocol_data(conn);
	PurpleRoomlist *roomlist;
	GList *fields = NULL;

	roomlist = purple_roomlist_new(conn->account);
	fields = g_list_append(fields, purple_roomlist_field_new(PURPLE_ROOMLIST_FIELD_STRING, "", "RoomId", TRUE));
	fields = g_list_append(fields, purple_roomlist_field_new(PURPLE_ROOMLIST_FIELD_BOOL, _("Visible"), "Visibility", FALSE));
	fields = g_list_append(fields, purple_roomlist_field_new(PURPLE_ROOMLIST_FIELD_BOOL, _("Private"), "Privacy", FALSE));
	purple_roomlist_set_fields(roomlist, fields);


	if (cxn->rooms_by_id)
		g_hash_table_foreach(cxn->rooms_by_id, get_room, roomlist);

	purple_roomlist_set_in_progress(roomlist, FALSE);
	return roomlist;
}

