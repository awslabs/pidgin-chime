/*
 * Pidgin/libpurple Chime client plugin
 *
 * Copyright © 2017 Amazon.com, Inc. or its affiliates.
 *
 * Authors: David Woodhouse <dwmw2@infradead.org>
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

#include "chime-connection.h"
#include "chime-call.h"
#include "chime-connection-private.h"

#include <libsoup/soup.h>

#include <arpa/inet.h>

struct _ChimeCallAudio {
	ChimeCall *call;
	SoupWebsocketConnection *ws;
};

struct xrp_header {
	guint16 type;
	guint16 len;
};

enum xrp_pkt_type {
	XRP_RT_MESSAGE = 2,
	XRP_AUTH_MESSAGE= 3,
	XRP_DATA_MESSAGE = 4,
};

static gboolean audio_receive_rt_msg(ChimeCallAudio *audio, gconstpointer pkt, gsize len)
{
	return FALSE;
}
static gboolean audio_receive_auth_msg(ChimeCallAudio *audio, gconstpointer pkt, gsize len)
{
	return FALSE;
}
static gboolean audio_receive_data_msg(ChimeCallAudio *audio, gconstpointer pkt, gsize len)
{
	return FALSE;
}
static gboolean audio_receive_packet(ChimeCallAudio *audio, gconstpointer pkt, gsize len)
{
	if (len < sizeof(struct xrp_header))
		return FALSE;

	const struct xrp_header *hdr = pkt;
	if (len != ntohs(hdr->len))
		return FALSE;

	/* Point to the payload, without (void *) arithmetic */
	pkt = hdr + 1;
	len -= 4;

	switch (ntohs(hdr->type)) {
	case XRP_RT_MESSAGE:
		return audio_receive_rt_msg(audio, pkt, len);
	case XRP_AUTH_MESSAGE:
		return audio_receive_auth_msg(audio, pkt, len);
	case XRP_DATA_MESSAGE:
		return audio_receive_data_msg(audio, pkt, len);
	}
	return FALSE;
}
static void on_audiows_closed(SoupWebsocketConnection *ws, gpointer _audio)
{
	/* XXX: Reconnect it */

}

static void on_audiows_message(SoupWebsocketConnection *ws, gint type,
			       GBytes *message, gpointer _audio)
{
	gsize s;
	gconstpointer d = g_bytes_get_data(message, &s);

	audio_receive_packet(_audio, d, s);
}

static void free_audio(gpointer _audio)
{
	ChimeCallAudio *audio = _audio;

	g_signal_handlers_disconnect_matched(G_OBJECT(audio->call), G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, audio);

	soup_websocket_connection_close(audio->ws, 0, NULL);
	g_object_unref(audio->ws);
	g_object_unref(audio->call);
	g_free(audio);
}

static void audio_ws_connect_cb(GObject *obj, GAsyncResult *res, gpointer _task)
{
	GTask *task = G_TASK(_task);
	ChimeCall *call = CHIME_CALL(g_task_get_task_data(task));

	GError *error = NULL;
	SoupWebsocketConnection *ws = soup_session_websocket_connect_finish(SOUP_SESSION(obj), res, &error);
	if (!ws) {
		g_task_return_error(task, error);
		return;
	}
	ChimeCallAudio *audio = g_new0(ChimeCallAudio, 1);
	audio->ws = ws;
	audio->call = g_object_ref(call);

	g_signal_connect(G_OBJECT(ws), "closed", G_CALLBACK(on_audiows_closed), audio);
	g_signal_connect(G_OBJECT(ws), "message", G_CALLBACK(on_audiows_message), audio);

	g_task_return_pointer(task, audio, free_audio);
	g_object_unref(task);
}

void chime_connection_join_call_audio_async(ChimeConnection *cxn,
					    ChimeCall *call,
					    GCancellable *cancellable,
					    GAsyncReadyCallback callback,
					    gpointer user_data)
{
	g_return_if_fail(CHIME_IS_CONNECTION(cxn));
	g_return_if_fail(CHIME_IS_CALL(call));

	GTask *task = g_task_new(cxn, cancellable, callback, user_data);
	g_task_set_task_data(task, g_object_ref(call), g_object_unref);

	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	/* Grrr, GDtlsClientConnection doesn't actually exist yet. Let's stick
	   with the WebSocket for now... */
	SoupURI *uri = soup_uri_new(chime_call_get_audio_ws_url(call));
	SoupMessage *msg = soup_message_new_from_uri("GET", uri);
	soup_uri_free(uri);

	soup_session_websocket_connect_async(priv->soup_sess, msg, NULL, NULL, NULL,
					     audio_ws_connect_cb, task);
}

ChimeCallAudio *chime_connection_join_call_audio_finish(ChimeConnection *self,
							GAsyncResult *result,
							GError **error)
{
	g_return_val_if_fail(CHIME_IS_CONNECTION(self), FALSE);
	g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

	return g_task_propagate_pointer(G_TASK(result), error);
}

