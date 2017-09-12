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

#include "protobuf/auth_message.pb-c.h"
#include "protobuf/rt_message.pb-c.h"
#include "protobuf/data_message.pb-c.h"

#include <arpa/inet.h>

struct _ChimeCallAudio {
	ChimeCall *call;
	SoupWebsocketConnection *ws;

	guint data_ack_source;
	guint32 data_next_seq;
	guint64 data_ack_mask;
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

static void audio_send_packet(ChimeCallAudio *audio, enum xrp_pkt_type type, const ProtobufCMessage *message)
{
	size_t len = protobuf_c_message_get_packed_size(message);

	len += sizeof(struct xrp_header);
	struct xrp_header *hdr = g_malloc0(len);
	hdr->type = htons(type);
	hdr->len = htons(len);
	protobuf_c_message_pack(message, (void *)(hdr + 1));
	soup_websocket_connection_send_binary(audio->ws, hdr, len);
	g_free(hdr);
}

static void audio_send_auth_packet(ChimeCallAudio *audio)
{
	ChimeConnection *cxn = chime_call_get_connection(audio->call);
	if (!cxn)
		return;

	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);
	AuthMessage msg;
	auth_message__init(&msg);
	msg.message_type = AUTH_MESSAGE_TYPE__REQUEST;
	msg.has_message_type = TRUE;

	msg.call_id = 0;
	msg.has_call_id = TRUE;

	msg.call_uuid = (char *)chime_call_get_uuid(audio->call);

	msg.service_type = SERVICE_TYPE__FULL_DUPLEX;
	msg.has_service_type = TRUE;

	msg.profile_id = 0;
	msg.has_profile_id = TRUE;

	msg.profile_uuid = (char *)chime_connection_get_profile_id(cxn);

	/* XX: What if it *just* expired? We'll need to renew it and try again? */
	msg.session_token = priv->session_token;

	msg.codec = 6; /* Opus Low. Later... */
	msg.has_codec = TRUE;

	msg.flags = FLAGS__FLAG_MUTE | FLAGS__FLAG_HAS_PROFILE_TABLE;
	msg.has_flags = TRUE;

	audio_send_packet(audio, XRP_AUTH_MESSAGE, &msg.base);
}
static gboolean audio_receive_rt_msg(ChimeCallAudio *audio, gconstpointer pkt, gsize len)
{
	return FALSE;
}
static gboolean audio_receive_auth_msg(ChimeCallAudio *audio, gconstpointer pkt, gsize len)
{
	AuthMessage *msg = auth_message__unpack(NULL, len, pkt);
	if (!msg)
		return FALSE;

	printf("Got AuthMessage authorised %d %d\n", msg->has_authorized, msg->authorized);
	auth_message__free_unpacked(msg, NULL);
	return TRUE;
}

static void do_send_ack(ChimeCallAudio *audio)
{
	DataMessage msg;
	data_message__init(&msg);

	msg.ack = audio->data_next_seq - 1;
	msg.has_ack = TRUE;

	if (audio->data_ack_mask) {
		msg.has_ack_mask = TRUE;
		msg.ack_mask = audio->data_ack_mask;
		audio->data_ack_mask = 0;
	}

	printf("send data ack %d %llx\n", msg.ack, (unsigned long long)msg.ack_mask);
	audio_send_packet(audio, XRP_DATA_MESSAGE, &msg.base);

}
static gboolean idle_send_ack(gpointer _audio)
{
	ChimeCallAudio *audio = _audio;
	do_send_ack(audio);
	audio->data_ack_source = 0;
	return FALSE;
}

static gboolean audio_receive_data_msg(ChimeCallAudio *audio, gconstpointer pkt, gsize len)
{
	DataMessage *msg = data_message__unpack(NULL, len, pkt);
	if (!msg)
		return FALSE;

	printf("Got DataMessage seq %d %d\n", msg->has_seq, msg->seq);
	if (!msg->has_seq)
		return FALSE;

	/* If 'pending' then packat 'data_next_seq' does need to be acked. */
	gboolean pending = !!audio->data_ack_source;

	if (pending || audio->data_ack_mask) {
		while (msg->seq > audio->data_next_seq) {
			if (audio->data_ack_mask & 0x8000000000000000ULL) {
				do_send_ack(audio);
				pending = FALSE;
				break;
			}
			audio->data_next_seq++;
			audio->data_ack_mask <<= 1;

			/* Iff there was already an ack pending, set that bit in the mask */
			if (pending) {
				audio->data_ack_mask |= 1;
				pending = FALSE;
			}
		}
	}
	audio->data_next_seq = msg->seq + 1;
	audio->data_ack_mask <<= 1;
	if (pending)
		audio->data_ack_mask |= 1;
	if (!audio->data_ack_source)
		audio->data_ack_source = g_idle_add(idle_send_ack, audio);
	return TRUE;

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
	SoupWebsocketConnection *ws = chime_connection_websocket_connect_finish(CHIME_CONNECTION(obj), res, &error);
	if (!ws) {
		g_task_return_error(task, error);
		return;
	}
	ChimeCallAudio *audio = g_new0(ChimeCallAudio, 1);
	audio->ws = ws;
	audio->call = g_object_ref(call);

	g_signal_connect(G_OBJECT(ws), "closed", G_CALLBACK(on_audiows_closed), audio);
	g_signal_connect(G_OBJECT(ws), "message", G_CALLBACK(on_audiows_message), audio);

	audio_send_auth_packet(audio);
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

	chime_connection_websocket_connect_async(cxn, msg, NULL, NULL, NULL,
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

