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
#include <debug.h>

#include "chime.h"
#include "chime-connection-private.h"

#include <libsoup/soup.h>

struct chime_im {
	PurpleConnection *conn;
	ChimeConversation *conv;
	ChimeContact *peer;
	gchar *last_msg_time;
	gchar *last_msg_id;

	GQueue *seen_msgs;
	GHashTable *msg_gather;
};

static gboolean is_msg_seen(GQueue *q, const gchar *id)
{
	return !!g_queue_find_custom(q, id, (GCompareFunc)strcmp);
}
static void mark_msg_seen(GQueue *q, const gchar *id)
{
	if (q->length == 10)
		g_free(g_queue_pop_tail(q));
	g_queue_push_head(q, g_strdup(id));
}

/* Called for all deliveries of incoming conversation messages, at startup and later */
static gboolean do_conv_deliver_msg(ChimeConnection *cxn, struct chime_im *im,
				    JsonNode *record, time_t msg_time)
{
	const gchar *sender, *message, *id;
	gint64 sys;

	if (!parse_string(record, "Sender", &sender) ||
	    !parse_string(record, "Content", &message) ||
	    !parse_string(record, "MessageId", &id) ||
	    !parse_int(record, "IsSystemMessage", &sys))
		return FALSE;

	if (is_msg_seen(im->seen_msgs, id))
		return TRUE;

	mark_msg_seen(im->seen_msgs, id);

	PurpleMessageFlags flags = PURPLE_MESSAGE_RECV;
	if (sys)
		flags |= PURPLE_MESSAGE_SYSTEM;

	if (strcmp(sender, chime_connection_get_profile_id(cxn))) {
		ChimeContact *contact = chime_connection_contact_by_id(cxn, sender);
		if (!contact)
			return FALSE;

		const gchar *email = chime_contact_get_email(contact);
		gchar *escaped = g_markup_escape_text(message, -1);
		serv_got_im(im->conn, email, escaped, flags, msg_time);
		g_free(escaped);
	} else {
		const gchar *email = chime_contact_get_email(im->peer);

		/* Ick, how do we inject a message from ourselves? */
		PurpleAccount *account = im->conn->account;
		PurpleConversation *pconv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM,
										  email, account);
		if (!pconv) {
			pconv = purple_conversation_new(PURPLE_CONV_TYPE_IM, account, email);
			if (!pconv) {
				purple_debug_error("chime", "NO CONV FOR %s\n", email);

				return FALSE;
			}
		}
		gchar *escaped = g_markup_escape_text(message, -1);
		purple_conversation_write(pconv, NULL, escaped, PURPLE_MESSAGE_SEND, msg_time);
		g_free(escaped);
	}

	return TRUE;
}

static void on_conv_msg(ChimeConversation *conv, JsonNode *record, struct chime_im *im)
{
	ChimeConnection *cxn = PURPLE_CHIME_CXN(im->conn);
	const gchar *id;
	if (!parse_string(record, "MessageId", &id))
		return;
	if (im->msg_gather) {
		/* Still gathering messages. Add to the table, to avoid dupes */
		g_hash_table_insert(im->msg_gather, (gchar *)id, json_node_ref(record));
		return;
	}
	GTimeVal tv;
	const gchar *created;
	if (!parse_time(record, "CreatedOn", &created, &tv))
		return;

	chime_update_last_msg(cxn, FALSE, chime_object_get_id(CHIME_OBJECT(im->conv)), created, id);

	do_conv_deliver_msg(cxn, im, record, tv.tv_sec);
}

static void on_conv_typing(ChimeConversation *conv, ChimeContact *contact, gboolean state, struct chime_im *im)
{
	const gchar *email = chime_contact_get_email(contact);

	if (state)
		serv_got_typing(im->conn, email, 0, PURPLE_TYPING);
	else
		serv_got_typing_stopped(im->conn, email);
}
/* Duplicated from messages.c for now until rooms are changed to match... */

struct msg_sort {
	GTimeVal tm;
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
		*l = g_list_insert_sorted(*l, ms, compare_ms);
	}
	return TRUE;
}

