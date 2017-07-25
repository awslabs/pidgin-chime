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

#include "chime.h"
#include "chime-connection-private.h"

#include <libsoup/soup.h>

struct chime_conversation {
	struct chime_msgs *msgs;

	ChimeConnection *cxn;

	gchar *channel;
	gchar *id;
	gchar *name;
	gchar *visibility;
	gboolean favourite;

	GHashTable *members;
	GHashTable *sent_msgs;
};
static gboolean conv_membership_jugg_cb(ChimeConnection *cxn, gpointer _unused, JsonNode *record);
static gboolean conv_typing_jugg_cb(ChimeConnection *cxn, gpointer _unused, JsonNode *record);

/* Called for all deliveries of incoming conversation messages, at startup and later */
static gboolean do_conv_deliver_msg(ChimeConnection *cxn, struct chime_conversation *conv,
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

	if (g_hash_table_size(conv->members) != 2) {
		/* Only 1:1 IM so far; representing multi-party conversations as chats comes later */
		return FALSE;
	}

	if (strcmp(sender, priv->profile_id)) {
		ChimeContact *contact = g_hash_table_lookup(priv->contacts.by_id,
							    sender);
		if (!contact)
			return FALSE;

		const gchar *email = chime_contact_get_email(contact);
		gchar *escaped = g_markup_escape_text(message, -1);
		serv_got_im(cxn->prpl_conn, email, escaped, flags, msg_time);
		g_free(escaped);
	} else {
		const gchar *msg_id;
		if (parse_string(record, "MessageId", &msg_id) &&
		    g_hash_table_remove(conv->sent_msgs, msg_id)) {
			/* This was a message sent from this client. No need to display it. */
			return TRUE;
		}
		const gchar **member_ids;
		int peer;

		member_ids = (const gchar **)g_hash_table_get_keys_as_array(conv->members, NULL);
		if (!strcmp(member_ids[0], priv->profile_id))
			peer = 1;
		else
			peer = 0;

		ChimeContact *contact = g_hash_table_lookup(priv->contacts.by_id,
							    member_ids[peer]);
		g_free(member_ids);
		if (!contact)
			return FALSE;

		const gchar *email = chime_contact_get_email(contact);

		/* Ick, how do we inject a message from ourselves? */
		PurpleAccount *account = cxn->prpl_conn->account;
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
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);
	struct chime_conversation *conv = g_hash_table_lookup(priv->conversations_by_id, msgs->id);

	do_conv_deliver_msg(cxn, conv, node, msg_time);
}

static void fetch_conversation_messages(struct chime_conversation *conv)
{
	if (conv->msgs) {
		if (conv->msgs->msgs_done)
			conv->msgs->msgs_done = FALSE;
		else
			return; /* Already in progress */
	} else {
		conv->msgs = g_new0(struct chime_msgs, 1);
		conv->msgs->id = conv->id;
		conv->msgs->members_done = TRUE;
		conv->msgs->cb = conv_deliver_msg;
	}
	printf("Fetch conv messages for %s\n", conv->id);
	fetch_messages(conv->cxn, conv->msgs, NULL);
}

