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
#include "chime-connection-private.h"
#include "chime-room.h"

#include <libsoup/soup.h>

struct chime_chat {
	/* msgs first as it's a "subclass". Really ought to do proper GTypes here... */
	struct chime_msgs msgs;

	ChimeRoom *room;
	const gchar *id;

	PurpleConversation *conv;
	gboolean got_members;

	GRegex *mention_regex;

	GHashTable *sent_msgs;
};

/*
 * Examples:
 *
 * <@all|All members> becomes All members
 * <@present|Present members> becomes Present members
 * <@75f50e24-d59d-40e4-996b-6ba3ff3f371f|Surname, Name> becomes Surname, Name
 */
#define MENTION_PATTERN "&lt;@([\\w\\-]+)\\|(.*?)&gt;"
#define MENTION_REPLACEMENT "<b>\\2</b>"

/*
 * Returns whether `me` was mentioned in the Chime `message`, and allocates a
 * new string in `*parsed`.
 */
static int parse_inbound_mentions(ChimeConnection *cxn, GRegex *mention_regex, const char *message, char **parsed)
{
	*parsed = g_regex_replace(mention_regex, message, -1, 0, MENTION_REPLACEMENT, 0, NULL);
	return strstr(message, chime_connection_get_profile_id(cxn)) || strstr(message, "&lt;@all|") ||
		strstr(message, "&lt;@present|");
}

static void replace(gchar **dst, const gchar *a, const gchar *b)
{
       gchar **parts = g_strsplit(*dst, a, 0);
       gchar *replaced = g_strjoinv(b, parts);
       g_strfreev(parts);
       g_free(*dst);
       *dst = replaced;
}

/*
 * This will simple look for all chat members mentions and replace them with
 * the Chime format for mentioning. As a special case we expand "@all" and
 * "@present".
 */
static gchar *parse_outbound_mentions(ChimeRoom *room, const gchar *message)
{
	GList *members = chime_room_get_members(room);

	gchar *parsed = g_strdup(message);
	replace(&parsed, "@all", "<@all|All Members>");
	replace(&parsed, "@present", "<@present|Present Members>");
	while (members) {
		ChimeRoomMember *member = members->data;
		const gchar *id = chime_contact_get_profile_id(member->contact);
		const gchar *display_name = chime_contact_get_display_name(member->contact);

		gchar *chime_mention = g_strdup_printf("<@%s|%s>", id, display_name);
		replace(&parsed, chime_contact_get_display_name(member->contact), chime_mention);
		g_free(chime_mention);

		members = g_list_remove(members, member);
	}
	return parsed;
}

static void parse_incoming_msg(ChimeConnection *cxn, struct chime_chat *chat,
			       JsonNode *node, time_t msg_time)
{
	PurpleConnection *conn = chat->conv->account->gc;
	int id = purple_conv_chat_get_id(PURPLE_CONV_CHAT(chat->conv));
	const gchar *content, *sender;

	if (!parse_string(node, "Content", &content) ||
	    !parse_string(node, "Sender", &sender))
		return;

	const gchar *from = _("Unknown sender");
	int msg_flags;

	if (!strcmp(sender, chime_connection_get_profile_id(cxn))) {
		from = purple_connection_get_display_name(conn);
		msg_flags = PURPLE_MESSAGE_SEND;
	} else {
		ChimeContact *who = chime_connection_contact_by_id(cxn, sender);
		if (who)
			from = chime_contact_get_display_name(who);
		msg_flags = PURPLE_MESSAGE_RECV;
	}

	gchar *escaped = g_markup_escape_text(content, -1);

	gchar *parsed = NULL;
	if (parse_inbound_mentions(cxn, chat->mention_regex, escaped, &parsed) &&
	    (msg_flags & PURPLE_MESSAGE_RECV)) {
		// Presumably this will trigger a notification.
		msg_flags |= PURPLE_MESSAGE_NICK;
	}
	g_free(escaped);
	serv_got_chat_in(conn, id, from, msg_flags, parsed, msg_time);
	g_free(parsed);
}

