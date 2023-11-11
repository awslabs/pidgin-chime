/*
 * This code is lifted from libsoup's soup-session.c which lacked
 * a copyright header but the websocket parts will be
 *
 * Copyright 2013, 2014 Red Hat, Inc.
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

#include <libsoup/soup.h>

#include <glib/gi18n.h>

#include "chime-connection.h"
#include "chime-connection-private.h"

static void
websocket_connect_async_complete (SoupSession *session, SoupMessage *msg, gpointer user_data)
{
	GTask *task = G_TASK(user_data);

	/* Disconnect websocket_connect_async_stop() handler. */
	g_signal_handlers_disconnect_matched (msg, G_SIGNAL_MATCH_DATA,
					      0, 0, NULL, NULL, task);

	g_task_return_new_error (task,
				 SOUP_WEBSOCKET_ERROR, SOUP_WEBSOCKET_ERROR_NOT_WEBSOCKET,
				 _("WebSocket handshake failed: (%d/%s)"),
				 msg->status_code, msg->reason_phrase);
	g_object_unref (task);
}

static void
websocket_connect_async_stop (SoupMessage *msg, gpointer user_data)
{
	GTask *task = G_TASK(user_data);
	ChimeConnection *cxn = CHIME_CONNECTION(g_task_get_task_data (task));
	ChimeConnectionPrivate *priv = chime_connection_get_private (cxn);
	GError *error = NULL;

	/* Disconnect websocket_connect_async_stop() handler. */
	g_signal_handlers_disconnect_matched (msg, G_SIGNAL_MATCH_DATA,
					      0, 0, NULL, NULL, task);

	g_object_ref(msg);
	if (soup_websocket_client_verify_handshake (msg, &error)) {
		GIOStream *stream = soup_session_steal_connection (priv->soup_sess, msg);
		SoupWebsocketConnection *client = soup_websocket_connection_new (stream,
				 soup_message_get_uri (msg),
				 SOUP_WEBSOCKET_CONNECTION_CLIENT,
				 soup_message_headers_get_one (msg->request_headers, "Origin"),
				 soup_message_headers_get_one (msg->response_headers, "Sec-WebSocket-Protocol"));
		g_object_unref (stream);

		g_task_return_pointer (task, client, g_object_unref);
	} else
		g_task_return_error (task, error);

	g_object_unref (msg);
	g_object_unref (task);
}

/**
 * chime_session_websocket_connect_async:
 * @session: a #SoupSession
 * @msg: #SoupMessage indicating the WebSocket server to connect to
 * @origin: (allow-none): origin of the connection
 * @protocols: (allow-none) (array zero-terminated=1): a
 *   %NULL-terminated array of protocols supported
 * @cancellable: a #GCancellable
 * @callback: the callback to invoke
 * @user_data: data for @callback
 *
 * Asynchronously creates a #SoupWebsocketConnection to communicate
 * with a remote server.
 *
 * All necessary WebSocket-related headers will be added to @msg, and
 * it will then be sent and asynchronously processed normally
 * (including handling of redirection and HTTP authentication).
 *
 * If the server returns "101 Switching Protocols", then @msg's status
 * code and response headers will be updated, and then the WebSocket
 * handshake will be completed. On success,
 * soup_websocket_connect_finish() will return a new
 * #SoupWebsocketConnection. On failure it will return a #GError.
 *
 * If the server returns a status other than "101 Switching
 * Protocols", then @msg will contain the complete response headers
 * and body from the server's response, and
 * soup_websocket_connect_finish() will return
 * %SOUP_WEBSOCKET_ERROR_NOT_WEBSOCKET.
 *
 * Since: 2.50
 */
void
chime_connection_websocket_connect_async (ChimeConnection      *cxn,
					  SoupMessage          *msg,
					  const char           *origin,
					  char                **protocols,
					  GCancellable         *cancellable,
					  GAsyncReadyCallback   callback,
					  gpointer              user_data)
{
	g_return_if_fail (CHIME_IS_CONNECTION (cxn));
	g_return_if_fail (SOUP_IS_MESSAGE (msg));

	ChimeConnectionPrivate *priv = chime_connection_get_private (cxn);

	soup_websocket_client_prepare_handshake (msg, origin, protocols);

	GTask *task = g_task_new (cxn, cancellable, callback, user_data);
	g_task_set_task_data (task, g_object_ref(cxn), g_object_unref);

	soup_message_add_status_code_handler (msg, "got-informational",
					      SOUP_STATUS_SWITCHING_PROTOCOLS,
					      G_CALLBACK (websocket_connect_async_stop), task);
	soup_session_queue_message(priv->soup_sess, msg, websocket_connect_async_complete, task);
}

/**
 * chime_session_websocket_connect_finish:
 * @session: a #SoupSession
 * @result: the #GAsyncResult passed to your callback
 * @error: return location for a #GError, or %NULL
 *
 * Gets the #ChimeWebsocketConnection response to a
 * chime_session_websocket_connect_async() call and (if successful),
 * returns a #ChimeWebsockConnection that can be used to communicate
 * with the server.
 *
 * Return value: (transfer full): a new #ChimeWebsocketConnection, or
 *   %NULL on error.
 *
 * Since: 2.50
 */
SoupWebsocketConnection *
chime_connection_websocket_connect_finish (ChimeConnection  *cxn,
					   GAsyncResult     *result,
					   GError          **error)
{
	g_return_val_if_fail (CHIME_IS_CONNECTION (cxn), NULL);
	g_return_val_if_fail (g_task_is_valid (result, cxn), NULL);

	return g_task_propagate_pointer (G_TASK (result), error);
}