static struct chime_conversation *one_conversation_cb(ChimeConnection *cxn, JsonNode *node)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);
	struct chime_conversation *conv;
	const gchar *channel, *id, *name, *visibility;
	gint64 favourite;
	JsonNode *members_node;

	if (!parse_string(node, "Channel", &channel) ||
	    !parse_string(node, "ConversationId", &id) ||
	    !parse_string(node, "Name", &name) ||
	    !parse_string(node, "Visibility", &visibility) ||
	    !parse_int(node, "Favorite", &favourite) ||
	    !(members_node = json_object_get_member(json_node_get_object(node), "Members")))
		return NULL;

	conv = g_hash_table_lookup(priv->conversations_by_id, id);
	if (conv) {
		g_hash_table_remove(priv->conversations_by_name, conv->name);
		g_free(conv->channel);
		g_free(conv->name);
		g_free(conv->visibility);
	} else {
		conv = g_new0(struct chime_conversation, 1);
		conv->cxn = cxn;
		conv->id = g_strdup(id);
		g_hash_table_insert(priv->conversations_by_id, conv->id, conv);
		chime_jugg_subscribe(cxn, channel, NULL, NULL, NULL);
		chime_jugg_subscribe(cxn, channel, "ConversationMembership",
				     conv_membership_jugg_cb, conv);
		chime_jugg_subscribe(cxn, channel, "TypingIndicator",
				     conv_typing_jugg_cb, conv);

		/* Do we need to fetch new messages? */
		const gchar *last_seen, *last_sent;

		if (!chime_read_last_msg(cxn, FALSE, conv->id, &last_seen, NULL))
			last_seen = "1970-01-01T00:00:00.000Z";

		if (!parse_string(node, "LastSent", &last_sent) ||
		    strcmp(last_seen, last_sent))
			fetch_conversation_messages(conv);
	}

	conv->channel = g_strdup(channel);
	conv->name = g_strdup(name);
	conv->visibility = g_strdup(visibility);
	conv->favourite = favourite;
	if (!conv->members)
		conv->members = g_hash_table_new(g_str_hash, g_str_equal);
	if (!conv->sent_msgs)
		conv->sent_msgs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	JsonArray *arr = json_node_get_array(members_node);
	int i, len = json_array_get_length(arr);
	for (i = 0; i < len; i++) {
		ChimeContact *member = chime_connection_parse_conversation_contact(cxn,
										   json_array_get_element(arr, i), NULL);
		if (member) {
			const gchar *id = chime_contact_get_profile_id(member);
			g_hash_table_insert(conv->members, (gpointer)id, member);
			if (len == 2 && strcmp(id, priv->profile_id)) {
				/* A two-party conversation contains only us (presumably!)
				 * and one other. Index 1:1 conversations on that "other" */
				g_hash_table_insert(priv->im_conversations_by_peer_id,
						    (void *)id, conv);

				printf("im_member for %s (%d) %s\n", conv->id, len, id);
			}
		}
	}

	g_hash_table_insert(priv->conversations_by_name, conv->name, conv);
	return conv;
}

static void fetch_conversations(ChimeConnection *cxn, const gchar *next_token);
static void conversationlist_cb(ChimeConnection *cxn, SoupMessage *msg,
			JsonNode *node, gpointer convlist)
{
	const gchar *next_token;

	if (SOUP_STATUS_IS_SUCCESSFUL(msg->status_code) && node) {
		JsonNode *convs_node;
		JsonObject *obj;
		JsonArray *convs_arr;

		obj = json_node_get_object(node);
		convs_node = json_object_get_member(obj, "Conversations");
		if (convs_node) {
			convs_arr = json_node_get_array(convs_node);
			guint i, len = json_array_get_length(convs_arr);
			for (i = 0; i < len; i++)
				one_conversation_cb(cxn, json_array_get_element(convs_arr, i));
		}
		if (parse_string(node, "NextToken", &next_token))
			fetch_conversations(cxn, next_token);
		else {
			ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);
			if (!priv->convs_online) {
				priv->convs_online = TRUE;
				chime_connection_calculate_online(cxn);
			}
		}
	}
}

static void fetch_conversations(ChimeConnection *cxn, const gchar *next_token)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);
	SoupURI *uri = soup_uri_new_printf(priv->messaging_url, "/conversations");

	soup_uri_set_query_from_fields(uri, "max-results", "50",
				       next_token ? "next-token" : NULL, next_token,
				       NULL);
	chime_connection_queue_http_request(cxn, NULL, uri, "GET", conversationlist_cb, NULL);
}

