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
#include "chime-connection-private.h"

#include <libsoup/soup.h>
#include "chime-websocket-connection.h"

static void connect_jugg(ChimeConnection *cxn);

struct jugg_subscription {
	JuggernautCallback cb;
	gpointer cb_data;
	gchar *klass;
};

static void free_jugg_subscription(gpointer user_data)
{
	struct jugg_subscription *sub = user_data;

	g_free(sub->klass);
	g_free(sub);
}

static void on_websocket_closed(ChimeWebsocketConnection *ws,
				gpointer _cxn)
{
	ChimeConnection *cxn = _cxn;
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	chime_connection_log(cxn, CHIME_LOGLVL_INFO, "WebSocket closed (%d: '%s')\n",
			     chime_websocket_connection_get_close_code(ws),
			     chime_websocket_connection_get_close_data(ws));

	g_clear_object(&priv->ws_conn);

	if (priv->jugg_connected)
		connect_jugg(cxn);
	else
		chime_connection_fail(cxn, CHIME_ERROR_NETWORK,
				      _("Failed to establish WebSocket connection"));
}

static void handle_callback(ChimeConnection *cxn, const gchar *msg)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);
	JsonParser *parser = json_parser_new();
	gboolean handled = FALSE;
	GError *error = NULL;

	if (!json_parser_load_from_data(parser, msg, strlen(msg), &error)) {
		chime_connection_log(cxn, CHIME_LOGLVL_WARNING, "Error parsing juggernaut message: '%s'\n",
				     error->message);
		g_error_free(error);
		g_object_unref(parser);
		return;
	}

	const gchar *channel = NULL;
	JsonNode *r = json_parser_get_root(parser);
	if (parse_string(r, "channel", &channel)) {
		JsonObject *obj = json_node_get_object(r);
		JsonNode *data_node = json_object_get_member(obj, "data");

		const gchar *klass;
		if (parse_string(data_node, "klass", &klass)) {
			    GList *l = g_hash_table_lookup(priv->subscriptions, channel);
			    while (l) {
				    struct jugg_subscription *sub = l->data;
				    if (sub->cb && (!sub->klass || !strcmp(sub->klass, klass)))
					    handled |= sub->cb(cxn, sub->cb_data, data_node);
				    l = l->next;
			    }
		}
	}
	if (!handled) {
		JsonGenerator *gen = json_generator_new();
		json_generator_set_root(gen, r);
		json_generator_set_pretty(gen, TRUE);

		gchar *data = json_generator_to_data(gen, NULL);
		chime_connection_log(cxn, CHIME_LOGLVL_INFO, "Unhandled jugg msg on channel '%s': %s\n",
				     channel, data);
		g_free(data);
		g_object_unref(gen);
	}
	g_object_unref(parser);
}

static void jugg_send(ChimeConnection *cxn, const gchar *fmt, ...)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);
	va_list args;
	gchar *str;

	va_start(args, fmt);
	str = g_strdup_vprintf(fmt, args);
	va_end(args);

	chime_connection_log(cxn, CHIME_LOGLVL_MISC, "Send juggernaut msg: %s\n", str);
	chime_websocket_connection_send_text(priv->ws_conn, str);
	g_free(str);
}

static void send_subscription_message(ChimeConnection *cxn, const gchar *type, const gchar *channel)
{
	jugg_send(cxn, "3:::{\"type\":\"%s\",\"channel\":\"%s\"}", type, channel);
}

static void send_resubscribe_message(ChimeConnection *cxn);
static void on_websocket_message(ChimeWebsocketConnection *ws, gint type,
				 GBytes *message, gpointer _cxn)
{
	ChimeConnection *cxn = _cxn;
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);
	gchar **parms;
	gconstpointer data;

	if (type != SOUP_WEBSOCKET_DATA_TEXT)
		return;

	data = g_bytes_get_data(message, NULL);

	chime_connection_log(cxn, CHIME_LOGLVL_MISC,
			     "websocket message received:\n'%s'\n", (char *)data);

	/* CONNECT */
	if (!strcmp(data, "1::")) {
		if (!priv->jugg_online) {
			priv->jugg_online = TRUE;
			chime_connection_calculate_online(cxn);
		}
		/* If we go this far, allow reconnect */
		if (!priv->jugg_connected && priv->subscriptions) {
			if (priv->jugg_resubscribe)
				send_resubscribe_message(cxn);
			else {
				guint i, len;
				const gchar **channels = (const gchar **)g_hash_table_get_keys_as_array(priv->subscriptions, &len);
				for (i = 0;  i < len; i++)
					send_subscription_message(cxn, "subscribe", channels[i]);
				g_free(channels);
			}
		}
		priv->jugg_resubscribe = TRUE;
		priv->jugg_connected = TRUE;
		return;
	}
	/* Keepalive */
	if (!strcmp(data, "2::")) {
		jugg_send(cxn, "2::");
		return;
	}
	parms = g_strsplit(data, ":", 4);
	if (parms[0] && parms[1] && *parms[1] && parms[2]) {
		/* Send an ack */
		jugg_send(cxn, "6:::%s", parms[1]);

		if (priv->subscriptions && !strcmp(parms[0], "3") && parms[3])
			handle_callback(cxn, parms[3]);
	}
	g_strfreev(parms);
}

