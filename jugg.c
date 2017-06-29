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

#include "chime.h"

#include <libsoup/soup.h>

static void connect_jugg(ChimeConnection *cxn);

struct jugg_subscription {
	JuggernautCallback cb;
	gpointer cb_data;
	gchar *klass;
};

static void on_websocket_closed(SoupWebsocketConnection *ws,
				gpointer _cxn)
{
	ChimeConnection *cxn = _cxn;

	printf("websocket closed: %d %s (%d)!\n", soup_websocket_connection_get_close_code(ws),
	       soup_websocket_connection_get_close_data(ws), cxn->jugg_connected);

	g_clear_object(&cxn->ws_conn);

	if (cxn->jugg_connected)
		connect_jugg(cxn);
	else
		purple_connection_error_reason(cxn->prpl_conn, PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
					       _("Failed to establish WebSocket connection"));

}

static void handle_callback(ChimeConnection *cxn, gchar *msg)
{
	JsonParser *parser = json_parser_new();
	gboolean handled = FALSE;
	GError *error = NULL;

	if (!json_parser_load_from_data(parser, msg, strlen(msg), &error)) {
		g_warning("Error loading data: %s", error->message);
		g_error_free(error);
		g_object_unref(parser);
		return;
	}

	const gchar *channel;
	JsonNode *r = json_parser_get_root(parser);
	if (parse_string(r, "channel", &channel)) {
		JsonObject *obj = json_node_get_object(r);
		JsonNode *data_node = json_object_get_member(obj, "data");

		const gchar *klass;
		if (parse_string(data_node, "klass", &klass)) {
			    GList *l = g_hash_table_lookup(cxn->subscriptions, channel);
			    while (l) {
				    struct jugg_subscription *sub = l->data;
				    if (!sub->klass || !strcmp(sub->klass, klass))
					    handled |= sub->cb(cxn, sub->cb_data, data_node);
				    l = l->next;
			    }
		}
	}
	g_object_unref(parser);
}

static void send_resubscribe_message(ChimeConnection *cxn);
static void on_websocket_message(SoupWebsocketConnection *ws, gint type,
				 GBytes *message, gpointer _cxn)
{
	ChimeConnection *cxn = _cxn;
	gchar **parms;
	gconstpointer data;

	if (type != SOUP_WEBSOCKET_DATA_TEXT)
		return;

	data = g_bytes_get_data(message, NULL);

	printf("websocket message received:\n'%s'\n", (char *)data);

	/* Ack */
	if (!strcmp(data, "1::")) {
		/* If we go this far, allow reconnect */
		cxn->jugg_connected = TRUE;
		return;
	}
	/* Keepalive */
	if (!strcmp(data, "2::")) {
		soup_websocket_connection_send_text(cxn->ws_conn,  "2::");
		return;
	}
	parms = g_strsplit(data, ":", 4);
	if (parms[0] && parms[1] && *parms[1] && parms[2]) {
		/* Send an ack */
		gchar *ack = g_strdup_printf("6:::%s", parms[1]);
		soup_websocket_connection_send_text(cxn->ws_conn, ack);
		g_free(ack);

		if (cxn->subscriptions && !strcmp(parms[0], "3") && parms[3])
			handle_callback(cxn, parms[3]);
	}
	g_strfreev(parms);
}

static void each_chan(gpointer _chan, gpointer _sub, gpointer _builder)
{
	JsonBuilder **builder = _builder;

	*builder = json_builder_add_string_value(*builder, _chan);
}

static void send_resubscribe_message(ChimeConnection *cxn)
{
	JsonBuilder *builder = json_builder_new();
	JsonGenerator *gen;
	gchar *msg, *msg2;

	builder = json_builder_begin_object(builder);
	builder = json_builder_set_member_name(builder, "type");
	builder = json_builder_add_string_value(builder, "resubscribe");
	builder = json_builder_set_member_name(builder, "channels");
	builder = json_builder_begin_array(builder);
	g_hash_table_foreach(cxn->subscriptions, each_chan, &builder);
	builder = json_builder_end_array(builder);
	builder = json_builder_end_object(builder);

	gen = json_generator_new();
	json_generator_set_root(gen, json_builder_get_root(builder));
	msg = json_generator_to_data(gen, NULL);

	msg2 = g_strdup_printf("3:::%s", msg);
	soup_websocket_connection_send_text(cxn->ws_conn, msg2);
	printf("resub msg: %s\n", msg2);
	g_free(msg2);
	g_free(msg);
	g_object_unref(gen);
	g_object_unref(builder);
}

