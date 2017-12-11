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

#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>

struct _ChimeCallAudio {
	ChimeCall *call;
	ChimeAudioState state;
	gboolean local_mute;
	gboolean muted;
	GMutex transport_lock;
	SoupWebsocketConnection *ws;
	guint data_ack_source;
	guint32 data_next_seq;
	guint64 data_ack_mask;
	gint32 data_next_logical_msg;
	GSList *data_messages;
	GHashTable *profiles;

	GstClockTime next_dts;
	gint64 last_send_local_time;
	GstAppSrc *audio_src;
	gboolean appsrc_need_data;

	GMutex rt_lock;
	guint send_rt_source;
	gint64 last_server_time_offset;
	gboolean echo_server_time;
	RTMessage rt_msg;
	AudioMessage audio_msg;
	ClientStatusMessage client_status_msg;
};

struct xrp_header {
	guint16 type;
	guint16 len;
};

enum xrp_pkt_type {
	XRP_RT_MESSAGE = 2,
	XRP_AUTH_MESSAGE= 3,
	XRP_DATA_MESSAGE = 4,
	XRP_STREAM_MESSAGE = 5,
};

/* Called from ChimeMeeting */
ChimeCallAudio *chime_call_audio_open(ChimeConnection *cxn, ChimeCall *call, gboolean muted);
void chime_call_audio_close(ChimeCallAudio *audio, gboolean hangup);
void chime_call_audio_reopen(ChimeCallAudio *audio, gboolean muted);
void chime_call_audio_set_state(ChimeCallAudio *audio, ChimeAudioState state);
void chime_call_audio_local_mute(ChimeCallAudio *audio, gboolean muted);

/* Called from audio code */
void chime_call_transport_connect(ChimeCallAudio *audio, gboolean muted);
void chime_call_transport_disconnect(ChimeCallAudio *audio, gboolean hangup);
void chime_call_transport_send_packet(ChimeCallAudio *audio, enum xrp_pkt_type type, const ProtobufCMessage *message);

/* Callbacks into audio code from transport */
gboolean audio_receive_packet(ChimeCallAudio *audio, gconstpointer pkt, gsize len);

void chime_call_audio_install_gst_app_callbacks(ChimeCallAudio *audio, GstAppSrc *appsrc, GstAppSink *appsink);