static void destroy_conversation(gpointer _conv)
{
	struct chime_conversation *conv = _conv;

	chime_jugg_unsubscribe(conv->cxn, conv->channel, NULL, NULL, NULL);
	chime_jugg_unsubscribe(conv->cxn, conv->channel, "ConversationMembership",
			       conv_membership_jugg_cb, conv);
	chime_jugg_unsubscribe(conv->cxn, conv->channel, "TypingIndicator",
			       conv_typing_jugg_cb, conv);
	g_free(conv->id);
	g_free(conv->channel);
	g_free(conv->name);
	g_free(conv->visibility);
	g_hash_table_destroy(conv->members);
	g_hash_table_destroy(conv->sent_msgs);
	conv->members = NULL;
	g_free(conv);
}

static gboolean conv_jugg_cb(ChimeConnection *cxn, gpointer _unused, JsonNode *data_node)
{
	JsonObject *obj = json_node_get_object(data_node);
	JsonNode *record = json_object_get_member(obj, "record");
	if (!record)
		return FALSE;

	return !!one_conversation_cb(cxn, record);
}

struct deferred_conv_jugg {
	JuggernautCallback cb;
	JsonNode *node;
};
static void fetch_new_conv_cb(ChimeConnection *cxn, SoupMessage *msg, JsonNode *node,
			      gpointer _defer)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);
	struct deferred_conv_jugg *defer = _defer;

	if (SOUP_STATUS_IS_SUCCESSFUL(msg->status_code)) {
		JsonObject *obj = json_node_get_object(node);
		node = json_object_get_member(obj, "Conversation");
		if (!node)
			goto bad;

		if (!one_conversation_cb(cxn, node))
			goto bad;

		/* Sanity check; we don't want to just keep looping for ever if it goes wrong */
		const gchar *conv_id;
		if (!parse_string(node, "ConversationId", &conv_id))
			goto bad;

		struct chime_conversation *conv = g_hash_table_lookup(priv->conversations_by_id,
								      conv_id);
		if (!conv)
			goto bad;

		/* OK, now we know about the new conversation we can play the msg node */
		defer->cb(cxn, NULL, defer->node);
		goto out;
	}
 bad:
	;
 out:
	json_node_unref(defer->node);
	g_free(defer);
}

static gboolean conv_msg_jugg_cb(ChimeConnection *cxn, gpointer _unused, JsonNode *data_node)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);
	JsonObject *obj = json_node_get_object(data_node);
	JsonNode *record = json_object_get_member(obj, "record");
	if (!record)
		return FALSE;

	const gchar *conv_id;
	if (!parse_string(record, "ConversationId", &conv_id))
		return FALSE;

	struct chime_conversation *conv = g_hash_table_lookup(priv->conversations_by_id,
							      conv_id);
	if (!conv) {
		/* It seems they don't do the helpful thing and send the notification
		 * of a new conversation before they send the first message. So let's
		 * go looking for it... */
		struct deferred_conv_jugg *defer = g_new0(struct deferred_conv_jugg, 1);
		defer->node = json_node_ref(data_node);
		defer->cb = conv_msg_jugg_cb;

		SoupURI *uri = soup_uri_new_printf(priv->messaging_url, "/conversations/%s", conv_id);
		if (chime_connection_queue_http_request(cxn, NULL, uri, "GET", fetch_new_conv_cb, defer))
			return TRUE;

		json_node_unref(defer->node);
		g_free(defer);
		return FALSE;
	}

	const gchar *id;
	if (!parse_string(record, "MessageId", &id))
		return FALSE;
	if (conv->msgs && conv->msgs->messages) {
		/* Still gathering messages. Add to the table, to avoid dupes */
		g_hash_table_insert(conv->msgs->messages, (gchar *)id, json_node_ref(record));
		return TRUE;
	}
	GTimeVal tv;
	const gchar *created;
	if (!parse_time(record, "CreatedOn", &created, &tv))
		return FALSE;

	chime_update_last_msg(cxn, FALSE, conv->id, created, id);

	return do_conv_deliver_msg(cxn, conv, record, tv.tv_sec);
}

