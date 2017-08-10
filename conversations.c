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
	struct chime_msgs msgs;

	PurpleConnection *conn;
	ChimeConversation *conv;
	ChimeContact *peer;

	GHashTable *sent_msgs;
};

/* Called for all deliveries of incoming conversation messages, at startup and later */
static gboolean do_conv_deliver_msg(ChimeConnection *cxn, struct chime_im *im,
				    JsonNode *record, time_t msg_time)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);
	const gchar *sender, *message;
	gint64 sys;

	if (!parse_string(record, "Sender", &sender) ||
	    !parse_string(record, "Content", &message) ||
	    !parse_int(record, "IsSystemMessage", &sys))
		return FALSE;

	PurpleMessageFlags flags = PURPLE_MESSAGE_RECV;
	if (sys)
		flags |= PURPLE_MESSAGE_SYSTEM;

	if (strcmp(sender, priv->profile_id)) {
		ChimeContact *contact = g_hash_table_lookup(priv->contacts.by_id,
							    sender);
		if (!contact)
			return FALSE;

		const gchar *email = chime_contact_get_email(contact);
		gchar *escaped = g_markup_escape_text(message, -1);
		serv_got_im(im->conn, email, escaped, flags, msg_time);
		g_free(escaped);
	} else {
		const gchar *msg_id;
		if (parse_string(record, "MessageId", &msg_id) &&
		    g_hash_table_remove(im->sent_msgs, msg_id)) {
			/* This was a message sent from this client. No need to display it. */
			return TRUE;
		}
		const gchar *email = chime_contact_get_email(im->peer);

		/* Ick, how do we inject a message from ourselves? */
		PurpleAccount *account = im->conn->account;
		PurpleConversation *pconv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM,
										  email, account);
		if (!pconv) {
			pconv = purple_conversation_new(PURPLE_CONV_TYPE_IM, account, email);
			if (!pconv) {
				printf("\n***** NO CONV FOR %s\n", email);

				return FALSE;
			}
		}
		gchar *escaped = g_markup_escape_text(message, -1);
		purple_conversation_write(pconv, NULL, escaped, PURPLE_MESSAGE_SEND, msg_time);
		g_free(escaped);
	}

	return TRUE;
}

/* Callback from message-gathering on startup */
static void conv_deliver_msg(ChimeConnection *cxn, struct chime_msgs *msgs,
			     JsonNode *node, time_t msg_time)
{
	struct chime_im *im = (struct chime_im *)msgs;

	do_conv_deliver_msg(cxn, im, node, msg_time);
}


