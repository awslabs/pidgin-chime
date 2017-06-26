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

#include <libsoup/soup.h>

struct chime_conversation {
	struct chime_chat *chat;

	gchar *channel;
	gchar *id;
	gchar *name;
	gchar *visibility;
	gboolean favourite;

	GHashTable *members;
};

static void one_conversation_cb(JsonArray *array, guint index_,
			JsonNode *node, gpointer _cxn)
{
	struct chime_connection *cxn = _cxn;
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
		return;

	conv = g_hash_table_lookup(cxn->conversations_by_id, id);
	if (conv) {
		g_hash_table_remove(cxn->conversations_by_name, conv->name);
		g_free(conv->channel);
		g_free(conv->name);
		g_free(conv->visibility);
	} else {
		conv = g_new0(struct chime_conversation, 1);
		conv->id = g_strdup(id);
	}

	conv->channel = g_strdup(channel);
	conv->name = g_strdup(name);
	conv->visibility = g_strdup(visibility);
	conv->favourite = favourite;
	conv->members = g_hash_table_new(g_str_hash, g_str_equal);

	const gchar *im_member = NULL;
	JsonArray *arr = json_node_get_array(members_node);
	int i, len = json_array_get_length(arr);
	for (i = 0; i < len; i++) {
		struct chime_contact *member;

		member = chime_contact_new(cxn, json_array_get_element(arr, i), TRUE);
		if (member) {
			g_hash_table_insert(conv->members, member->profile_id, member);
			if (strcmp(member->profile_id, cxn->profile_id)) {
				if (im_member)
					im_member = NULL;
				else
					im_member = member->profile_id;
			}
			printf("im_member %s\n", im_member);
		}
	}

	/* Now im_member is set to the "other" member of any two-party conversations
	 * which contains only us and one other. */
	if (im_member)
		g_hash_table_insert(cxn->im_conversations_by_peer_id, (void *)im_member, conv);

	g_hash_table_insert(cxn->conversations_by_id, conv->id, conv);
	g_hash_table_insert(cxn->conversations_by_name, conv->name, conv);
}

static void fetch_conversations(struct chime_connection *cxn, const gchar *next_token);
static void conversationlist_cb(struct chime_connection *cxn, SoupMessage *msg,
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
			json_array_foreach_element(convs_arr, one_conversation_cb, cxn);
		}
		if (parse_string(node, "NextToken", &next_token))
			fetch_conversations(cxn, next_token);
		else {
			/* Aren't we supposed to do something to indicate we're done? */
		}
	}
}

static void fetch_conversations(struct chime_connection *cxn, const gchar *next_token)
{
	SoupURI *uri = soup_uri_new_printf(cxn->messaging_url, "/conversations");

	soup_uri_set_query_from_fields(uri, "max-results", "50",
				       next_token ? "next-token" : NULL, next_token,
				       NULL);
	chime_queue_http_request(cxn, NULL, uri, conversationlist_cb, NULL);
}

static void destroy_conversation(gpointer _conv)
{
	struct chime_conversation *conv = _conv;

	g_free(conv->id);
	g_free(conv->channel);
	g_free(conv->name);
	g_free(conv->visibility);
	g_hash_table_destroy(conv->members);
	conv->members = NULL;
	g_free(conv);
}

static gboolean conv_msg_cb(gpointer _cxn, const gchar *klass, JsonNode *node)
{
	struct chime_connection *cxn = _cxn;

	JsonObject *obj = json_node_get_object(node);
	JsonNode *record = json_object_get_member(obj, "record");
	if (!record)
		return FALSE;

	const gchar *sender, *message, *created;
	gint64 sys;

	if (!parse_string(record, "Sender", &sender) ||
	    !parse_string(record, "Content", &message) ||
	    !parse_string(record, "CreatedOn", &created) ||
	    !parse_int(record, "IsSystemMessage", &sys))
		return FALSE;

	struct chime_contact *contact = g_hash_table_lookup(cxn->contacts_by_id,
							    sender);
	struct chime_conversation *conv = g_hash_table_lookup(cxn->im_conversations_by_peer_id,
							      sender);
	/* Only 1:1 IM so far; representing multi-party conversations as chats comes later */
	if (!contact || !conv)
		return FALSE;

	GTimeVal tv;
	if (!g_time_val_from_iso8601(created, &tv))
		return FALSE;

	PurpleMessageFlags flags = PURPLE_MESSAGE_RECV;
	if (sys)
		flags |= PURPLE_MESSAGE_SYSTEM;
	serv_got_im(cxn->prpl_conn, contact->email, message, flags, tv.tv_sec);
	return TRUE;
}

void chime_init_conversations(struct chime_connection *cxn)
{
	cxn->im_conversations_by_peer_id = g_hash_table_new(g_str_hash, g_str_equal);
	cxn->conversations_by_name = g_hash_table_new(g_str_hash, g_str_equal);
	cxn->conversations_by_id = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, destroy_conversation);
	chime_jugg_subscribe(cxn, cxn->device_channel, "ConversationMessage", conv_msg_cb, cxn);
	fetch_conversations(cxn, NULL);
}