static gboolean conv_membership_jugg_cb(ChimeConnection *cxn, gpointer _unused, JsonNode *data_node)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);
	JsonObject *obj = json_node_get_object(data_node);
	JsonNode *record = json_object_get_member(obj, "record");
	if (!record)
		return FALSE;

	const gchar *conv_id;
	if (!parse_string(record, "ConversationId", &conv_id))
		return FALSE;

	struct chime_conversation *conv = g_hash_table_lookup(priv->conversations_by_id,
							      conv_id);
	if (!conv) {
		/* It seems they don't do the helpful thing and send the notification
		 * of a new conversation before they send the first message. So let's
		 * go looking for it... */
		struct deferred_conv_jugg *defer = g_new0(struct deferred_conv_jugg, 1);
		defer->node = json_node_ref(data_node);
		defer->cb = conv_membership_jugg_cb;

		SoupURI *uri = soup_uri_new_printf(priv->messaging_url, "/conversations/%s", conv_id);
		if (chime_connection_queue_http_request(cxn, NULL, uri, "GET", fetch_new_conv_cb, defer))
			return TRUE;

		json_node_unref(defer->node);
		g_free(defer);
		return FALSE;
	}

#if 0 /* We think we fixed this by reregistering the device as OSX not Android */
	/* WTF? Some users get only ConversationMembership updates when a new message
	   comes in? */
	obj = json_node_get_object(record);
	JsonNode *member = json_object_get_member(obj, "Member");
	if (!member)
		return FALSE;

	/* Has this member seen a message later than the last one we have? */
	const gchar *last_seen, *last_delivered;

	if (!chime_read_last_msg(cxn, FALSE, conv->id, &last_seen, NULL))
		last_seen = "1970-01-01T00:00:00.000Z";

	if (!parse_string(member, "LastDelivered", &last_delivered) ||
	    strcmp(last_seen, last_delivered)) {
		printf("WTF refetching messages for ConversationMembership update\n");
		fetch_conversation_messages(conv);
	} else printf("no fetch last %s\n", last_seen);
#endif
	return TRUE;
}

static gboolean conv_typing_jugg_cb(ChimeConnection *cxn, gpointer _conv, JsonNode *data_node)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);
	struct chime_conversation *conv = _conv;

	gint64 state;
	if (!parse_int(data_node, "state", &state))
		return FALSE;

	JsonNode *node = json_node_get_parent(data_node);
	if (!node)
		return FALSE;

	JsonObject *obj = json_node_get_object(node);
	node = json_object_get_member(obj, "from");

	const gchar *from;
	if (!node || !parse_string(node, "id", &from))
		return FALSE;

	ChimeContact *contact = g_hash_table_lookup(priv->contacts.by_id, from);
	if (!contact)
		return FALSE;

	if (g_hash_table_size(conv->members) != 2) {
		/* Only 1:1 so far */
		return FALSE;
	}

	const gchar *email = chime_contact_get_email(contact);

	if (state)
		serv_got_typing(cxn->prpl_conn, email, 0, PURPLE_TYPING);
	else
		serv_got_typing_stopped(cxn->prpl_conn, email);

	return TRUE;
}

void chime_init_conversations(ChimeConnection *cxn)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	priv->im_conversations_by_peer_id = g_hash_table_new(g_str_hash, g_str_equal);
	priv->conversations_by_name = g_hash_table_new(g_str_hash, g_str_equal);
	priv->conversations_by_id = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, destroy_conversation);
	chime_jugg_subscribe(cxn, priv->device_channel, "ConversationMembership",
			     conv_membership_jugg_cb, NULL);
	chime_jugg_subscribe(cxn, priv->device_channel, "ConversationMessage",
			     conv_msg_jugg_cb, NULL);
	chime_jugg_subscribe(cxn, priv->device_channel, "Conversation",
			     conv_jugg_cb, NULL);
	fetch_conversations(cxn, NULL);
}