static void chat_deliver_msg(ChimeConnection *cxn, struct chime_msgs *msgs,
			     JsonNode *node, time_t msg_time)
{
	struct chime_chat *chat = (struct chime_chat *)msgs; /* Really */
	const gchar *msg_id;

	/* Eliminate duplicates with outbound messages */
	if (parse_string(node, "MessageId", &msg_id) &&
	    g_hash_table_remove(chat->sent_msgs, msg_id))
		return;

	parse_incoming_msg(cxn, chat, node, msg_time);
}

static void on_room_membership(ChimeRoom *room, ChimeRoomMember *member, struct chime_chat *chat)
{
	const gchar *who = chime_contact_get_email(member->contact);

	if (!member->active) {
		if (purple_conv_chat_find_user(PURPLE_CONV_CHAT(chat->conv), who))
			purple_conv_chat_remove_user(PURPLE_CONV_CHAT(chat->conv), who, NULL);
		return;
	}

	PurpleConvChatBuddyFlags flags = 0;
	if (member->admin)
		flags |= PURPLE_CBFLAGS_OP;
	if (!member->present)
		flags |= PURPLE_CBFLAGS_AWAY;

	if (purple_conv_chat_find_user(PURPLE_CONV_CHAT(chat->conv), who))
		purple_conv_chat_user_set_flags(PURPLE_CONV_CHAT(chat->conv), who, flags);
	else {
		purple_conv_chat_add_user(PURPLE_CONV_CHAT(chat->conv), who,
					  NULL, flags, chat->msgs.members_done);
		PurpleConvChatBuddy *cbuddy = purple_conv_chat_cb_find(PURPLE_CONV_CHAT(chat->conv), who);
		if (cbuddy) {
			g_free(cbuddy->alias);
			cbuddy->alias = g_strdup(chime_contact_get_display_name(member->contact));
		}
	}
}

void chime_destroy_chat(struct chime_chat *chat)
{
	PurpleConnection *conn = chat->conv->account->gc;
	struct purple_chime *pc = purple_connection_get_protocol_data(conn);
	ChimeConnection *cxn = PURPLE_CHIME_CXN(conn);
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);
	ChimeRoom *room = chat->room;
	int id = purple_conv_chat_get_id(PURPLE_CONV_CHAT(chat->conv));

	if (chat->msgs.soup_msg) {
		soup_session_cancel_message(priv->soup_sess, chat->msgs.soup_msg, 1);
		chat->msgs.soup_msg = NULL;
	}
	g_signal_handlers_disconnect_matched(room, G_SIGNAL_MATCH_DATA,
					     0, 0, NULL, NULL, chat);
	chime_connection_close_room(cxn, chat->room);
	serv_got_chat_left(conn, id);
	g_hash_table_remove(pc->live_chats, GUINT_TO_POINTER(id));
	g_hash_table_remove(pc->chats_by_room, room);

	if (chat->msgs.messages)
		g_hash_table_destroy(chat->msgs.messages);
	if (chat->sent_msgs)
		g_hash_table_destroy(chat->sent_msgs);
	g_object_unref(chat->room);
	g_regex_unref(chat->mention_regex);
	g_free(chat);
	printf("Destroyed chat %p\n", chat);
}

static void on_room_members_done(ChimeRoom *room, struct chime_chat *chat)
{
	PurpleConnection *conn = chat->conv->account->gc;
	ChimeConnection *cxn = PURPLE_CHIME_CXN(conn);

	chat->msgs.members_done = TRUE;
	if (chat->msgs.msgs_done)
		chime_complete_messages(cxn, &chat->msgs);
}