/* Once the message fetching is complete, we can play the fetched messages in order */
static void conv_msgs_flush(GObject *source, GAsyncResult *result, gpointer _im)
{
	ChimeConnection *cxn = CHIME_CONNECTION(source);
	struct chime_im *im = _im;

	GError *error = NULL;
	if (!chime_connection_create_conversation_finish(cxn, result, &error)) {
		purple_debug(PURPLE_DEBUG_ERROR, "chime", "Failed to fetch messages: %s\n", error->message);
		g_clear_error(&error);
	}

	GList *l = NULL;
	/* Sort messages by time */
	g_hash_table_foreach_remove(im->msg_gather, insert_queued_msg, &l);
	g_clear_pointer(&im->msg_gather, g_hash_table_destroy);

	while (l) {
		struct msg_sort *ms = l->data;
		JsonNode *node = ms->node;
		do_conv_deliver_msg(cxn, im, node, ms->tm.tv_sec);
		g_free(ms);
		l = g_list_remove(l, ms);

		/* Last message, note down the received time */
		if (!l) {
			const gchar *tm, *id;
			if (parse_string(node, "CreatedOn", &tm) &&
			    parse_string(node, "MessageId", &id))
				chime_update_last_msg(cxn, FALSE, chime_object_get_id(CHIME_OBJECT(im->conv)), tm, id);
		}
		json_node_unref(node);
	}
}

void on_chime_new_conversation(ChimeConnection *cxn, ChimeConversation *conv, PurpleConnection *conn)
{
	struct purple_chime *pc = purple_connection_get_protocol_data(conn);

	GList *members = chime_conversation_get_members(conv);
	if (g_list_length(members) != 2) {
		/* We don't support non-1:1 conversations yet. We need to handle them as 'chats'. */
		return;
	}
	struct chime_im *im = g_new0(struct chime_im, 1);
	im->conn = conn;
	im->conv = g_object_ref(conv);
	im->peer = members->data;
	im->seen_msgs = g_queue_new();
	if (!strcmp(chime_connection_get_profile_id(cxn), chime_object_get_id(CHIME_OBJECT(im->peer))))
		im->peer = members->next->data;
	g_list_free(members);
	g_object_ref(im->peer);
	purple_debug(PURPLE_DEBUG_INFO, "chime", "New conversation %s with %s\n", chime_object_get_id(CHIME_OBJECT(im->peer)),
		     chime_contact_get_email(im->peer));

	g_hash_table_insert(pc->ims_by_email, (void *)chime_contact_get_email(im->peer), im);

	g_signal_connect(conv, "typing", G_CALLBACK(on_conv_typing), im);
	g_signal_connect(conv, "message", G_CALLBACK(on_conv_msg), im);

	/* Do we need to fetch new messages? */
	const gchar *last_seen, *last_sent;

	last_sent = chime_conversation_get_last_sent(conv);

	if (!chime_read_last_msg(conn, CHIME_OBJECT(conv), &last_seen, &im->last_msg_id))
		last_seen = "1970-01-01T00:00:00.000Z";

	if (im->last_msg_id)
		mark_msg_seen(im->seen_msgs, im->last_msg_id);

	if (last_sent && strcmp(last_seen, last_sent)) {
		purple_debug(PURPLE_DEBUG_INFO, "chime", "Fetch conv messages for %s\n", chime_object_get_id(CHIME_OBJECT(im->peer)));

		im->msg_gather = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, (GDestroyNotify)json_node_unref);
		im->last_msg_time = g_strdup(last_seen);
		chime_connection_fetch_messages_async(PURPLE_CHIME_CXN(conn), CHIME_OBJECT(conv), NULL, last_seen, NULL, conv_msgs_flush, im);
	}
}

static void im_destroy(gpointer _im)
{
	struct chime_im *im = _im;

	g_signal_handlers_disconnect_matched(im->conv, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, im);
	g_free(im->last_msg_time);
	g_free(im->last_msg_id);
	g_queue_free_full(im->seen_msgs, g_free);
	if (im->msg_gather)
		g_hash_table_destroy(im->msg_gather);
	g_object_unref(im->conv);
	g_object_unref(im->peer);
	g_free(im);
}

void purple_chime_init_conversations(struct purple_chime *pc)
{
	pc->ims_by_email = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, im_destroy);
}

void purple_chime_destroy_conversations(struct purple_chime *pc)
{
	g_clear_pointer(&pc->ims_by_email, g_hash_table_destroy);
}

struct im_send_data {
	PurpleConnection *conn;
	struct chime_im *im;
	gchar *who;
	gchar *message;
	PurpleMessageFlags flags;
};

static void im_send_error(ChimeConnection *cxn, struct im_send_data *imd,
			  const gchar *format, ...)
{
	va_list args;

	va_start(args, format);
	gchar *msg = g_strdup_vprintf(format, args);
	va_end(args);

	PurpleConversation *pconv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_ANY,
									  imd->who,
									  imd->conn->account);
	if (pconv)
		purple_conversation_write(pconv, NULL, msg, PURPLE_MESSAGE_ERROR, time(NULL));

	g_free(msg);
}

