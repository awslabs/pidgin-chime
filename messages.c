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
#include <debug.h>

#include "chime.h"

static void chime_update_last_msg(ChimeConnection *cxn, struct chime_msgs *msgs,
				  const gchar *msg_time, const gchar *msg_id);

static void mark_msg_seen(GQueue *q, const gchar *id)
{
	if (q->length == 10)
		g_free(g_queue_pop_tail(q));
	g_queue_push_head(q, g_strdup(id));
}
static gboolean is_msg_unseen(GQueue *q, const gchar *id)
{
	if (g_queue_find_custom(q, id, (GCompareFunc)strcmp))
		return FALSE;
	mark_msg_seen(q, id);
	return TRUE;
}

struct msg_sort {
	GTimeVal tm;
	const gchar *id;
	JsonNode *node;
};

static gint compare_ms(gconstpointer _a, gconstpointer _b)
{
	const struct msg_sort *a = _a;
	const struct msg_sort *b = _b;

	if (a->tm.tv_sec > b->tm.tv_sec)
		return 1;
	if (a->tm.tv_sec == b->tm.tv_sec &&
	    a->tm.tv_usec > b->tm.tv_usec)
		return 1;
	return 0;
}

static int insert_queued_msg(gpointer _id, gpointer _node, gpointer _list)
{
	const gchar *str;
	GList **l = _list;

	if (parse_string(_node, "CreatedOn", &str)) {
		struct msg_sort *ms = g_new0(struct msg_sort, 1);
		if (!g_time_val_from_iso8601(str, &ms->tm)) {
			g_free(ms);
			return TRUE;
		}
		ms->node = json_node_ref(_node);
		ms->id = _id;
		*l = g_list_insert_sorted(*l, ms, compare_ms);
	}
	return TRUE;
}

void chime_complete_messages(ChimeConnection *cxn, struct chime_msgs *msgs)
{
	GList *l = NULL;

	/* Sort messages by time */
	g_hash_table_foreach_remove(msgs->msg_gather, insert_queued_msg, &l);
	g_clear_pointer(&msgs->msg_gather, g_hash_table_destroy);

	while (l) {
		struct msg_sort *ms = l->data;
		const gchar *id = ms->id;
		JsonNode *node = ms->node;
		if (is_msg_unseen(msgs->seen_msgs, id))
			msgs->cb(cxn, msgs, node, ms->tm.tv_sec);
		g_free(ms);
		l = g_list_remove(l, ms);

		/* Last message, note down the received time */
		if (!l && !msgs->msgs_failed) {
			const gchar *tm;
			if (parse_string(node, "CreatedOn", &tm))
				chime_update_last_msg(cxn, msgs, tm, id);
		}
		json_node_unref(node);
	}
}

static gboolean msg_newer(JsonNode *old, JsonNode *new)
{
	const gchar *old_updated = NULL, *new_updated = NULL;

	if (!parse_string(new, "UpdatedOn", &new_updated))
		return FALSE;
	if (!parse_string(old, "UpdatedOn", &old_updated))
		return TRUE;

	GTimeVal old_tv, new_tv;
	if (!g_time_val_from_iso8601(new_updated, &new_tv) ||
	    !g_time_val_from_iso8601(old_updated, &old_tv))
		return FALSE;

	if (new_tv.tv_sec > old_tv.tv_sec ||
	    (new_tv.tv_sec == old_tv.tv_sec && new_tv.tv_usec > old_tv.tv_usec))
		return TRUE;

	return FALSE;
}

static void on_message_received(ChimeObject *obj, JsonNode *node, struct chime_msgs *msgs)
{
	ChimeConnection *cxn = PURPLE_CHIME_CXN(msgs->conn);
	const gchar *id;
	if (!parse_string(node, "MessageId", &id))
		return;
	if (msgs->msg_gather) {
		/* Still gathering messages. Add to the table, to avoid dupes */
		JsonNode *old_node = g_hash_table_lookup(msgs->msg_gather, id);
		if (old_node) {
			if (!msg_newer(node, old_node))
				return;
			/* Remove first because the key belongs to the value */
			g_hash_table_remove(msgs->msg_gather, id);
		}
		g_hash_table_insert(msgs->msg_gather, (gchar *)id, json_node_ref(node));
		return;
	}
	GTimeVal tv;
	const gchar *created;
	if (!parse_time(node, "CreatedOn", &created, &tv))
		return;

	if (!msgs->msgs_failed)
		chime_update_last_msg(cxn, msgs, created, id);

	if (is_msg_unseen(msgs->seen_msgs, id))
		msgs->cb(cxn, msgs, node, tv.tv_sec);
}

