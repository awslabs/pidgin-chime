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

#include <prpl.h>

#include "chime.h"

#include <libsoup/soup.h>

static void on_websocket_closed(SoupWebsocketConnection *ws,
				gpointer _cxn)
{
	struct chime_connection *cxn = _cxn;

	printf("websocket closed: %d %s!\n", soup_websocket_connection_get_close_code(ws),
	       soup_websocket_connection_get_close_data(ws));
}

static void on_websocket_message(SoupWebsocketConnection *ws, gint type,
				 GBytes *message, gpointer _cxn)
{
	struct chime_connection *cxn = _cxn;
	gchar **parms;
	int i;
	gsize size;
	gconstpointer data;

	if (type != SOUP_WEBSOCKET_DATA_TEXT)
		return;

	data = g_bytes_get_data(message, NULL);

	/* Ack */
	if (!strcmp(data, "1::")) {
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
	}
	g_strfreev(parms);
	printf("websocket message (type %d) received:\n%s", type, (char *)data);
	if (0) for (i = 0; i < size; i++) {
		if (!(i & 0xf))
			printf("\n%04x:", i);
		printf(" %02x", ((unsigned char *)data)[i]);
	}
	printf("\n");
}

static void ws2_cb(GObject *obj, GAsyncResult *res, gpointer _cxn)
{
	struct chime_connection *cxn = _cxn;
	GError *error = NULL;


	cxn->ws_conn = soup_session_websocket_connect_finish(SOUP_SESSION(obj),
							      res, &error);
	if (!cxn->ws_conn) {
		gchar *reason = g_strdup_printf(_("Websocket connection error %s"),
						error->message);
		purple_connection_error_reason(cxn->prpl_conn, PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
					       reason);
		g_free(reason);
		return;
	}
	printf("Got ws conn %p\n", cxn->ws_conn);
	g_signal_connect(G_OBJECT(cxn->ws_conn), "closed",
			 G_CALLBACK(on_websocket_closed), cxn);
	g_signal_connect(G_OBJECT(cxn->ws_conn), "message",
			 G_CALLBACK(on_websocket_message), cxn);

	if (1) soup_websocket_connection_send_text(cxn->ws_conn,
						   "3:::{\"type\":\"subscribe\",\"channel\":\"chat_room!9f50569a-f988-4802-a225-fc08e97d96fd\",\"channels\":null,\"except\":null,\"data\":null}");
	purple_connection_set_state(cxn->prpl_conn, PURPLE_CONNECTED);
}

static void ws_cb(struct chime_connection *cxn, SoupMessage *msg, JsonNode *node, gpointer _unused)
{
	gchar **ws_opts = NULL;
	gchar **protos = NULL;
	gchar *url;
	SoupURI *uri;
	static int foo = 0;

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

	uri = soup_uri_new_printf(cxn->websocket_url, "/1/websocket/%s", ws_opts[0]);
	soup_uri_set_query_from_fields(uri, "session_uuid", cxn->session_id, NULL);

	/* New message */
	msg = soup_message_new_from_uri("GET", uri);
	soup_uri_free(uri);
	purple_connection_update_progress(cxn->prpl_conn, _("Establishing WebSocket connection..."),
					  4, CONNECT_STEPS);
	protos = g_strsplit(ws_opts[3], ",", 0);
	soup_session_websocket_connect_async(cxn->soup_sess, msg, NULL, protos, NULL, ws2_cb, cxn);
	g_strfreev(protos);
	g_strfreev(ws_opts);
}

void chime_init_juggernaut(struct chime_connection *cxn)
{
	SoupURI *uri = soup_uri_new_printf(cxn->websocket_url, "/1");
	soup_uri_set_query_from_fields(uri, "session_uuid", cxn->session_id, NULL);

	purple_connection_update_progress(cxn->prpl_conn, _("Obtaining WebSocket params..."),
					  2, CONNECT_STEPS);
	chime_queue_http_request(cxn, NULL, uri, ws_cb, NULL, TRUE);
}
