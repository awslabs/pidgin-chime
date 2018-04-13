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
#include "chime-call-screen.h"

#include <string.h>
#include <ctype.h>

#include <gst/rtp/gstrtpbuffer.h>

struct screen_pkt {
	unsigned char type;
	unsigned char flag;
	unsigned char source;
	unsigned char dest;
};

enum screen_pkt_type {
	SCREEN_PKT_TYPE_UNKNOWN = 0,
	SCREEN_PKT_TYPE_CAPTURE = 1,
	SCREEN_PKT_TYPE_KEY_REQUEST = 2,
	SCREEN_PKT_TYPE_PRESENTER_BEGIN = 3,
	SCREEN_PKT_TYPE_PRESENTER_END = 4,
	SCREEN_PKT_TYPE_STREAM_STOP = 5,
	SCREEN_PKT_TYPE_HEARTBEAT_REQUEST = 6,
	SCREEN_PKT_TYPE_HEARTBEAT_RESPONSE = 7,
	SCREEN_PKT_TYPE_VIEWER_BEGIN = 8,
	SCREEN_PKT_TYPE_VIEWER_END = 9,
	SCREEN_PKT_TYPE_RR = 10,
	SCREEN_PKT_TYPE_PING_REQUEST = 11,
	SCREEN_PKT_TYPE_PING_RESPONSE = 12,
	SCREEN_PKT_TYPE_PRESENTER_SWITCH = 16,
	SCREEN_PKT_TYPE_CONTROL = 17,
	SCREEN_PKT_TYPE_PRESENTER_ACK = 18,
	SCREEN_PKT_TYPE_PRESENTER_UPLINK_PROBE = 19,
	SCREEN_PKT_TYPE_EXIT = 20,
};

enum screen_pkt_flag {
	SCREEN_PKT_FLAG_BROADCAST = 1,
	SCREEN_PKT_FLAG_LOCAL = 2,
	SCREEN_PKT_FLAG_SYNTHESISED = 4,
	SCREEN_PKT_FLAG_UNICAST = 8,
};

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

static void screen_send_packet(ChimeCallScreen *screen, enum screen_pkt_type type, void *data, size_t dlen)
{
	unsigned char source = 0;
	unsigned char dest = 0;
	enum screen_pkt_flag flag = SCREEN_PKT_FLAG_LOCAL;

	if (dlen) {
		/* Ick, we need to fix websockets to take iovecs */
		struct screen_pkt *buf = g_malloc0(sizeof(*buf) + dlen);
		buf->type = type;
		buf->source = source;
		buf->dest = dest;
		buf->flag = flag;
		memcpy(&buf[1], data, dlen);

		soup_websocket_connection_send_binary(screen->ws, buf, sizeof(*buf) + dlen);
	} else {
		struct screen_pkt pkt;
		pkt.type = type;
		pkt.source = source;
		pkt.dest = dest;
		pkt.flag = flag;

		soup_websocket_connection_send_binary(screen->ws, &pkt, sizeof(pkt));
	}
}

static void on_screenws_closed(SoupWebsocketConnection *ws, gpointer _screen)
{
	//	ChimeCallScreen *screen = _screen;

	//	chime_call_transport_disconnect(screen, FALSE);
	//	chime_call_transport_connect(screen, screen->silent);
}

static void on_screenws_message(SoupWebsocketConnection *ws, gint type,
			       GBytes *message, gpointer _screen)
{
	ChimeCallScreen *screen = _screen;
	gsize s;
	gconstpointer d = g_bytes_get_data(message, &s);

	if (getenv("CHIME_SCREEN_DEBUG")) {
		printf("incoming:\n");
		hexdump(d, s);
	}

	if (s < 4)
		return;

	const struct screen_pkt *pkt = d;
	switch(pkt->type) {
	case SCREEN_PKT_TYPE_HEARTBEAT_REQUEST:
		screen_send_packet(screen, SCREEN_PKT_TYPE_HEARTBEAT_RESPONSE, NULL, 0);
		break;

	case SCREEN_PKT_TYPE_CAPTURE:
		if (screen->screen_src) {
			GstBuffer *buffer = gst_rtp_buffer_new_allocate(s - 4, 0, 0);
			gst_buffer_fill(buffer, 0, &pkt[1], s - sizeof(*pkt));
			gst_app_src_push_buffer(GST_APP_SRC(screen->screen_src), buffer);
		}
		break;
	}
}