static void on_websocket_pong(ChimeWebsocketConnection *ws,
				 GByteArray *data, gpointer _cxn)
{
	printf("Received Pong frame with %d bytes of payload\n", data->len);

}
static void each_chan(gpointer _chan, gpointer _sub, gpointer _builder)
{
	JsonBuilder **builder = _builder;

	*builder = json_builder_add_string_value(*builder, _chan);
}

static void send_resubscribe_message(ChimeConnection *cxn)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);
	JsonBuilder *builder = json_builder_new();
	builder = json_builder_begin_object(builder);
	builder = json_builder_set_member_name(builder, "type");
	builder = json_builder_add_string_value(builder, "resubscribe");
	builder = json_builder_set_member_name(builder, "channels");
	builder = json_builder_begin_array(builder);
	g_hash_table_foreach(priv->subscriptions, each_chan, &builder);
	builder = json_builder_end_array(builder);
	builder = json_builder_end_object(builder);

	JsonNode *node = json_builder_get_root(builder);
	chime_connection_jugg_send(cxn, node);

	json_node_unref(node);
	g_object_unref(builder);
}

static void connect_jugg_cb(SoupSession *session, SoupMessage *msg, gpointer _cxn)
{
	ChimeConnection *cxn = CHIME_CONNECTION(_cxn);

	g_signal_handlers_disconnect_matched(msg, G_SIGNAL_MATCH_DATA,
                                              0, 0, NULL, NULL, cxn);

	/* The request should never complete; it should always be upgraded */
	chime_connection_fail(cxn, CHIME_ERROR_NETWORK,
			      _("Failed to establish WebSocket connection: %d (%s)"),
			      msg->status_code, msg->reason_phrase);
	g_object_unref(cxn);
}

static void jugg_upgrade_cb(SoupMessage *msg, gpointer _cxn)
{
	ChimeConnection *cxn = CHIME_CONNECTION(_cxn);
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);
	GError *error = NULL;

	if (!soup_websocket_client_verify_handshake(msg, &error)) {
		/* Is there a better way to kill it with fire? Using soup_session_cancel_message()
		   seemed to result in this function being called again, which didn't work very well. */
		g_object_unref(soup_session_steal_connection(priv->soup_sess, msg));
		chime_connection_fail_error(cxn, error);
		g_error_free(error);
		g_object_unref(cxn);
		return;
	}

	g_signal_handlers_disconnect_matched(msg, G_SIGNAL_MATCH_DATA,
					     0, 0, NULL, NULL, cxn);

	g_object_ref(msg);
	GIOStream *stream = soup_session_steal_connection(priv->soup_sess, msg);
	priv->ws_conn = chime_websocket_connection_new(stream, soup_message_get_uri(msg),
						       SOUP_WEBSOCKET_CONNECTION_CLIENT,
						       soup_message_headers_get_one (msg->request_headers, "Origin"),
						       soup_message_headers_get_one (msg->response_headers, "Sec-WebSocket-Protocol"));
	g_object_unref(msg);

	/* Remove limit on the payload size */
	chime_websocket_connection_set_max_incoming_payload_size(priv->ws_conn, 0);
	chime_websocket_connection_set_keepalive_interval(priv->ws_conn, 10);
	jugg_send(cxn, "1::");

	priv->closed_handler = g_signal_connect(G_OBJECT(priv->ws_conn), "closed",
						G_CALLBACK(on_websocket_closed), cxn);
	priv->message_handler = g_signal_connect(G_OBJECT(priv->ws_conn), "message",
						 G_CALLBACK(on_websocket_message), cxn);
	priv->message_handler = g_signal_connect(G_OBJECT(priv->ws_conn), "pong",
						 G_CALLBACK(on_websocket_pong), cxn);
	g_object_unref(cxn);
}

static void connect_jugg(ChimeConnection *cxn)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);
	SoupURI *uri = soup_uri_new_printf(priv->websocket_url, "/1/websocket/%s", priv->ws_key);
	soup_uri_set_query_from_fields(uri, "session_uuid", priv->session_id, NULL);

	SoupMessage *msg = soup_message_new_from_uri("GET", uri);
	soup_uri_free(uri);

	priv->jugg_connected = FALSE;

	soup_websocket_client_prepare_handshake(msg, NULL, NULL);
	g_object_ref(cxn);
	soup_message_add_status_code_handler(msg, "got-informational",
					     SOUP_STATUS_SWITCHING_PROTOCOLS,
					     G_CALLBACK(jugg_upgrade_cb), cxn);
	soup_session_queue_message(priv->soup_sess, msg, connect_jugg_cb, cxn);
}