static void on_conv_msg(ChimeConversation *conv, JsonNode *record, struct chime_im *im)
{
	ChimeConnection *cxn = PURPLE_CHIME_CXN(im->conn);
	const gchar *id;
	if (!parse_string(record, "MessageId", &id))
		return;
	if (im->msgs.messages) {
		/* Still gathering messages. Add to the table, to avoid dupes */
		g_hash_table_insert(im->msgs.messages, (gchar *)id, json_node_ref(record));
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

void on_chime_new_conversation(ChimeConnection *cxn, ChimeConversation *conv, PurpleConnection *conn)
{
	struct purple_chime *pc = purple_connection_get_protocol_data(conn);
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	GList *members = chime_conversation_get_members(conv);
	if (g_list_length(members) != 2) {
		/* We don't support non-1:1 conversations yet. We need to handle them as 'chats'. */
		return;
	}
	struct chime_im *im = g_new0(struct chime_im, 1);
	im->msgs.id = chime_object_get_id(CHIME_OBJECT(conv));
	im->msgs.members_done = TRUE;
	im->msgs.cb = conv_deliver_msg;
	im->conn = conn;
	im->sent_msgs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	im->conv = g_object_ref(conv);
	im->peer = members->data;
	if (!strcmp(priv->profile_id, chime_object_get_id(CHIME_OBJECT(im->peer))))
		im->peer = members->next->data;
	g_list_free(members);
	g_object_ref(im->peer);
	purple_debug(PURPLE_DEBUG_INFO, "chime", "New conversation %s with %s\n", chime_object_get_id(CHIME_OBJECT(im->peer)),
		     chime_contact_get_email(im->peer));

	g_hash_table_insert(pc->im_conversations_by_peer_id,
			    (void *)chime_object_get_id(CHIME_OBJECT(im->peer)), im);

	g_signal_connect(conv, "typing", G_CALLBACK(on_conv_typing), im);
	g_signal_connect(conv, "message", G_CALLBACK(on_conv_msg), im);

	/* Do we need to fetch new messages? */
	const gchar *last_seen, *last_sent;

	last_sent = chime_conversation_get_last_sent(conv);

	if (!chime_read_last_msg(conn, FALSE, chime_object_get_id(CHIME_OBJECT(conv)), &last_seen, NULL))
		last_seen = "1970-01-01T00:00:00.000Z";

	if (last_sent && strcmp(last_seen, last_sent)) {
		purple_debug(PURPLE_DEBUG_INFO, "chime", "Fetch conv messages for %s\n", im->msgs.id);

		const gchar *after = NULL;
		if (chime_read_last_msg(conn, FALSE, im->msgs.id, &after, &im->msgs.last_msg) &&
		    after && after[0])
			im->msgs.last_msg_time = g_strdup(after);
		fetch_messages(PURPLE_CHIME_CXN(im->conn), &im->msgs, NULL);
	}
}

static void im_destroy(gpointer _im)
{
	struct chime_im *im = _im;

	g_hash_table_destroy(im->sent_msgs);
	g_signal_handlers_disconnect_matched(im->conv, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, im);
	g_object_unref(im->conv);
	g_object_unref(im->peer);
	g_free(im);
}

void purple_chime_init_conversations(struct purple_chime *pc)
{
	pc->im_conversations_by_peer_id = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, im_destroy);
}

void purple_chime_destroy_conversations(struct purple_chime *pc)
{
	g_clear_pointer(&pc->im_conversations_by_peer_id, g_hash_table_destroy);
}

struct im_send_data {
	PurpleConnection *conn;
	struct chime_im *im;
	gchar *who;
	gchar *message;
	PurpleMessageFlags flags;
};

static void im_error(ChimeConnection *cxn, struct im_send_data *imd,
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

static void send_im_cb(ChimeConnection *cxn, SoupMessage *msg, JsonNode *node, gpointer _imd)
{
	struct im_send_data *imd = _imd;

	/* Nothing to do o nsuccess */
	if (!SOUP_STATUS_IS_SUCCESSFUL(msg->status_code)) {
		im_error(cxn, imd, _("Failed to send message(%d): %s\n"),
			 msg->status_code, msg->reason_phrase);
	} else {
		JsonObject *obj = json_node_get_object(node);
		JsonNode *node = json_object_get_member(obj, "Message");
		const gchar *msg_id;
		if (node && parse_string(node, "MessageId", &msg_id)) {
			g_hash_table_add(imd->im->sent_msgs, g_strdup(msg_id));
		}
	}
	g_free(imd->message);
	g_free(imd);
}

unsigned int chime_send_typing(PurpleConnection *conn, const char *name, PurpleTypingState state)
{
	/* We can't get to PURPLE_TYPED unless we've already sent PURPLE_TYPING... */
	if (state == PURPLE_TYPED)
		return 0;

	struct purple_chime *pc = purple_connection_get_protocol_data(conn);
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (pc->cxn);

	ChimeContact *contact = g_hash_table_lookup(priv->contacts.by_name, name);
	if (!contact)
		return 0;

	const gchar *id = chime_contact_get_profile_id(contact);
	struct chime_im *imd = g_hash_table_lookup(pc->im_conversations_by_peer_id, id);
	if (!imd)
		return 0;

	chime_conversation_send_typing(pc->cxn, imd->conv, state == PURPLE_TYPING);

	return 0;
}

static int send_im(ChimeConnection *cxn, struct im_send_data *imd)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	/* For idempotency of requests. Not that we retry. */
	gchar *uuid = purple_uuid_random();

	JsonBuilder *jb = json_builder_new();
	jb = json_builder_begin_object(jb);
	jb = json_builder_set_member_name(jb, "Content");
	jb = json_builder_add_string_value(jb, imd->message);
	jb = json_builder_set_member_name(jb, "ClientRequestToken");
	jb = json_builder_add_string_value(jb, uuid);
	jb = json_builder_end_object(jb);

	int ret;
	SoupURI *uri = soup_uri_new_printf(priv->messaging_url, "/conversations/%s/messages",
					   chime_object_get_id(CHIME_OBJECT(imd->im->conv)));
	JsonNode *node = json_builder_get_root(jb);
	if (chime_connection_queue_http_request(cxn, node, uri, "POST", send_im_cb, imd))
		ret = 1;
	else {
		ret = -1;
		g_free(imd->who);
		g_free(imd->message);
		g_free(imd);
	}

	json_node_unref(node);
	g_object_unref(jb);
	return ret;
}

static void conv_create_cb(ChimeConnection *cxn, SoupMessage *msg, JsonNode *node, gpointer _imd)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);
	struct im_send_data *imd = _imd;
	struct purple_chime *pc = purple_connection_get_protocol_data(imd->conn);

	if (SOUP_STATUS_IS_SUCCESSFUL(msg->status_code)) {
		JsonObject *obj = json_node_get_object(node);
		node = json_object_get_member(obj, "Conversation");
		if (!node)
			goto bad;

		if (!chime_connection_parse_conversation(cxn, node, NULL)) {
			printf("No conversation");
			goto bad;
		}

		ChimeContact *contact = g_hash_table_lookup(priv->contacts.by_name, imd->who);
		if (!contact) {
			printf("no contact for %s\n", imd->who);
			goto bad;
		}

		const gchar *id = chime_contact_get_profile_id(contact);
		imd->im = g_hash_table_lookup(pc->im_conversations_by_peer_id, id);
		if (!imd->im) {
			printf("No im for %s\n", id);
			goto bad;
		}

		send_im(cxn, imd);
		return;
	}
 bad:
	im_error(cxn, imd, _("Failed to create IM conversation"));
	g_free(imd->who);
	g_free(imd->message);
	g_free(imd);
}

