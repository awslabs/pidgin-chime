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

#include <libsoup/soup.h>

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

void chime_complete_messages(ChimeConnection *cxn, struct chime_msgs *msgs)
{
	GList *l = NULL;
	printf("List at %p\n", &l);
	/* Sort messages by time */
	g_hash_table_foreach_remove(msgs->messages, insert_queued_msg, &l);
	g_hash_table_destroy(msgs->messages);
	g_clear_pointer(&msgs->last_msg, g_free);
	g_clear_pointer(&msgs->last_msg_time, g_free);
	msgs->messages = NULL;

	while (l) {
		struct msg_sort *ms = l->data;
		JsonNode *node = ms->node;
		msgs->cb(cxn, msgs, node, ms->tm.tv_sec);
		g_free(ms);
		l = g_list_remove(l, ms);

		/* Last message, note down the received time */
		if (!l) {
			const gchar *tm, *id;
			if (parse_string(node, "CreatedOn", &tm) &&
			    parse_string(node, "MessageId", &id))
				chime_update_last_msg(cxn, msgs->is_room, msgs->id, tm, id);
		}
		json_node_unref(node);
	}
}

static void one_msg_cb(JsonArray *array, guint index_,
		       JsonNode *node, gpointer _msgs)
{
	struct chime_msgs *msgs = _msgs;
	const char *id;

	if (!parse_string(node, "MessageId", &id))
		return;

	/* Drop if it it's the last of the messages we'd already seen */
	if (msgs->last_msg && !strcmp(id, msgs->last_msg)) {
		g_clear_pointer(&msgs->last_msg, g_free);
		return;
	}

	g_hash_table_insert(msgs->messages, (gpointer)id, json_node_ref(node));
}

static void fetch_msgs_cb(ChimeConnection *cxn, SoupMessage *msg, JsonNode *node, gpointer _msgs)
{
	struct chime_msgs *msgs = _msgs;
	const gchar *next_token;

	JsonObject *obj = json_node_get_object(node);
	JsonNode *msgs_node = json_object_get_member(obj, "Messages");
	JsonArray *msgs_array = json_node_get_array(msgs_node);

	msgs->soup_msg = NULL;
	json_array_foreach_element(msgs_array, one_msg_cb, msgs);

	if (parse_string(node, "NextToken", &next_token))
		fetch_messages(cxn, _msgs, next_token);
	else {
		msgs->msgs_done = TRUE;
		if (msgs->members_done)
			chime_complete_messages(cxn, msgs);
	}
}

void fetch_messages(ChimeConnection *cxn, struct chime_msgs *msgs, const gchar *next_token)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);
	SoupURI *uri = soup_uri_new_printf(priv->messaging_url, "/%ss/%s/messages",
					   msgs->is_room ? "room" : "conversation", msgs->id);
	const gchar *opts[4];
	int i = 0;

	if (!msgs->messages)
		msgs->messages = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, (GDestroyNotify)json_node_unref);

	if (msgs->last_msg_time && msgs->last_msg_time[0]) {
		opts[i++] = "after";
		opts[i++] = msgs->last_msg_time;
	}
	if (next_token) {
		opts[i++] = "next-token";
		opts[i++] = next_token;
	}
	while (i < 4)
		opts[i++] = NULL;

	soup_uri_set_query_from_fields(uri, "max-results", "50", opts[0], opts[1], opts[2], opts[3], NULL);
	msgs->soup_msg = chime_connection_queue_http_request(cxn, NULL, uri, "GET", fetch_msgs_cb, msgs);
}



