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
	gchar *favourite;

	GHashTable *members;
};

static void one_conversation_cb(JsonArray *array, guint index_,
			JsonNode *node, gpointer _cxn)
{
	struct chime_connection *cxn = _cxn;
	struct chime_conversation *conv;
	const gchar *channel, *id, *name, *visibility, *favourite;
	JsonNode *members_node;

	if (!parse_string(node, "Channel", &channel) ||
	    !parse_string(node, "ConversationId", &id) ||
	    !parse_string(node, "Name", &name) ||
	    !parse_string(node, "Visibility", &visibility) ||
	    !parse_string(node, "Favorite", &favourite) ||
	    !(members_node = json_object_get_member(json_node_get_object(node), "Members")))
		return;

	conv = g_hash_table_lookup(cxn->conversations_by_id, id);
	if (conv) {
		g_hash_table_remove(cxn->conversations_by_name, conv->name);
		g_free(conv->channel);
		g_free(conv->name);
		g_free(conv->visibility);
		g_free(conv->favourite);
	} else {
		conv = g_new0(struct chime_conversation, 1);
		conv->id = g_strdup(id);
	}

	conv->channel = g_strdup(channel);
	conv->name = g_strdup(name);
	conv->visibility = g_strdup(visibility);
	conv->favourite = g_strdup(favourite);
	conv->members = g_hash_table_new(g_str_hash, g_str_equal);

	JsonArray *arr = json_node_get_array(members_node);
	int i, len = json_array_get_length(arr);
	for (i = 0; i < len; i++) {
		struct chime_contact *member;

		member = chime_contact_new(cxn, json_array_get_element(arr, i), TRUE);
		if (member)
			g_hash_table_insert(conv->members, member->profile_id, member);
	}

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
	g_free(conv->favourite);
	g_hash_table_destroy(conv->members);
	conv->members = NULL;
	g_free(conv);
}

void chime_init_conversations(struct chime_connection *cxn)
{
	cxn->conversations_by_name = g_hash_table_new(g_str_hash, g_str_equal);
	cxn->conversations_by_id = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, destroy_conversation);
	fetch_conversations(cxn, NULL);
}

void chime_destroy_conversations(struct chime_connection *cxn)
{
	g_hash_table_destroy(cxn->conversations_by_name);
	g_hash_table_destroy(cxn->conversations_by_id);
	cxn->conversations_by_name = cxn->conversations_by_id = NULL;
}