static void on_room_message(ChimeRoom *room, JsonNode *node, struct chime_chat *chat)
{
	PurpleConnection *conn = chat->conv->account->gc;
	ChimeConnection *cxn = PURPLE_CHIME_CXN(conn);

	const gchar *msg_id;
	if (!parse_string(node, "MessageId", &msg_id))
		return;

	if (chat->msgs.messages) {
		/* Still gathering messages. Add to the table, to avoid dupes */
		g_hash_table_insert(chat->msgs.messages, (gchar *)msg_id,
				    json_node_ref(node));
		return;
	}

	const gchar *msg_time;
	GTimeVal tv;
	if (!parse_time(node, "CreatedOn", &msg_time, &tv))
		return;

	chime_update_last_msg(cxn, TRUE, chime_room_get_id(chat->room), msg_time, msg_id);

	chat_deliver_msg(cxn, &chat->msgs, node, tv.tv_sec);
}

static struct chime_chat *do_join_chat(PurpleConnection *conn, ChimeConnection *cxn, ChimeRoom *room)
{
	if (!room)
		return NULL;

	struct purple_chime *pc = purple_connection_get_protocol_data(conn);
	struct chime_chat *chat = g_hash_table_lookup(pc->chats_by_room, room);
	if (chat)
		return chat;

	chat = g_new0(struct chime_chat, 1);
	chat->room = g_object_ref(room);

	int chat_id = ++pc->chat_id;
	chat->conv = serv_got_joined_chat(conn, chat_id, chime_room_get_name(room));

	g_hash_table_insert(pc->live_chats, GUINT_TO_POINTER(chat_id), chat);
	g_hash_table_insert(pc->chats_by_room, room, chat);
	chat->sent_msgs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	chat->mention_regex = g_regex_new(MENTION_PATTERN, G_REGEX_EXTENDED, 0, NULL);

	chat->msgs.is_room = TRUE;
	chat->msgs.id = chime_room_get_id(room);
	chat->msgs.cb = chat_deliver_msg;

	const gchar *after = NULL;
	if (chime_read_last_msg(conn, CHIME_OBJECT(chat->room), &after, &chat->msgs.last_msg) &&
	    after && after[0])
		chat->msgs.last_msg_time = g_strdup(after);

	fetch_messages(cxn, &chat->msgs, NULL);

	g_signal_connect(room, "message", G_CALLBACK(on_room_message), chat);
	g_signal_connect(room, "members-done", G_CALLBACK(on_room_members_done), chat);
	g_signal_connect(room, "membership", G_CALLBACK(on_room_membership), chat);
	chime_connection_open_room(cxn, room);

	return chat;
}

void chime_purple_join_chat(PurpleConnection *conn, GHashTable *data)
{
	ChimeConnection *cxn = PURPLE_CHIME_CXN(conn);
	const gchar *roomid = g_hash_table_lookup(data, "RoomId");

	printf("join_chat %p %s %s\n", data, roomid, (gchar *)g_hash_table_lookup(data, "Name"));

	ChimeRoom *room = chime_connection_room_by_id(cxn, roomid);
	if (!room)
		return;
	do_join_chat(conn, cxn, room);
}

void chime_purple_chat_leave(PurpleConnection *conn, int id)
{
	struct purple_chime *pc = purple_connection_get_protocol_data(conn);
	struct chime_chat *chat = g_hash_table_lookup(pc->live_chats, GUINT_TO_POINTER(id));

	chime_destroy_chat(chat);
}

static void sent_msg_cb(GObject *source, GAsyncResult *result, gpointer _chat)
{
	struct chime_chat *chat = _chat;
	ChimeConnection *cxn = CHIME_CONNECTION(source);
	GError *error = NULL;

	JsonNode *msgnode = chime_connection_send_message_finish(cxn, result, &error);
	if (!msgnode) {
		purple_conversation_write(chat->conv, NULL, error->message, PURPLE_MESSAGE_ERROR, time(NULL));
		g_clear_error(&error);
		return;
	} else {
		const gchar *msg_time, *msg_id, *last_seen;
		GTimeVal tv, seen_tv;

		if (!parse_time(msgnode, "CreatedOn", &msg_time, &tv))
			tv.tv_sec = time(NULL);

		/* If we have already received a message at least this new before
		 * the response to the creation arrived, then don't deliver it to
		 * Pidgin again.... */
		if (chime_read_last_msg(purple_conversation_get_gc(chat->conv), CHIME_OBJECT(chat->room), &last_seen, NULL) &&
		    g_time_val_from_iso8601(last_seen, &seen_tv) &&
		    (seen_tv.tv_sec > tv.tv_sec ||
		     (seen_tv.tv_sec == tv.tv_sec && seen_tv.tv_usec >= tv.tv_usec)))
			return;

		/* If we're doing it, then stick it into the hash table so that
		 * chat_deliver_msg won't do it again when it does come back in. */
		if (parse_string(msgnode, "MessageId", &msg_id))
			g_hash_table_add(chat->sent_msgs, g_strdup(msg_id));

		parse_incoming_msg(cxn, chat, msgnode, tv.tv_sec);
	}
}