void chime_destroy_conversations(ChimeConnection *cxn)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	chime_jugg_unsubscribe(cxn, priv->device_channel, "ConversationMembership",
			       conv_membership_jugg_cb, NULL);
	chime_jugg_unsubscribe(cxn, priv->device_channel, "ConversationMessage",
			       conv_msg_jugg_cb, NULL);
	chime_jugg_unsubscribe(cxn, priv->device_channel, "Conversation",
			       conv_jugg_cb, NULL);
	g_hash_table_destroy(priv->im_conversations_by_peer_id);
	g_hash_table_destroy(priv->conversations_by_name);
	g_hash_table_destroy(priv->conversations_by_id);
	priv->conversations_by_name = priv->conversations_by_id = NULL;
}

struct im_send_data {
	struct chime_conversation *conv;
	gchar *who;
	gchar *message;
	PurpleMessageFlags flags;
};

static void im_error(ChimeConnection *cxn, struct im_send_data *im,
		     const gchar *format, ...)
{
	va_list args;

	va_start(args, format);
	gchar *msg = g_strdup_vprintf(format, args);
	va_end(args);

	PurpleConversation *pconv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_ANY,
									  im->who,
									  cxn->prpl_conn->account);
	if (pconv)
		purple_conversation_write(pconv, NULL, msg, PURPLE_MESSAGE_ERROR, time(NULL));

	g_free(msg);
}

static void send_im_cb(ChimeConnection *cxn, SoupMessage *msg, JsonNode *node, gpointer _im)
{
	struct im_send_data *im = _im;

	/* Nothing to do o nsuccess */
	if (!SOUP_STATUS_IS_SUCCESSFUL(msg->status_code)) {
		im_error(cxn, im, _("Failed to send message(%d): %s\n"),
			 msg->status_code, msg->reason_phrase);
	} else {
		JsonObject *obj = json_node_get_object(node);
		JsonNode *node = json_object_get_member(obj, "Message");
		const gchar *msg_id;
		if (node && parse_string(node, "MessageId", &msg_id)) {
			g_hash_table_add(im->conv->sent_msgs, g_strdup(msg_id));
		}
	}
	g_free(im->message);
	g_free(im);
}

unsigned int chime_send_typing(PurpleConnection *conn, const char *name, PurpleTypingState state)
{
	/* We can't get to PURPLE_TYPED unless we've already sent PURPLE_TYPING... */
	if (state == PURPLE_TYPED)
		return 0;

	ChimeConnection *cxn = purple_connection_get_protocol_data(conn);
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	ChimeContact *contact = g_hash_table_lookup(priv->contacts.by_name, name);
	if (!contact)
		return 0;

	const gchar *id = chime_contact_get_profile_id(contact);
	struct chime_conversation *conv = g_hash_table_lookup(priv->im_conversations_by_peer_id,
							      id);
	if (!conv)
		return 0;

	JsonBuilder *jb = json_builder_new();
	jb = json_builder_begin_object(jb);
	jb = json_builder_set_member_name(jb, "channel");
	jb = json_builder_add_string_value(jb, conv->channel);
	jb = json_builder_set_member_name(jb, "data");
	jb = json_builder_begin_object(jb);
	jb = json_builder_set_member_name(jb, "klass");
	jb = json_builder_add_string_value(jb, "TypingIndicator");
	jb = json_builder_set_member_name(jb, "state");
	jb = json_builder_add_boolean_value(jb, state == PURPLE_TYPING);
	jb = json_builder_end_object(jb);
	jb = json_builder_set_member_name(jb, "except");
	jb = json_builder_begin_array(jb);
	jb = json_builder_add_string_value(jb, priv->ws_key);
	jb = json_builder_end_array(jb);
	jb = json_builder_set_member_name(jb, "type");
	jb = json_builder_add_string_value(jb, "publish");
	jb = json_builder_end_object(jb);

	JsonNode *node = json_builder_get_root(jb);
	chime_connection_jugg_send(cxn, node);

	json_node_unref(node);
	g_object_unref(jb);

	return 0;
}