static int create_im_conv(ChimeConnection *cxn, struct im_send_data *im, const gchar *profile_id)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);
	JsonBuilder *jb = json_builder_new();
	jb = json_builder_new();
	jb = json_builder_begin_object(jb);
	jb = json_builder_set_member_name(jb, "ProfileIds");
	jb = json_builder_begin_array(jb);
	jb = json_builder_add_string_value(jb, profile_id);
	jb = json_builder_end_array(jb);
	jb = json_builder_end_object(jb);

	int ret;
	SoupURI *uri = soup_uri_new_printf(priv->messaging_url, "/conversations");
	JsonNode *node = json_builder_get_root(jb);
	if (chime_connection_queue_http_request(cxn, node, uri, "POST", conv_create_cb, im))
		ret = 1;
	else {
		ret = -1;
		g_free(im->who);
		g_free(im->message);
		g_free(im);
	}

	json_node_unref(node);
	g_object_unref(jb);
	return ret;
}

static void autocomplete_im_cb(ChimeConnection *cxn, SoupMessage *msg, JsonNode *node, gpointer _imd)
{
	struct im_send_data *imd = _imd;

	if (SOUP_STATUS_IS_SUCCESSFUL(msg->status_code)) {
		JsonArray *arr = json_node_get_array(node);

		guint i, len = json_array_get_length(arr);
		for (i = 0; i < len; i++) {
			JsonNode *node = json_array_get_element(arr, i);
			const gchar *email, *profile_id;

			if (parse_string(node, "email", &email) &&
			    parse_string(node, "id", &profile_id) &&
			    !strcmp(imd->who, email)) {
				create_im_conv(cxn, imd, profile_id);
				return;
			}
		}
	}
	im_error(cxn, imd, _("Failed to find user"));
	g_free(imd->who);
	g_free(imd->message);
	g_free(imd);
}

int chime_purple_send_im(PurpleConnection *gc, const char *who, const char *message, PurpleMessageFlags flags)
{
	ChimeConnection *cxn = PURPLE_CHIME_CXN(gc);
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);
	struct purple_chime *pc = purple_connection_get_protocol_data(gc);
	struct im_send_data *imd = g_new0(struct im_send_data, 1);
	imd->conn = gc;
	imd->message = purple_unescape_html(message);
	imd->who = g_strdup(who);
	imd->flags = flags;

	ChimeContact *contact = g_hash_table_lookup(priv->contacts.by_name, who);
	if (contact) {
		const gchar *id = chime_contact_get_profile_id(contact);
		imd->im = g_hash_table_lookup(pc->im_conversations_by_peer_id, id);
		if (imd->im)
			return send_im(cxn, imd);

		return create_im_conv(cxn, imd, id);
	}

	SoupURI *uri = soup_uri_new_printf(priv->contacts_url, "/registered_auto_completes");
	JsonBuilder *jb = json_builder_new();
	jb = json_builder_begin_object(jb);
	jb = json_builder_set_member_name(jb, "q");
	jb = json_builder_add_string_value(jb, who);
	jb = json_builder_end_object(jb);

	int ret;
	JsonNode *node = json_builder_get_root(jb);
	if (chime_connection_queue_http_request(cxn, node, uri, "POST", autocomplete_im_cb, imd))
		ret = 1;
	else {
		ret = -1;
		g_free(imd->who);
		g_free(imd->message);
		g_free(imd);
	}

	json_node_unref(node);
	g_object_unref(jb);
	return ret;
}

