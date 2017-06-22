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

struct chime_chat {
	struct chime_room *room;
	PurpleConversation *conv;
	GHashTable *messages; /* While fetching */
	GHashTable *members;
};

static void chat_msg_cb(gpointer _chat, JsonNode *node)
{
	struct chime_chat *chat = _chat;
	const gchar *str;

	JsonObject *obj = json_node_get_object(node);
	JsonNode *record = json_object_get_member(obj, "record");
	if (!record)
		return;
	if (parse_string(record, "Content", &str)) {
		PurpleConnection *conn = chat->conv->account->gc;
		struct chime_connection *cxn = purple_connection_get_protocol_data(conn);
		int id = purple_conv_chat_get_id(PURPLE_CONV_CHAT(chat->conv));
		serv_got_chat_in(conn, id, "someone", PURPLE_MESSAGE_RECV, str,0xb297796e - time(NULL) );
	}
}

void chime_destroy_chat(struct chime_chat *chat)
{
	PurpleConnection *conn = chat->conv->account->gc;
	struct chime_connection *cxn = purple_connection_get_protocol_data(conn);
	struct chime_room *room = chat->room;
	int id = purple_conv_chat_get_id(PURPLE_CONV_CHAT(room->chat->conv));

	chime_jugg_unsubscribe(cxn, room->channel, chat_msg_cb, room);

	serv_got_chat_left(conn, id);
	g_hash_table_remove(cxn->live_chats, GUINT_TO_POINTER(room->id));

	if (chat->messages)
		g_hash_table_destroy(chat->messages);
	if (chat->members)
		g_hash_table_destroy(chat->members);

	g_free(chat);
	room->chat = NULL;
	printf("Destroyed chat %p\n", chat);
}

static void one_msg_cb(JsonArray *array, guint index_,
		       JsonNode *node, gpointer _hash)
{
	struct chime_chat *chat;
	const char *id;

	if (parse_string(node, "MessageId", &id))
		g_hash_table_insert(_hash, (gpointer)id, json_node_ref(node));
}

void fetch_chat_messages(struct chime_connection *cxn, struct chime_chat *chat, const gchar *next_token);
static void fetch_msgs_cb(struct chime_connection *cxn, SoupMessage *msg, JsonNode *node, gpointer _chat)
{
	struct chime_chat *chat = _chat;
	const gchar *next_token;

	JsonObject *obj = json_node_get_object(node);
	JsonNode *msgs_node = json_object_get_member(obj, "Messages");
	JsonArray *msgs_array = json_node_get_array(msgs_node);

	json_array_foreach_element(msgs_array, one_msg_cb, chat->messages);

	if (parse_string(node, "NextToken", &next_token))
		fetch_chat_messages(cxn, _chat, next_token);
	else {
		/* Done fetching. Now (XXX: sort and) deliver */
	}
}

void fetch_chat_messages(struct chime_connection *cxn, struct chime_chat *chat, const gchar *next_token)
{
	struct chime_room *room = chat->room;
	SoupURI *uri = soup_uri_new_printf(cxn->messaging_url, "/rooms/%s/messages", room->id);
	const gchar *opts[4];
	int i = 0;
	gchar *last_msgs_key = g_strdup_printf("last-room-%s", room->id);
	const gchar *after = purple_account_get_string(cxn->prpl_conn->account, last_msgs_key, NULL);
	g_free(last_msgs_key);
	if (after && after[0]) {
		opts[i++] = "after";
		opts[i++] = after;
	}
	if (next_token) {
		opts[i++] = "next-token";
		opts[i++] = next_token;
	}
	while (i < 4)
		opts[i++] = NULL;

	soup_uri_set_query_from_fields(uri, "max-results", "10", opts[0], opts[1], opts[2], opts[3], NULL);
	chime_queue_http_request(cxn, NULL, uri, fetch_msgs_cb, chat, TRUE);
}

void chime_purple_join_chat(PurpleConnection *conn, GHashTable *data)
{
	struct chime_connection *cxn = purple_connection_get_protocol_data(conn);
	struct chime_room *room;
	struct chime_chat *chat;
	const gchar *roomid = g_hash_table_lookup(data, "RoomId");

	printf("join_chat %p %s %s\n", data, roomid, (gchar *)g_hash_table_lookup(data, "Name"));
	room = g_hash_table_lookup(cxn->rooms_by_id, roomid);
	if (!room || room->chat)
		return;

	chat = g_new0(struct chime_chat, 1);
	room->chat = chat;
	chat->room = room;

	int chat_id = ++cxn->chat_id;
	chat->conv = serv_got_joined_chat(conn, chat_id, g_hash_table_lookup(data, "Name"));
	g_hash_table_insert(cxn->live_chats, GUINT_TO_POINTER(chat_id), chat);
	chime_jugg_subscribe(cxn, room->channel, chat_msg_cb, chat);

	fetch_chat_messages(cxn, chat, NULL);
}

void chime_purple_chat_leave(PurpleConnection *conn, int id)
{
	struct chime_connection *cxn = purple_connection_get_protocol_data(conn);
	struct chime_chat *chat = g_hash_table_lookup(cxn->live_chats, GUINT_TO_POINTER(id));

	chime_destroy_chat(chat);
}