static int send_im(ChimeConnection *cxn, struct im_send_data *im)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	/* For idempotency of requests. Not that we retry. */
	gchar *uuid = purple_uuid_random();

	JsonBuilder *jb = json_builder_new();
	jb = json_builder_begin_object(jb);
	jb = json_builder_set_member_name(jb, "Content");
	jb = json_builder_add_string_value(jb, im->message);
	jb = json_builder_set_member_name(jb, "ClientRequestToken");
	jb = json_builder_add_string_value(jb, uuid);
	jb = json_builder_end_object(jb);

	int ret;
	SoupURI *uri = soup_uri_new_printf(priv->messaging_url, "/conversations/%s/messages",
					   im->conv->id);
	JsonNode *node = json_builder_get_root(jb);
	if (chime_connection_queue_http_request(cxn, node, uri, "POST", send_im_cb, im))
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

static void conv_create_cb(ChimeConnection *cxn, SoupMessage *msg, JsonNode *node, gpointer _im)
{
	struct im_send_data *im = _im;

	if (SOUP_STATUS_IS_SUCCESSFUL(msg->status_code)) {
		JsonObject *obj = json_node_get_object(node);
		node = json_object_get_member(obj, "Conversation");
		if (!node)
			goto bad;

		im->conv = one_conversation_cb(cxn, node);
		if (!im->conv)
			goto bad;

		send_im(cxn, im);
		return;
	}
 bad:
	im_error(cxn, im, _("Failed to create IM conversation"));
	g_free(im->who);
	g_free(im->message);
	g_free(im);
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

static void autocomplete_im_cb(ChimeConnection *cxn, SoupMessage *msg, JsonNode *node, gpointer _im)
{
	struct im_send_data *im = _im;

	if (SOUP_STATUS_IS_SUCCESSFUL(msg->status_code)) {
		JsonArray *arr = json_node_get_array(node);

		guint i, len = json_array_get_length(arr);
		for (i = 0; i < len; i++) {
			JsonNode *node = json_array_get_element(arr, i);
			const gchar *email, *profile_id;

			if (parse_string(node, "email", &email) &&
			    parse_string(node, "id", &profile_id) &&
			    !strcmp(im->who, email)) {
				create_im_conv(cxn, im, profile_id);
				return;
			}
		}
	}
	im_error(cxn, im, _("Failed to find user"));
	g_free(im->who);
	g_free(im->message);
	g_free(im);
}

int chime_purple_send_im(PurpleConnection *gc, const char *who, const char *message, PurpleMessageFlags flags)
{
	ChimeConnection *cxn = purple_connection_get_protocol_data(gc);
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	struct im_send_data *im = g_new0(struct im_send_data, 1);
	im->message = purple_unescape_html(message);
	im->who = g_strdup(who);
	im->flags = flags;

	ChimeContact *contact = g_hash_table_lookup(priv->contacts.by_name, who);
	if (contact) {
		const gchar *id = chime_contact_get_profile_id(contact);
		im->conv = g_hash_table_lookup(priv->im_conversations_by_peer_id,
					       id);
		if (im->conv)
			return send_im(cxn, im);

		return create_im_conv(cxn, im, id);
	}

	SoupURI *uri = soup_uri_new_printf(priv->contacts_url, "/registered_auto_completes");
	JsonBuilder *jb = json_builder_new();
	jb = json_builder_begin_object(jb);
	jb = json_builder_set_member_name(jb, "q");
	jb = json_builder_add_string_value(jb, who);
	jb = json_builder_end_object(jb);

	int ret;
	JsonNode *node = json_builder_get_root(jb);
	if (chime_connection_queue_http_request(cxn, node, uri, "POST", autocomplete_im_cb, im))
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