/* Once the message fetching is complete, we can play the fetched messages in order */
static void fetch_msgs_cb(GObject *source, GAsyncResult *result, gpointer _msgs)
{
	ChimeConnection *cxn = CHIME_CONNECTION(source);
	struct chime_msgs *msgs = _msgs;

	GError *error = NULL;
	if (!chime_connection_create_conversation_finish(cxn, result, &error)) {
		purple_debug(PURPLE_DEBUG_ERROR, "chime", "Failed to fetch messages: %s\n", error->message);
		g_clear_error(&error);

		/* Don't update the 'last seen'. Better luck next time... */
		msgs->msgs_failed = TRUE;
	}
	msgs->msgs_done = TRUE;
	if (msgs->members_done)
		chime_complete_messages(cxn, msgs);
}

static void on_room_members_done(ChimeRoom *room, struct chime_msgs *msgs)
{
	ChimeConnection *cxn = PURPLE_CHIME_CXN(msgs->conn);

	msgs->members_done = TRUE;
	if (msgs->msgs_done)
		chime_complete_messages(cxn, msgs);
}

static void on_last_sent_updated(ChimeObject *obj, GParamSpec *ignored, struct chime_msgs *msgs)
{
	gchar *last_sent;
	g_object_get(obj, "last-sent", &last_sent, NULL);

	if (g_strcmp0(last_sent, msgs->last_seen)) {
		purple_debug(PURPLE_DEBUG_INFO, "chime", "Fetch messages for %s; LastSent updated to %s\n",
			     chime_object_get_id(msgs->obj), last_sent);

		chime_connection_fetch_messages_async(PURPLE_CHIME_CXN(msgs->conn), obj, NULL, msgs->last_seen, NULL, fetch_msgs_cb, msgs);
		msgs->msgs_done = FALSE;
		msgs->msg_gather = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, (GDestroyNotify)json_node_unref);
	}

	g_free(last_sent);
}

void init_msgs(PurpleConnection *conn, struct chime_msgs *msgs, ChimeObject *obj, chime_msg_cb cb, const gchar *name, JsonNode *first_msg)
{
	msgs->conn = conn;
	msgs->obj = g_object_ref(obj);
	msgs->cb = cb;
	msgs->seen_msgs = g_queue_new();

	const gchar *last_seen;
	gchar *last_id = NULL;
	if (!chime_read_last_msg(conn, obj, &last_seen, &last_id))
		last_seen = "1970-01-01T00:00:00.000Z";
	msgs->last_seen = g_strdup(last_seen);

	if (last_id) {
		mark_msg_seen(msgs->seen_msgs, last_id);
		g_free(last_id);
	}

	g_signal_connect(obj, "notify::last-sent", G_CALLBACK(on_last_sent_updated), msgs);
	g_signal_connect(obj, "message", G_CALLBACK(on_message_received), msgs);

	if (CHIME_IS_ROOM(obj)) {
		g_signal_connect(obj, "members-done", G_CALLBACK(on_room_members_done), msgs);
		/* Always fetch messages for rooms since we won't have been told of any
		 * updates to LastSent anyway. */
	} else {
		msgs->members_done = TRUE;

		/* Do we need to fetch new messages? */
		gchar *last_sent;
		g_object_get(obj, "last-sent", &last_sent, NULL);

		if (!last_sent || ! strcmp(last_seen, last_sent))
			msgs->msgs_done = TRUE;

		g_free(last_sent);
	}

	if (!msgs->msgs_done) {
		purple_debug(PURPLE_DEBUG_INFO, "chime", "Fetch messages for %s\n", name);
		chime_connection_fetch_messages_async(PURPLE_CHIME_CXN(conn), obj, NULL, last_seen, NULL, fetch_msgs_cb, msgs);
	}

	if (!msgs->msgs_done || !msgs->members_done)
		msgs->msg_gather = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, (GDestroyNotify)json_node_unref);

	if (first_msg)
		on_message_received(obj, first_msg, msgs);
}