static void ws2_cb(GObject *obj, GAsyncResult *res, gpointer _cxn)
{
	ChimeConnection *cxn = _cxn;
	GError *error = NULL;

	cxn->ws_conn = soup_session_websocket_connect_finish(SOUP_SESSION(obj),
							      res, &error);
	if (!cxn->ws_conn) {
		gchar *reason = g_strdup_printf(_("Websocket connection error %s"),
						error->message);
		g_error_free(error);
		purple_connection_error_reason(cxn->prpl_conn, PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
					       reason);
		g_free(reason);
		return;
	}

#if SOUP_CHECK_VERSION(2, 56, 0)
	/* Remove limit on the payload size */
	soup_websocket_connection_set_max_incoming_payload_size(cxn->ws_conn, 0);
#endif

#if SOUP_CHECK_VERSION(2, 58, 0)
	/* NOTE: use 10 seconds for the interval but we might want to make it
	 * a setting so it can be disabled for debugging purposes
	 */
	soup_websocket_connection_set_keepalive_interval(cxn->ws_conn, 10);
#endif

	printf("Got ws conn %p\n", cxn->ws_conn);
	g_signal_connect(G_OBJECT(cxn->ws_conn), "closed",
			 G_CALLBACK(on_websocket_closed), cxn);
	g_signal_connect(G_OBJECT(cxn->ws_conn), "message",
			 G_CALLBACK(on_websocket_message), cxn);

	if (cxn->subscriptions)
		send_resubscribe_message(cxn);

	purple_connection_set_state(cxn->prpl_conn, PURPLE_CONNECTED);
}

static void connect_jugg(ChimeConnection *cxn)
{
	SoupURI *uri = soup_uri_new_printf(cxn->websocket_url, "/1/websocket/%s", cxn->ws_key);
	soup_uri_set_query_from_fields(uri, "session_uuid", cxn->session_id, NULL);

	SoupMessage *msg = soup_message_new_from_uri("GET", uri);
	soup_uri_free(uri);

	printf("no connected\n");
	cxn->jugg_connected = FALSE;
	soup_session_websocket_connect_async(cxn->soup_sess, msg, NULL, NULL, NULL, ws2_cb, cxn);
}


static void ws_cb(ChimeConnection *cxn, SoupMessage *msg, JsonNode *node, gpointer _unused)
{
	gchar **ws_opts = NULL;

	if (msg->status_code != 200) {
		gchar *reason = g_strdup_printf(_("Websocket connection error (%d): %s"),
						msg->status_code, msg->reason_phrase);
		purple_connection_error_reason(cxn->prpl_conn, PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
					       reason);
		g_free(reason);
		return;
	}
	if (msg->response_body->data)
		ws_opts = g_strsplit(msg->response_body->data, ":", 4);

	if (!ws_opts || !ws_opts[1] || !ws_opts[2] || !ws_opts[3] ||
	    strncmp(ws_opts[3], "websocket,", 10)) {
		purple_connection_error_reason(cxn->prpl_conn, PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
					       _("Unexpected response in WebSocket setup"));
		return;
	}

	cxn->ws_key = g_strdup(ws_opts[0]);
	purple_connection_update_progress(cxn->prpl_conn, _("Establishing WebSocket connection..."),
					  3, CONNECT_STEPS);
	g_strfreev(ws_opts);

	connect_jugg(cxn);
}

static gboolean chime_sublist_destroy(gpointer k, gpointer v, gpointer user_data)
{
	g_list_free_full(v, g_free);
	return TRUE;
}

void chime_destroy_juggernaut(ChimeConnection *cxn)
{
	if (cxn->subscriptions) {
		g_hash_table_foreach_remove(cxn->subscriptions, chime_sublist_destroy, NULL);
		g_hash_table_destroy(cxn->subscriptions);
		cxn->subscriptions = NULL;
	}
	g_clear_object(&cxn->ws_conn);
	g_clear_pointer(&cxn->ws_key, g_free);
}

