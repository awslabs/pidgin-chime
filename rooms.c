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

static void one_room_cb(JsonArray *array, guint index_,
			JsonNode *node, gpointer _cxn)
{
	ChimeConnection *cxn = _cxn;
	struct chime_room *room;
	const gchar *channel, *id, *type, *name, *privacy, *visibility;

	if (!parse_string(node, "Channel", &channel) ||
	    !parse_string(node, "RoomId", &id) ||
	    !parse_string(node, "Type", &type) ||
	    !parse_string(node, "Name", &name) ||
	    !parse_string(node, "Privacy", &privacy) ||
	    !parse_string(node, "Visibility", &visibility))
		return;

	room = g_hash_table_lookup(cxn->rooms_by_id, id);
	if (room) {
		g_hash_table_remove(cxn->rooms_by_name, room->name);
		g_free(room->channel);
		g_free(room->type);
		g_free(room->name);
		g_free(room->privacy);
		g_free(room->visibility);
	} else {
		room = g_new0(struct chime_room, 1);
		room->id = g_strdup(id);
		g_hash_table_insert(cxn->rooms_by_id, room->id, room);
	}

	room->channel = g_strdup(channel);
	room->type = g_strdup(type);
	room->name = g_strdup(name);
	room->privacy = g_strdup(privacy);
	room->visibility = g_strdup(visibility);

	g_hash_table_insert(cxn->rooms_by_name, room->name, room);
}

static void fetch_rooms(ChimeConnection *cxn, const gchar *next_token);
static void roomlist_cb(ChimeConnection *cxn, SoupMessage *msg,
			JsonNode *node, gpointer _roomlist)
{
	const gchar *next_token;

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
		if (parse_string(node, "NextToken", &next_token))
			fetch_rooms(cxn, next_token);
		else {
			/* Aren't we supposed to do something to indicate we're done? */
		}
	}
}

static void fetch_rooms(ChimeConnection *cxn, const gchar *next_token)
{
	SoupURI *uri = soup_uri_new_printf(cxn->messaging_url, "/rooms");

	soup_uri_set_query_from_fields(uri, "max-results", "50",
				       next_token ? "next-token" : NULL, next_token,
				       NULL);
	chime_connection_queue_http_request(cxn, NULL, uri, "GET", roomlist_cb, NULL);
}

static void destroy_room(gpointer _room)
{
	struct chime_room *room = _room;

	if (room->chat)
		chime_destroy_chat(room->chat);

	g_free(room->channel);
	g_free(room->id);
	g_free(room->type);
	g_free(room->name);
	g_free(room->privacy);
	g_free(room->visibility);
	g_free(room);
}

static gboolean visible_rooms_jugg_cb(ChimeConnection *cxn, gpointer _unused, JsonNode *data_node)
{
	const gchar *type;
	if (!parse_string(data_node, "type", &type))
		return FALSE;

	if (strcmp(type, "update"))
		return FALSE;

	fetch_rooms(cxn, NULL);
	return TRUE;
}

void chime_init_rooms(ChimeConnection *cxn)
{
	cxn->rooms_by_name = g_hash_table_new(g_str_hash, g_str_equal);
	cxn->rooms_by_id = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, destroy_room);
	cxn->live_chats = g_hash_table_new(g_direct_hash, g_direct_equal);
	fetch_rooms(cxn, NULL);
	chime_jugg_subscribe(cxn, cxn->profile_channel, "VisibleRooms", visible_rooms_jugg_cb, NULL);
}

void chime_destroy_rooms(ChimeConnection *cxn)
{
	g_clear_pointer(&cxn->rooms_by_name, g_hash_table_unref);
	g_clear_pointer(&cxn->rooms_by_id, g_hash_table_unref);
	g_clear_pointer(&cxn->live_chats, g_hash_table_unref);
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
	ChimeConnection *cxn = purple_connection_get_protocol_data(conn);
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



GList *chime_purple_chat_info(PurpleConnection *conn)
{
	struct proto_chat_entry *pce = g_new0(struct proto_chat_entry, 1);
	GList *l;
	pce->label = _("Name:");
	pce->identifier = "Name";
	pce->required = TRUE;

	l = g_list_append(NULL, pce);

	/* Ick. We don't want this to be *shown* but the name alone isn't sufficient
	   because they aren't unique, and there's no way to preserve it otherwise
	   when the chat is added to the buddy list. */
	pce = g_new0(struct proto_chat_entry, 1);
	pce->label = _("RoomId");
	pce->identifier = "RoomId";
	pce->required = TRUE;

	return g_list_append(l, pce);
}

GHashTable *chime_purple_chat_info_defaults(PurpleConnection *conn, const char *name)
{
	ChimeConnection *cxn = purple_connection_get_protocol_data(conn);
	struct chime_room *room = NULL;
	GHashTable *hash;

	if (!name)
		return NULL;

	if (cxn->rooms_by_name)
		room = g_hash_table_lookup(cxn->rooms_by_name, name);
	if (!room)
		return NULL;

	hash = g_hash_table_new(g_str_hash, g_str_equal);
	g_hash_table_insert(hash, (char *)"Name", room->name);
	g_hash_table_insert(hash, (char *)"RoomId", room->id);
	return hash;
}