unsigned int chime_send_typing(PurpleConnection *conn, const char *name, PurpleTypingState state)
{
	/* We can't get to PURPLE_TYPED unless we've already sent PURPLE_TYPING... */
	if (state == PURPLE_TYPED)
		return 0;

	struct purple_chime *pc = purple_connection_get_protocol_data(conn);
	struct chime_im *im = g_hash_table_lookup(pc->ims_by_email, name);
	if (!im)
		return 0;

	chime_conversation_send_typing(pc->cxn, im->conv, state == PURPLE_TYPING);

	return 0;
}

static void sent_im_cb(GObject *source, GAsyncResult *result, gpointer _imd)
{
	struct im_send_data *imd = _imd;
	ChimeConnection *cxn = CHIME_CONNECTION(source);
	GError *error = NULL;

	JsonNode *msgnode = chime_connection_send_message_finish(cxn, result, &error);

	const gchar *msg_id;
	if (msgnode) {
		if (!parse_string(msgnode, "MessageId", &msg_id))
			im_send_error(cxn, imd, _("Failed to send message"));
		json_node_unref(msgnode);
	} else {
		im_send_error(cxn, imd, error->message);
		g_clear_error(&error);
	}

	g_free(imd->who);
	g_free(imd->message);
	g_free(imd);
}


static void create_im_cb(GObject *source, GAsyncResult *result, gpointer _imd)
{
	ChimeConnection *cxn = CHIME_CONNECTION(source);
	struct im_send_data *imd = _imd;
	ChimeConversation *conv = chime_connection_create_conversation_finish(cxn, result, NULL);
	struct purple_chime *pc = purple_connection_get_protocol_data(imd->conn);

	if (conv) {
		g_object_unref(conv);

		imd->im = g_hash_table_lookup(pc->ims_by_email, imd->who);
		if (!imd->im) {
			purple_debug(PURPLE_DEBUG_INFO, "chime", "No im for %s\n", imd->who);
			goto bad;
		}

		chime_connection_send_message_async(cxn, CHIME_OBJECT(imd->im->conv), imd->message, NULL, sent_im_cb, imd);
		return;
	}
 bad:
	im_send_error(cxn, imd, _("Failed to create IM conversation"));
	g_free(imd->who);
	g_free(imd->message);
	g_free(imd);
}

static void autocomplete_im_cb(GObject *source, GAsyncResult *result, gpointer _imd)
{
	ChimeConnection *cxn = CHIME_CONNECTION(source);
	struct im_send_data *imd = _imd;
	GSList *contacts = chime_connection_autocomplete_contact_finish(cxn, result, NULL);

	while (contacts) {
		ChimeContact *contact = contacts->data;
		if (!strcmp(imd->who, chime_contact_get_email(contact))) {
			GSList *l = g_slist_append(NULL, contact);
			chime_connection_create_conversation_async(cxn, l, NULL, create_im_cb, imd);
			g_slist_free_1(l);
			g_slist_free_full(contacts, g_object_unref);
			return;
		}
		g_object_unref(contact);
		contacts = g_slist_remove(contacts, contact);
	}

	im_send_error(cxn, imd, _("Failed to find user"));
	g_free(imd->who);
	g_free(imd->message);
	g_free(imd);
}

int chime_purple_send_im(PurpleConnection *gc, const char *who, const char *message, PurpleMessageFlags flags)
{
	struct purple_chime *pc = purple_connection_get_protocol_data(gc);

	struct im_send_data *imd = g_new0(struct im_send_data, 1);
	imd->conn = gc;
	imd->message = purple_unescape_html(message);
	imd->who = g_strdup(who);
	imd->flags = flags;

	imd->im = g_hash_table_lookup(pc->ims_by_email, who);
	if (imd->im) {
		chime_connection_send_message_async(pc->cxn, CHIME_OBJECT(imd->im->conv), imd->message, NULL, sent_im_cb, imd);
		return 0;
	}

	ChimeContact *contact = chime_connection_contact_by_email(pc->cxn, who);
	if (contact) {
		GSList *l = g_slist_append(NULL, contact);
		chime_connection_create_conversation_async(pc->cxn, l, NULL, create_im_cb, imd);
		g_slist_free_1(l);
		return 0;
	}

	chime_connection_autocomplete_contact_async(pc->cxn, who, NULL, autocomplete_im_cb, imd);
	return 0;
}

