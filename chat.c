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
	/* msgs first as it's a "subclass". Really ought to do proper GTypes here... */
	struct chime_msgs msgs;

	struct chime_room *room;
	PurpleConversation *conv;
	/* For cancellation */
	SoupMessage *members_msg;
	gboolean got_members;
	GHashTable *members;
};

struct chat_member {
	gchar *id;
	gchar *full_name;
};

static void chat_deliver_msg(struct chime_connection *cxn, struct chime_msgs *msgs,
			     JsonNode *node, time_t msg_time)
{
	struct chime_chat *chat = (struct chime_chat *)msgs; /* Really */
	struct chat_member *who = NULL;
	const gchar *str, *content;

	if (parse_string(node, "Content", &content)) {
		PurpleConnection *conn = chat->conv->account->gc;
		int id = purple_conv_chat_get_id(PURPLE_CONV_CHAT(chat->conv));

		if (parse_string(node, "Sender", &str))
			who = g_hash_table_lookup(chat->members, str);

		serv_got_chat_in(conn, id, who ? who->full_name : _("<unknown sender>"),
				 PURPLE_MESSAGE_RECV, content, msg_time);
	}

}


static gboolean add_chat_member(struct chime_chat *chat, JsonNode *node)
{
	const char *id, *full_name;
	PurpleConvChatBuddyFlags flags;
	JsonObject *obj = json_node_get_object(node);
	JsonNode *member = json_object_get_member(obj, "Member");
	if (!member)
		return FALSE;

	const gchar *presence;
	if (!parse_string(node, "Presence", &presence))
		return FALSE;
	if (!strcmp(presence, "notPresent"))
		flags = PURPLE_CBFLAGS_AWAY;
	else if (!strcmp(presence, "present"))
		flags = PURPLE_CBFLAGS_VOICE;
	else {
		printf("Unknown presence %s\n", presence);
		return FALSE;
	}
	if (!parse_string(member, "ProfileId", &id) ||
	    !parse_string(member, "FullName", &full_name))
		return FALSE;

	if (!g_hash_table_lookup(chat->members, id)) {
		struct chat_member *m = g_new0(struct chat_member, 1);
		m->id = g_strdup(id);
		m->full_name = g_strdup(full_name);
		g_hash_table_insert(chat->members, m->id, m);

		purple_conv_chat_add_user(PURPLE_CONV_CHAT(chat->conv), m->full_name,
					  NULL, flags, chat->msgs.members_done);
	} else {
		purple_conv_chat_user_set_flags(PURPLE_CONV_CHAT(chat->conv), full_name, flags);
	}
	return TRUE;
}

static gboolean chat_jugg_cb(struct chime_connection *cxn, gpointer _chat,
			     const gchar *klass, const gchar *type, JsonNode *record)
{
	struct chime_chat *chat = _chat;

	if (!strcmp(klass, "RoomMessage")) {
		const gchar *msg_id;
		if (!parse_string(record, "MessageId", &msg_id))
			return FALSE;

		if (chat->msgs.messages) {
			/* Still gathering messages. Add to the table, to avoid dupes */
			g_hash_table_insert(chat->msgs.messages, (gchar *)msg_id,
					    json_node_ref(record));
			return TRUE;
		}

		const gchar *msg_time;
		GTimeVal tv;
		if (!parse_time(record, "CreatedOn", &msg_time, &tv))
			return FALSE;

		chime_update_last_msg(cxn, TRUE, chat->room->id, msg_time, msg_id);

		chat_deliver_msg(cxn, &chat->msgs, record, tv.tv_sec);
		return TRUE;
	} else if (!strcmp(klass, "RoomMembership")) {
		/* What abpout removal? */
		return add_chat_member(chat, record);
	}
	return FALSE;
}

void chime_destroy_chat(struct chime_chat *chat)
{
	PurpleConnection *conn = chat->conv->account->gc;
	struct chime_connection *cxn = purple_connection_get_protocol_data(conn);
	struct chime_room *room = chat->room;
	int id = purple_conv_chat_get_id(PURPLE_CONV_CHAT(room->chat->conv));

	if (chat->msgs.soup_msg) {
		soup_session_cancel_message(cxn->soup_sess, chat->msgs.soup_msg, 1);
		chat->msgs.soup_msg = NULL;
	}
	if (chat->members_msg) {
		soup_session_cancel_message(cxn->soup_sess, chat->members_msg, 1);
		chat->members_msg = NULL;
	}
	chime_jugg_unsubscribe(cxn, room->channel, NULL, chat_jugg_cb, chat);

	serv_got_chat_left(conn, id);
	g_hash_table_remove(cxn->live_chats, GUINT_TO_POINTER(room->id));

	if (chat->msgs.messages)
		g_hash_table_destroy(chat->msgs.messages);
	if (chat->members)
		g_hash_table_destroy(chat->members);

	g_free(chat);
	room->chat = NULL;
	printf("Destroyed chat %p\n", chat);
}

static void one_member_cb(JsonArray *array, guint index_,
			  JsonNode *node, gpointer _chat)
{
	add_chat_member(_chat, node);
}