void chime_init_juggernaut(ChimeConnection *cxn)
{
	SoupURI *uri = soup_uri_new_printf(cxn->websocket_url, "/1");
	soup_uri_set_query_from_fields(uri, "session_uuid", cxn->session_id, NULL);

	purple_connection_update_progress(cxn->prpl_conn, _("Obtaining WebSocket params..."),
					  2, CONNECT_STEPS);
	chime_queue_http_request(cxn, NULL, uri, ws_cb, NULL);
}

static void send_subscription_message(ChimeConnection *cxn, const gchar *type, const gchar *channel)
{
	gchar *msg = g_strdup_printf("3:::{\"type\":\"%s\",\"channel\":\"%s\"}", type, channel);
	printf("sub: %s\n", msg);
	soup_websocket_connection_send_text(cxn->ws_conn, msg);
	g_free(msg);
}

/*
 * We allow multiple subscribers to a channel, as long as {cb, cb_data, klass}
 * is unique.
 *
 * cxn->subscriptions is a GHashTable with 'channel' as key.
 *
 * Each value is a GList, containing the set of {cb,cb_data} subscribers.
 *
 * We send the server a subscribe request when the first subscription to a
 * channel occurs, and an unsubscribe request when the last one goes away.
 */
static gboolean compare_sub(gconstpointer _a, gconstpointer _b)
{
	const struct jugg_subscription *a = _a;
	const struct jugg_subscription *b = _b;

	return !(a->cb == b->cb && a->cb_data == b->cb_data && !g_strcmp0(a->klass, b->klass));
}

void chime_jugg_subscribe(ChimeConnection *cxn, const gchar *channel, const gchar *klass,
			  JuggernautCallback cb, gpointer cb_data)
{
	struct jugg_subscription *sub = g_new0(struct jugg_subscription, 1);
	GList *l;

	sub->cb = cb;
	sub->cb_data = cb_data;
	if (klass)
		sub->klass = g_strdup(klass);
	if (!cxn->subscriptions)
		cxn->subscriptions = g_hash_table_new_full(g_str_hash, g_str_equal,
							   g_free, NULL);

	l = g_hash_table_lookup(cxn->subscriptions, channel);
	if (!l && cxn->ws_conn)
		send_subscription_message(cxn, "subscribe", channel);

	if (g_list_find_custom(l, sub, compare_sub)) {
		g_free(sub);
		return;
	}

	l = g_list_append(l, sub);
	g_hash_table_replace(cxn->subscriptions, g_strdup(channel), l);
}

void chime_jugg_unsubscribe(ChimeConnection *cxn, const gchar *channel, const gchar *klass,
			    JuggernautCallback cb, gpointer cb_data)
{
	struct jugg_subscription sub;
	GList *l, *item;

	if (!cxn->subscriptions)
		return;

	l = g_hash_table_lookup(cxn->subscriptions, channel);
	if (!l)
		return;

	sub.cb = cb;
	sub.cb_data = cb_data;
	sub.klass = (gchar *)klass;

	item = g_list_find_custom(l, &sub, compare_sub);
	if (item) {
		l = g_list_remove(l, item->data);
		if (!l) {
			g_hash_table_remove(cxn->subscriptions, channel);
			if (cxn->ws_conn)
				send_subscription_message(cxn, "unsubscribe", channel);
		} else
			g_hash_table_replace(cxn->subscriptions, g_strdup(channel), l);
	}
}

int jugg_dump_incoming(ChimeConnection *cxn, gpointer cb_data, JsonNode *node)
{
	JsonGenerator *gen = json_generator_new();
	gchar *msg;
	json_generator_set_root(gen, node);
	json_generator_set_pretty(gen, TRUE);
	msg = json_generator_to_data(gen, NULL);
	printf("incoming %s : %s\n", (gchar *)cb_data, msg);
	g_free(msg);
	g_object_unref(gen);

	return TRUE;
}

void chime_purple_keepalive(PurpleConnection *conn)
{
	ChimeConnection *cxn = purple_connection_get_protocol_data(conn);

	/* XX: Check for response. But at least this will trigger a disconnect
	   in some cases which we otherwise wouldn't have noticed. */
	if (cxn->ws_conn && cxn->jugg_connected)
		soup_websocket_connection_send_text(cxn->ws_conn, "1::");
}