static void ws_cb(ChimeConnection *cxn, SoupMessage *msg, JsonNode *node, gpointer _unused)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);
	gchar **ws_opts = NULL;

	if (msg->status_code != 200) {
		chime_connection_fail(cxn, CHIME_ERROR_NETWORK,
				      _("Websocket connection error (%d): %s"),
				      msg->status_code, msg->reason_phrase);
		return;
	}
	if (msg->response_body->data)
		ws_opts = g_strsplit(msg->response_body->data, ":", 4);

	if (!ws_opts || !ws_opts[1] || !ws_opts[2] || !ws_opts[3] ||
	    strncmp(ws_opts[3], "websocket,", 10)) {
		chime_connection_fail(cxn, CHIME_ERROR_NETWORK,
				      _("Unexpected response in WebSocket setup: '%s'"),
				      msg->response_body->data);
		return;
	}

	priv->ws_key = g_strdup(ws_opts[0]);
	chime_connection_progress(cxn, 30, _("Establishing WebSocket connection..."));
	g_strfreev(ws_opts);

	connect_jugg(cxn);
}

static gboolean chime_sublist_destroy(gpointer k, gpointer v, gpointer _cxn)
{
	ChimeConnection *cxn = _cxn;
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	if (priv->ws_conn)
		send_subscription_message(_cxn, "unsubscribe", k);

	g_list_free_full(v, free_jugg_subscription);
	return TRUE;
}


static void on_final_ws_close(ChimeWebsocketConnection *ws, gpointer _unused)
{
	g_object_unref(ws);
}

void chime_destroy_juggernaut(ChimeConnection *cxn)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	if (priv->subscriptions) {
		g_hash_table_foreach_remove(priv->subscriptions, chime_sublist_destroy, cxn);
		g_hash_table_destroy(priv->subscriptions);
		priv->subscriptions = NULL;
	}

	/* The ChimeConnection is going away, so disconnect the signals which
	 * refer to it...*/
	if (priv->ws_conn) {
		g_signal_handler_disconnect(G_OBJECT(priv->ws_conn), priv->message_handler);
		g_signal_handler_disconnect(G_OBJECT(priv->ws_conn), priv->closed_handler);
		priv->message_handler = priv->closed_handler = 0;

		jugg_send(cxn, "0::");
		g_signal_connect(G_OBJECT(priv->ws_conn), "closed", G_CALLBACK(on_final_ws_close), NULL);
		priv->ws_conn = NULL;
	}

	g_clear_pointer(&priv->ws_key, g_free);
}

void chime_init_juggernaut(ChimeConnection *cxn)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);
	SoupURI *uri = soup_uri_new_printf(priv->websocket_url, "/1");
	soup_uri_set_query_from_fields(uri, "session_uuid", priv->session_id, NULL);

	chime_connection_progress(cxn, 20, _("Obtaining WebSocket params..."));
	chime_connection_queue_http_request(cxn, NULL, uri, "GET", ws_cb, NULL);
}

gboolean chime_connection_jugg_send(ChimeConnection *cxn, JsonNode *node)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	if (!priv->ws_conn)
		return FALSE;

	JsonGenerator *jg = json_generator_new();
	json_generator_set_root(jg, node);
	gchar *msg = json_generator_to_data(jg, NULL);
	jugg_send(cxn, "3:::%s", msg);
	g_free(msg);
	g_object_unref(jg);

	return TRUE;
}

/*
 * We allow multiple subscribers to a channel, as long as {cb, cb_data, klass}
 * is unique.
 *
 * priv->subscriptions is a GHashTable with 'channel' as key.
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
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);
	struct jugg_subscription *sub = g_new0(struct jugg_subscription, 1);
	GList *l;

	sub->cb = cb;
	sub->cb_data = cb_data;
	if (klass)
		sub->klass = g_strdup(klass);
	if (!priv->subscriptions)
		priv->subscriptions = g_hash_table_new_full(g_str_hash, g_str_equal,
							   g_free, NULL);

	l = g_hash_table_lookup(priv->subscriptions, channel);
	if (!l && priv->ws_conn)
		send_subscription_message(cxn, "subscribe", channel);

	if (g_list_find_custom(l, sub, compare_sub)) {
		free_jugg_subscription(sub);
		return;
	}

	l = g_list_append(l, sub);
	g_hash_table_replace(priv->subscriptions, g_strdup(channel), l);
}

void chime_jugg_unsubscribe(ChimeConnection *cxn, const gchar *channel, const gchar *klass,
			    JuggernautCallback cb, gpointer cb_data)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);
	struct jugg_subscription sub;
	GList *l, *item;

	if (!priv->subscriptions)
		return;

	l = g_hash_table_lookup(priv->subscriptions, channel);
	if (!l)
		return;

	sub.cb = cb;
	sub.cb_data = cb_data;
	sub.klass = (gchar *)klass;

	item = g_list_find_custom(l, &sub, compare_sub);
	if (item) {
		l = g_list_delete_link(l, item);
		if (!l) {
			g_hash_table_remove(priv->subscriptions, channel);
			if (priv->ws_conn)
				send_subscription_message(cxn, "unsubscribe", channel);
		} else
			g_hash_table_replace(priv->subscriptions, g_strdup(channel), l);
	}
}