static void screen_ws_connect_cb(GObject *obj, GAsyncResult *res, gpointer _screen)
{
	ChimeCallScreen *screen = _screen;
	ChimeConnection *cxn = CHIME_CONNECTION(obj);
	GError *error = NULL;
	SoupWebsocketConnection *ws = chime_connection_websocket_connect_finish(cxn, res, &error);
	if (!ws) {
		/* If it was cancelled, 'screen' may have been freed. */
		if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
			chime_debug("screen ws error %s\n", error->message);
			screen->state = CHIME_SCREEN_STATE_FAILED;
		}
		g_clear_error(&error);
		g_object_unref(cxn);
		return;
	}
	chime_debug("screen ws connected!\n");
	g_signal_connect(G_OBJECT(ws), "closed", G_CALLBACK(on_screenws_closed), screen);
	g_signal_connect(G_OBJECT(ws), "message", G_CALLBACK(on_screenws_message), screen);
	screen->ws = ws;
	screen->state = CHIME_SCREEN_STATE_CONNECTED;

	g_object_unref(cxn);
}


ChimeCallScreen *chime_call_screen_open(ChimeConnection *cxn, ChimeCall *call)
{
	ChimeCallScreen *screen = g_new0(ChimeCallScreen, 1);

	screen->state = CHIME_SCREEN_STATE_CONNECTING;
	screen->call = call;
	screen->cancel = g_cancellable_new();

	SoupURI *uri = soup_uri_new(chime_call_get_desktop_bithub_url(screen->call));
	SoupMessage *msg = soup_message_new_from_uri("GET", uri);
	soup_message_headers_append(msg->request_headers, "User-Agent", "BibaScreen/2.0");
	soup_message_headers_append(msg->request_headers, "X-BitHub-Call-Id", chime_call_get_uuid(call));
	soup_message_headers_append(msg->request_headers, "X-BitHub-Client-Type", "screen");
	soup_message_headers_append(msg->request_headers, "X-BitHub-Capabilities", "1");
	char *cookie_hdr = g_strdup_printf("_relay_session=%s",
					   chime_connection_get_session_token(cxn));
	soup_message_headers_append(msg->request_headers, "Cookie", cookie_hdr);
	g_free(cookie_hdr);

	char *protocols[] = { (char *)"biba", NULL };
	gchar *origin = g_strdup_printf("http://%s", soup_uri_get_host(uri));
	soup_uri_free(uri);

	chime_connection_websocket_connect_async(g_object_ref(cxn), msg, origin, protocols,
						 screen->cancel, screen_ws_connect_cb, screen);
	g_free(origin);

	return screen;
}

static void on_final_screenws_close(SoupWebsocketConnection *ws, gpointer _unused)
{
	chime_debug("screen ws close\n");
	g_object_unref(ws);
}

static GstAppSrcCallbacks no_appsrc_callbacks;
void chime_call_screen_close(ChimeCallScreen *screen)
{
	if (screen->cancel) {
		g_cancellable_cancel(screen->cancel);
		g_object_unref(screen->cancel);
		screen->cancel = NULL;
	}
	if (screen->ws) {
		g_signal_handlers_disconnect_matched(G_OBJECT(screen->ws), G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, screen);
		g_signal_connect(G_OBJECT(screen->ws), "closed", G_CALLBACK(on_final_screenws_close), NULL);
		soup_websocket_connection_close(screen->ws, 0, NULL);
		screen->ws = NULL;
	}
	if (screen->screen_src)
		gst_app_src_set_callbacks(screen->screen_src, &no_appsrc_callbacks, NULL, NULL);
	g_free(screen);
}

static void screen_appsrc_need_data(GstAppSrc *src, guint length, gpointer _screen)
{
	ChimeCallScreen *screen = _screen;
	screen->appsrc_need_data = TRUE;
}

static void screen_appsrc_enough_data(GstAppSrc *src, gpointer _screen)
{
	ChimeCallScreen *screen = _screen;
	screen->appsrc_need_data = FALSE;
}

static void screen_appsrc_destroy(gpointer _screen)
{
	ChimeCallScreen *screen = _screen;

	screen->screen_src = NULL;
}

static GstAppSrcCallbacks screen_appsrc_callbacks = {
	.need_data = screen_appsrc_need_data,
	.enough_data = screen_appsrc_enough_data,
};


void chime_call_screen_install_app_callbacks(ChimeCallScreen *screen, GstAppSrc *appsrc)
{
	printf("Send viewer start...\n");
	screen_send_packet(screen, SCREEN_PKT_TYPE_VIEWER_BEGIN, NULL, 0);

	screen->screen_src = appsrc;
	gst_app_src_set_callbacks(appsrc, &screen_appsrc_callbacks, screen, screen_appsrc_destroy);
}
