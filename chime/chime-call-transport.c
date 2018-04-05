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

#include "chime-connection-private.h"
#include "chime-call.h"
#include "chime-call-audio.h"

#include <arpa/inet.h>
#include <string.h>
#include <ctype.h>

static void hexdump(const void *buf, int len)
{
	char linechars[17];
	int i;

	memset(linechars, 0, sizeof(linechars));
	for (i=0; i < len; i++) {
		unsigned char c = ((unsigned char *)buf)[i];
		if (!(i & 15)) {
			if (i)
				printf("   %s", linechars);
			printf("\n%04x:", i);
		}
		printf(" %02x", c);
		linechars[i & 15] = isprint(c) ? c : '.';
	}
	if (i & 15) {
		linechars[i & 15] = 0;
		printf("   %s", linechars);
	}
	printf("\n");
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

	if (getenv("CHIME_AUDIO_DEBUG")) {
		printf("incoming:\n");
		hexdump(d, s);
	}

	audio_receive_packet(_audio, d, s);
}

static void audio_send_auth_packet(ChimeCallAudio *audio)
{
	ChimeConnection *cxn = chime_call_get_connection(audio->call);
	if (!cxn)
		return;

	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);
	AuthMessage msg = AUTH_MESSAGE__INIT;
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

	msg.codec = 7; /* Opus Med. Later... */
	msg.has_codec = TRUE;

	msg.flags = FLAGS__FLAG_HAS_PROFILE_TABLE | FLAGS__FLAG_HAS_CLIENT_STATUS;
	if (audio->silent)
		msg.flags |= FLAGS__FLAG_MUTE;
	msg.has_flags = TRUE;

	chime_call_transport_send_packet(audio, XRP_AUTH_MESSAGE, &msg.base);
}

static void audio_send_hangup_packet(ChimeCallAudio *audio)
{
	ChimeConnection *cxn = chime_call_get_connection(audio->call);
	if (!cxn)
		return;

	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);
	AuthMessage msg = AUTH_MESSAGE__INIT;
	msg.message_type = AUTH_MESSAGE_TYPE__HANGUP;
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

	msg.codec = 7; /* Opus Med. Later... */
	msg.has_codec = TRUE;

	msg.flags = FLAGS__FLAG_HAS_PROFILE_TABLE;
	if (audio->silent)
		msg.flags |= FLAGS__FLAG_MUTE;
	msg.has_flags = TRUE;

	chime_call_transport_send_packet(audio, XRP_AUTH_MESSAGE, &msg.base);
}

static void audio_ws_connect_cb(GObject *obj, GAsyncResult *res, gpointer _audio)
{
	ChimeCallAudio *audio = _audio;
	ChimeConnection *cxn = CHIME_CONNECTION(obj);
	GError *error = NULL;
	SoupWebsocketConnection *ws = chime_connection_websocket_connect_finish(cxn, res, &error);
	if (!ws) {
		chime_debug("audio ws error %s\n", error->message);
		audio->state = CHIME_AUDIO_STATE_FAILED;
		g_object_unref(cxn);
		return;
	}
	chime_debug("audio ws connected!\n");
	g_signal_connect(G_OBJECT(ws), "closed", G_CALLBACK(on_audiows_closed), audio);
	g_signal_connect(G_OBJECT(ws), "message", G_CALLBACK(on_audiows_message), audio);
	audio->ws = ws;

	audio_send_auth_packet(audio);
	g_object_unref(cxn);
}


void chime_call_transport_connect(ChimeCallAudio *audio, gboolean silent)
{
	/* Grrr, GDtlsClientConnection doesn't actually exist yet. Let's stick
	   with the WebSocket for now... */
	SoupURI *uri = soup_uri_new_printf(chime_call_get_audio_ws_url(audio->call), "/audio");
	SoupMessage *msg = soup_message_new_from_uri("GET", uri);

	audio->silent = silent;

	char *protocols[] = { (char *)"opus-med", NULL };
	gchar *origin = g_strdup_printf("http://%s", soup_uri_get_host(uri));
	soup_uri_free(uri);

	ChimeConnection *cxn = chime_call_get_connection(audio->call);
	chime_connection_websocket_connect_async(g_object_ref(cxn), msg, origin, protocols, NULL,
						 audio_ws_connect_cb, audio);
	g_free(origin);
}

static void on_final_audiows_close(SoupWebsocketConnection *ws, gpointer _unused)
{
	chime_debug("audio ws close\n");
	g_object_unref(ws);
}


void chime_call_transport_disconnect(ChimeCallAudio *audio, gboolean hangup)
{
	if (hangup)
		audio_send_hangup_packet(audio);
	g_mutex_lock(&audio->transport_lock);
	soup_websocket_connection_close(audio->ws, 0, NULL);
	g_signal_handlers_disconnect_matched(G_OBJECT(audio->ws), G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, audio);
	g_signal_connect(G_OBJECT(audio->ws), "closed", G_CALLBACK(on_final_audiows_close), NULL);
	g_mutex_unlock(&audio->transport_lock);
	audio->ws = NULL;
}



void chime_call_transport_send_packet(ChimeCallAudio *audio, enum xrp_pkt_type type, const ProtobufCMessage *message)
{
	if (!audio->ws)
		return;

	size_t len = protobuf_c_message_get_packed_size(message);

	len += sizeof(struct xrp_header);
	struct xrp_header *hdr = g_malloc0(len);
	hdr->type = htons(type);
	hdr->len = htons(len);
	protobuf_c_message_pack(message, (void *)(hdr + 1));
	if (getenv("CHIME_AUDIO_DEBUG")) {
		printf("sending protobuf of len %zd\n", len);
		hexdump(hdr, len);
	}
	g_mutex_lock(&audio->transport_lock);
	soup_websocket_connection_send_binary(audio->ws, hdr, len);
	g_mutex_unlock(&audio->transport_lock);
	g_free(hdr);
}