void cleanup_msgs(struct chime_msgs *msgs)
{
	g_queue_free_full(msgs->seen_msgs, g_free);
	if (msgs->msg_gather)
		g_hash_table_destroy(msgs->msg_gather);
	/* Caller disconnects all signals with 'msgs' as user_data */
	g_clear_pointer(&msgs->last_seen, g_free);
	g_clear_object(&msgs->obj);
}

static void chime_update_last_msg(ChimeConnection *cxn, struct chime_msgs *msgs,
				  const gchar *msg_time, const gchar *msg_id)
{
	gchar *key = g_strdup_printf("last-%s-%s",
				     CHIME_IS_ROOM(msgs->obj) ? "room" : "conversation",
				     chime_object_get_id(msgs->obj));
	gchar *val = g_strdup_printf("%s|%s", msg_id, msg_time);

	purple_account_set_string(cxn->prpl_conn->account, key, val);
	g_free(key);
	g_free(val);

	g_free(msgs->last_seen);
	msgs->last_seen = g_strdup(msg_time);

	msgs->unseen = TRUE;
}

/* WARE! msg_id is allocated, msg_time is const */
gboolean chime_read_last_msg(PurpleConnection *conn, ChimeObject *obj,
			     const gchar **msg_time, gchar **msg_id)
{
	gchar *key = g_strdup_printf("last-%s-%s", CHIME_IS_ROOM(obj) ? "room" : "conversation", chime_object_get_id(obj));
	const gchar *val = purple_account_get_string(conn->account, key, NULL);
	g_free(key);

	if (!val || !val[0])
		return FALSE;

	*msg_time = strrchr(val, '|');
	if (!*msg_time) {
		/* Only a date, no msgid */
		*msg_time = val;
		if (msg_id)
			*msg_id = NULL;
		return TRUE;
	}

	if (msg_id)
		*msg_id = g_strndup(val, *msg_time - val);
	(*msg_time)++; /* Past the | */

	return TRUE;
}

static void chime_conv_updated_cb(PurpleConversation *conv, PurpleConvUpdateType type,
				  PurpleConnection *conn)
{
	if (conv->account != conn->account)
		return;

	int unseen_count = GPOINTER_TO_INT(purple_conversation_get_data(conv, "unseen-count"));

	purple_debug(PURPLE_DEBUG_INFO, "chime",
		     "Conversation '%s' updated, type %d, unseen %d\n",
		     conv->name, type, unseen_count);

	if (type != PURPLE_CONV_UPDATE_UNSEEN)
		return;

	struct purple_chime *pc = purple_connection_get_protocol_data(conn);
	struct chime_msgs *msgs = NULL;

	if (purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_CHAT) {
		int id = purple_conv_chat_get_id(PURPLE_CONV_CHAT(conv));
		msgs = g_hash_table_lookup(pc->live_chats, GUINT_TO_POINTER(id));
	} else if (purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_IM) {
		msgs = g_hash_table_lookup(pc->ims_by_email, conv->name);
	}

	if (!msgs || !msgs->unseen)
		return;

	if (unseen_count)
		return;

	const gchar *msg_id = g_queue_peek_head(msgs->seen_msgs);
	g_return_if_fail(msg_id);

	chime_connection_update_last_read_async(PURPLE_CHIME_CXN(conn), msgs->obj, msg_id, NULL, NULL, NULL);
	msgs->unseen = FALSE;
}


void purple_chime_init_messages(PurpleConnection *conn)
{
	purple_signal_connect(purple_conversations_get_handle(),
			      "conversation-updated", conn,
			      PURPLE_CALLBACK(chime_conv_updated_cb), conn);
}

void purple_chime_destroy_messages(PurpleConnection *conn)
{
	purple_signal_disconnect(purple_conversations_get_handle(),
				 "conversation-updated", conn,
				 PURPLE_CALLBACK(chime_conv_updated_cb));
}