void chime_destroy_conversations(struct chime_connection *cxn)
{
	g_hash_table_destroy(cxn->im_conversations_by_peer_id);
	g_hash_table_destroy(cxn->conversations_by_name);
	g_hash_table_destroy(cxn->conversations_by_id);
	cxn->conversations_by_name = cxn->conversations_by_id = NULL;
}

struct im_send_data {
	struct chime_contact *contact;
	gchar *message;
	PurpleMessageFlags flags;
};

static void im_error(struct chime_connection *cxn, struct im_send_data *im,
		     const gchar *format, ...)
{
	va_list args;

	va_start(args, format);
	gchar *msg = g_strdup_vprintf(format, args);
	va_end(args);

	PurpleConversation *pconv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_ANY,
									  im->contact->email,
									  cxn->prpl_conn->account);
	if (pconv)
		purple_conversation_write(pconv, NULL, msg, PURPLE_MESSAGE_ERROR, time(NULL));

	g_free(msg);
}

static void send_im_cb(struct chime_connection *cxn, SoupMessage *msg, JsonNode *node, gpointer _im)
{
	struct im_send_data *im = _im;

	/* Nothing to do o nsuccess */
	if (!SOUP_STATUS_IS_SUCCESSFUL(msg->status_code)) {
		im_error(cxn, im, _("Failed to send message(%d): %s\n"),
			 msg->status_code, msg->reason_phrase);
	}
	g_free(im->message);
	g_free(im);
}


static int send_im(struct chime_connection *cxn, struct chime_conversation *conv,
		   struct im_send_data *im)
{
	/* For idempotency of requests. Not that we retry. */
	gchar *uuid = purple_uuid_random();

	JsonBuilder *jb = json_builder_new();
	jb = json_builder_new();
	jb = json_builder_begin_object(jb);
	jb = json_builder_set_member_name(jb, "Content");
	jb = json_builder_add_string_value(jb, im->message);
	jb = json_builder_set_member_name(jb, "ClientRequestToken");
	jb = json_builder_add_string_value(jb, uuid);
	jb = json_builder_end_object(jb);

	int ret;
	SoupURI *uri = soup_uri_new_printf(cxn->messaging_url, "/conversations/%s/messages", conv->id);
	if (chime_queue_http_request(cxn, json_builder_get_root(jb), uri, send_im_cb, im))
		ret = 1;
	else {
		ret = -1;
		g_free(im->message);
		g_free(im);
	}

	g_object_unref(jb);
	return ret;
}

static void conv_create_cb(struct chime_connection *cxn, SoupMessage *msg, JsonNode *node, gpointer _im)
{
	struct im_send_data *im = _im;

	if (SOUP_STATUS_IS_SUCCESSFUL(msg->status_code)) {
		JsonObject *obj = json_node_get_object(node);
		node = json_object_get_member(obj, "Conversation");
		if (!node)
			goto bad;

		one_conversation_cb(NULL, 0, node, cxn);
		struct chime_conversation *conv = g_hash_table_lookup(cxn->im_conversations_by_peer_id,
								      im->contact->profile_id);
		if (!conv)
			goto bad;

		send_im(cxn, conv, im);
		return;
	}
 bad:
	im_error(cxn, im, _("Failed to create IM conversation"));
	g_free(im->message);
	g_free(im);
}

static int create_im_conv(struct chime_connection *cxn, struct im_send_data *im)
{
	JsonBuilder *jb = json_builder_new();
	jb = json_builder_new();
	jb = json_builder_begin_object(jb);
	jb = json_builder_set_member_name(jb, "ProfileIds");
	jb = json_builder_begin_array(jb);
	jb = json_builder_add_string_value(jb, im->contact->profile_id);
	jb = json_builder_end_array(jb);
	jb = json_builder_end_object(jb);

	int ret;
	SoupURI *uri = soup_uri_new_printf(cxn->messaging_url, "/conversations");
	if (chime_queue_http_request(cxn, json_builder_get_root(jb), uri, conv_create_cb, im))
		ret = 1;
	else {
		ret = -1;
		g_free(im->message);
		g_free(im);
	}

	g_object_unref(jb);
	return ret;
}

int chime_purple_send_im(PurpleConnection *gc, const char *who, const char *message, PurpleMessageFlags flags)
{
	struct chime_connection *cxn = purple_connection_get_protocol_data(gc);

	struct chime_contact *contact = g_hash_table_lookup(cxn->contacts_by_email, who);
	if (!contact) {
		/* XXX: Send an invite? */
		return -1;
	}
	struct im_send_data *im = g_new0(struct im_send_data, 1);
	im->contact = contact;
	im->message = g_strdup(message);
	im->flags = flags;

	struct chime_conversation *conv = g_hash_table_lookup(cxn->im_conversations_by_peer_id,
							      contact->profile_id);
	if (conv)
		return send_im(cxn, conv, im);
	else
		return create_im_conv(cxn, im);
}