int chime_purple_chat_send(PurpleConnection *conn, int id, const char *message, PurpleMessageFlags flags)
{
	struct purple_chime *pc = purple_connection_get_protocol_data(conn);
	struct chime_chat *chat = g_hash_table_lookup(pc->live_chats, GUINT_TO_POINTER(id));

	/* Chime does not understand HTML. */
	gchar *unescaped = purple_unescape_html(message);

	/* Expand member names into the format Chime understands */
	gchar *expanded = parse_outbound_mentions(chat->room, unescaped);
	g_free(unescaped);

	chime_connection_send_message_async(pc->cxn, CHIME_OBJECT(chat->room), expanded, NULL, sent_msg_cb, chat);

	g_free(expanded);
	return 0;
}

void purple_chime_init_chats(struct purple_chime *pc)
{
	pc->live_chats = g_hash_table_new(g_direct_hash, g_direct_equal);
	pc->chats_by_room = g_hash_table_new(g_direct_hash, g_direct_equal);
}

void purple_chime_destroy_chats(struct purple_chime *pc)
{
	g_clear_pointer(&pc->live_chats, g_hash_table_unref);
	g_clear_pointer(&pc->chats_by_room, g_hash_table_unref);
}

static void on_chime_room_mentioned(ChimeConnection *cxn, ChimeRoom *room, JsonNode *node, PurpleConnection *conn)
{
	struct purple_chime *pc = purple_connection_get_protocol_data(conn);
	struct chime_chat *chat = g_hash_table_lookup(pc->chats_by_room, room);

	if (!chat)
		chat = do_join_chat(conn, cxn, room);
	if (chat)
		on_room_message(room, node, chat);
}

static void on_chime_new_room(ChimeConnection *cxn, ChimeRoom *room, PurpleConnection *conn)
{
	const gchar *id, *last_mentioned;
	GTimeVal mention_tv;

	/* If no LastMentioned or we can't parse it, nothing to do */
	g_object_get(room, "id", &id, "last-mentioned", &last_mentioned, NULL);
	if (!last_mentioned || !g_time_val_from_iso8601(last_mentioned, &mention_tv))
		return;

	const gchar *msg_time;
	GTimeVal msg_tv;

	if (chime_read_last_msg(conn, CHIME_OBJECT(room), &msg_time, NULL) &&
	    g_time_val_from_iso8601(msg_time, &msg_tv) &&
	    (mention_tv.tv_sec < msg_tv.tv_sec ||
	     (mention_tv.tv_sec == msg_tv.tv_sec && mention_tv.tv_usec <= msg_tv.tv_usec))) {
		/* LastMentioned is older than we've already seen. Nothing to do. */
		return;
	}

	/* We have been mentioned since we last looked at this room. Open it now. */
	do_join_chat(conn, cxn, room);
}

void purple_chime_init_chats_post(PurpleConnection *conn)
{
	struct purple_chime *pc = purple_connection_get_protocol_data(conn);

	chime_connection_foreach_room(pc->cxn, (ChimeRoomCB)on_chime_new_room, conn);

	g_signal_connect(pc->cxn, "room-mention", G_CALLBACK(on_chime_room_mentioned), conn);
}