void fetch_chat_memberships(struct chime_connection *cxn, struct chime_chat *chat, const gchar *next_token);
static void fetch_members_cb(struct chime_connection *cxn, SoupMessage *msg, JsonNode *node, gpointer _chat)
{
	struct chime_chat *chat = _chat;
	const gchar *next_token;

	JsonObject *obj = json_node_get_object(node);
	JsonNode *members_node = json_object_get_member(obj, "RoomMemberships");
	JsonArray *members_array = json_node_get_array(members_node);

	chat->members_msg = NULL;
	json_array_foreach_element(members_array, one_member_cb, chat);

	if (parse_string(node, "NextToken", &next_token))
		fetch_chat_memberships(cxn, _chat, next_token);
	else {
		chat->msgs.members_done = TRUE;
		if (chat->msgs.msgs_done)
			chime_complete_messages(cxn, &chat->msgs);
	}
}

void fetch_chat_memberships(struct chime_connection *cxn, struct chime_chat *chat, const gchar *next_token)
{
	struct chime_room *room = chat->room;
	SoupURI *uri = soup_uri_new_printf(cxn->messaging_url, "/rooms/%s/memberships", room->id);

	soup_uri_set_query_from_fields(uri, "max-results", "50", next_token ? "next-token" : NULL, next_token, NULL);
	chat->members_msg = chime_queue_http_request(cxn, NULL, uri, fetch_members_cb, chat);
}

static void kill_member(gpointer _member)
{
	struct chat_member *member = _member;

	g_free(member->id);
	g_free(member->full_name);
	g_free(member);
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
	chat->conv = serv_got_joined_chat(conn, chat_id, room->name);
	g_hash_table_insert(cxn->live_chats, GUINT_TO_POINTER(chat_id), chat);

	chat->members = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, kill_member);
	chime_jugg_subscribe(cxn, room->channel, NULL, chat_jugg_cb, chat);

	chat->msgs.is_room = TRUE;
	chat->msgs.id = room->id;
	chat->msgs.cb = chat_deliver_msg;
	fetch_messages(cxn, &chat->msgs, NULL);
	fetch_chat_memberships(cxn, chat, NULL);
}

void chime_purple_chat_leave(PurpleConnection *conn, int id)
{
	struct chime_connection *cxn = purple_connection_get_protocol_data(conn);
	struct chime_chat *chat = g_hash_table_lookup(cxn->live_chats, GUINT_TO_POINTER(id));

	chime_destroy_chat(chat);
}

static void send_msg_cb(struct chime_connection *cxn, SoupMessage *msg, JsonNode *node, gpointer _chat)
{
       struct chime_chat *chat = _chat;

       /* Nothing to do o nsuccess */
       if (!SOUP_STATUS_IS_SUCCESSFUL(msg->status_code)) {
		gchar *err_msg = g_strdup_printf(_("Failed to deliver message (%d): %s"),
						 msg->status_code, msg->reason_phrase);
		purple_conversation_write(chat->conv, NULL, err_msg, PURPLE_MESSAGE_ERROR, time(NULL));
		g_free(err_msg);
       }
}

int chime_purple_chat_send(PurpleConnection *conn, int id, const char *message, PurpleMessageFlags flags)
{
	struct chime_connection *cxn = purple_connection_get_protocol_data(conn);
	struct chime_chat *chat = g_hash_table_lookup(cxn->live_chats, GUINT_TO_POINTER(id));
	int ret;

	/* For idempotency of requests. Not that we retry. */
	gchar *uuid = purple_uuid_random();

	JsonBuilder *jb = json_builder_new();
	jb = json_builder_new();
	jb = json_builder_begin_object(jb);
	jb = json_builder_set_member_name(jb, "Content");
	jb = json_builder_add_string_value(jb, message);
	jb = json_builder_set_member_name(jb, "ClientRequestToken");
	jb = json_builder_add_string_value(jb, uuid);
	jb = json_builder_end_object(jb);

	SoupURI *uri = soup_uri_new_printf(cxn->messaging_url, "/rooms/%s/messages", chat->room->id);
	if (chime_queue_http_request(cxn, json_builder_get_root(jb), uri, send_msg_cb, chat))
		ret = 0;
	else
		ret = -1;

	g_object_unref(jb);
	return ret;
}

static gboolean chat_demuxing_jugg_cb(struct chime_connection *cxn, gpointer _unused,
				      const gchar *klass, const gchar *type, JsonNode *record)
{
	const gchar *room_id;
	if (!parse_string(record, "RoomId", &room_id))
		return FALSE;

	struct chime_room *room = g_hash_table_lookup(cxn->rooms_by_id, room_id);
	if (!room)
		return FALSE;

	return chat_jugg_cb(cxn, room->chat, klass, type, record);
}


void chime_init_chats(struct chime_connection *cxn)
{
	chime_jugg_subscribe(cxn, cxn->device_channel, "RoomMessage", chat_demuxing_jugg_cb, cxn);
}

void chime_destroy_chats(struct chime_connection *cxn)
{
	chime_jugg_unsubscribe(cxn, cxn->device_channel, "RoomMessage", chat_demuxing_jugg_cb, cxn);
}
