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
#include "chime-call-audio.h"

#include <libsoup/soup.h>

#include "auth_message.pb-c.h"
#include "rt_message.pb-c.h"
#include "data_message.pb-c.h"

#include <opus/opus.h>

#include <gst/rtp/gstrtpbuffer.h>

#include <arpa/inet.h>
#include <string.h>
#include <ctype.h>



static gboolean audio_receive_rt_msg(ChimeCallAudio *audio, gconstpointer pkt, gsize len)
{
	RTMessage *msg = rtmessage__unpack(NULL, len, pkt);
	if (!msg)
		return FALSE;
	gint64 now = g_get_monotonic_time();

	if (msg->client_status) {
		/* This never seems to happen in practice. We just get a Juggernaut message
		 * about the call roster, with a 'muter' node in our own participant information. */
		if (msg->client_status->has_remote_muted && msg->client_status->remote_muted) {
			chime_call_audio_local_mute(audio, TRUE);

			audio->rt_msg.client_status = &audio->client_status_msg;
			audio->client_status_msg.has_remote_mute_ack = TRUE;
			audio->client_status_msg.remote_mute_ack = TRUE;
		} else {
			audio->rt_msg.client_status = NULL;
		}

	}
	if (msg->audio) {
		if (msg->audio->has_server_time) {
			audio->last_server_time_offset = msg->audio->server_time - now;
			audio->echo_server_time = TRUE;
		}
		if (msg->audio->has_audio && audio->audio_src && audio->appsrc_need_data) {
			GstBuffer *buffer = gst_rtp_buffer_new_allocate(msg->audio->audio.len, 0, 0);
			GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
			if (gst_rtp_buffer_map(buffer, GST_MAP_WRITE, &rtp)) {

				chime_debug("Audio RX seq %d ts %u\n", msg->audio->seq, msg->audio->sample_time);

				gst_rtp_buffer_set_ssrc(&rtp, audio->recv_ssrc);
				gst_rtp_buffer_set_payload_type(&rtp, 97);
				gst_rtp_buffer_set_seq(&rtp, msg->audio->seq);
				gst_rtp_buffer_set_timestamp(&rtp, msg->audio->sample_time);
				gst_rtp_buffer_unmap(&rtp);

				gst_buffer_fill(buffer, gst_rtp_buffer_calc_header_len(0),
						msg->audio->audio.data, msg->audio->audio.len);

				gst_app_src_push_buffer(GST_APP_SRC(audio->audio_src), buffer);
			}
		} else if (msg->audio->has_audio && msg->audio->audio.len) {
			chime_debug("Audio drop (%p %d) seq %d ts %u\n",
				    audio->audio_src, audio->appsrc_need_data,
				    msg->audio->seq, msg->audio->sample_time);
		}

	}
	gboolean send_sig = FALSE;
	int i;
	for (i=0; i < msg->n_profiles; i++) {
		if (!msg->profiles[i]->has_stream_id)
			continue;

		const gchar *profile_id = g_hash_table_lookup(audio->profiles,
							      GUINT_TO_POINTER(msg->profiles[i]->stream_id));
		if (!profile_id) {
			chime_debug("no profile for stream id %d\n",
			       msg->profiles[i]->stream_id);
			continue;
		}

		int vol;
		if (msg->profiles[i]->has_muted && msg->profiles[i]->muted)
			vol = -128;
		else if (msg->profiles[i]->has_volume)
			vol = - msg->profiles[i]->volume;
		else /* We should have one or the other */
			continue;

		int signal_strength = -1;
		if (msg->profiles[i]->has_signal_strength)
			signal_strength = msg->profiles[i]->signal_strength;
		chime_debug("Participant %s vol %d\n", profile_id, vol);
		if (chime_call_participant_audio_stats(audio->call, profile_id, vol, signal_strength))
			send_sig = TRUE;
	}
	if (send_sig)
		chime_call_emit_participants(audio->call);

	rtmessage__free_unpacked(msg, NULL);
	return TRUE;
}

static gboolean audio_reconnect(gpointer _audio)
{
	ChimeCallAudio *audio = _audio;

	audio->timeout_source = 0;

	chime_call_transport_disconnect(audio, TRUE);
	chime_call_transport_connect(audio, audio->silent);

	return G_SOURCE_REMOVE;
}

static gboolean timed_send_rt_packet(ChimeCallAudio *audio);
static void do_send_rt_packet(ChimeCallAudio *audio, GstBuffer *buffer)
{
	GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
	guint nr_samples; /* This is added *after* sending the frame */

	g_mutex_lock(&audio->rt_lock);
	gint64 now = g_get_monotonic_time();
	if (!audio->timeout_source && audio->last_rx + 10000000 < now) {
		chime_debug("RX timeout, reconnect audio\n");
		audio->timeout_source = g_timeout_add(0, audio_reconnect, audio);
	}
	if (buffer && GST_BUFFER_DURATION_IS_VALID(buffer) &&
	    GST_BUFFER_DTS_IS_VALID(buffer) && gst_rtp_buffer_map(buffer, GST_MAP_READ, &rtp)) {
		GstClockTime dts, pts, dur;

		dts = GST_BUFFER_DTS(buffer);
		pts = GST_BUFFER_PTS(buffer);
		dur = GST_BUFFER_DURATION(buffer);

		nr_samples = GST_BUFFER_DURATION(buffer) / NS_PER_SAMPLE;
		chime_debug("buf dts %ld pts %ld dur %ld samples %d\n", dts, pts, dur, nr_samples);
		if (audio->next_dts) {
			int frames_missed;

			if (dts < audio->next_dts) {
				chime_debug("Out of order frame %ld < %ld\n", dts, audio->next_dts);
				goto drop;
			}
			frames_missed = (dts - audio->next_dts) / dur;
			if (frames_missed) {
				chime_debug("Missed %d frames\n", frames_missed);
				audio->audio_msg.sample_time += frames_missed * nr_samples;
				audio->next_dts += frames_missed * dur;
			}
			audio->next_dts += dur;
		} else {
			audio->next_dts = dts + dur;
		}

		if (audio->state == CHIME_AUDIO_STATE_AUDIO) {
//			printf ("State %d, send audio\n", audio->state);
			audio->audio_msg.audio.len = gst_rtp_buffer_get_payload_len(&rtp);
			audio->audio_msg.audio.data = gst_rtp_buffer_get_payload(&rtp);
		} else {
//			printf ("State %d, send no audio\n", audio->state);
			audio->audio_msg.audio.len = 0;
		}
	} else {
		int delta_samples = (now - audio->last_send_local_time) / NS_PER_SAMPLE;
		if (delta_samples > 480)
			audio->audio_msg.sample_time += delta_samples - 320;
		audio->next_dts = 0;
		nr_samples = 320;
		audio->audio_msg.audio.len = 0;
	}
	audio->audio_msg.seq = (audio->audio_msg.seq + 1) & 0xffff;

	if (audio->last_server_time_offset) {
		gint64 t = audio->last_server_time_offset + now;
		if (audio->echo_server_time) {
			audio->audio_msg.has_echo_time = 1;
			audio->audio_msg.echo_time = t;
			audio->echo_server_time = FALSE;
		}
		audio->audio_msg.has_server_time = TRUE;
		audio->audio_msg.server_time = t;
	} else
		audio->audio_msg.has_echo_time = 0;

	audio->audio_msg.has_total_frames_lost = TRUE;
	audio->audio_msg.total_frames_lost = 0;

	audio->audio_msg.has_ntp_time = TRUE;
	audio->audio_msg.ntp_time = g_get_real_time();

	audio->audio_msg.has_audio = TRUE;

	audio->last_send_local_time = now;
	chime_call_transport_send_packet(audio, XRP_RT_MESSAGE, &audio->rt_msg.base);
	if (audio->audio_msg.audio.data) {
		audio->audio_msg.audio.data = NULL;
		gst_rtp_buffer_unmap(&rtp);
	}
	audio->audio_msg.sample_time += nr_samples;
 drop:
	g_mutex_unlock(&audio->rt_lock);
}

static gboolean timed_send_rt_packet(ChimeCallAudio *audio)
{
	if (audio->state >= CHIME_AUDIO_STATE_AUDIOLESS)
		do_send_rt_packet(audio, NULL);
	return TRUE;
}

static gboolean audio_receive_auth_msg(ChimeCallAudio *audio, gconstpointer pkt, gsize len)
{
	AuthMessage *msg = auth_message__unpack(NULL, len, pkt);
	if (!msg)
		return FALSE;

	chime_debug("Got AuthMessage authorised %d %d\n", msg->has_authorized, msg->authorized);
	if (msg->has_authorized && msg->authorized) {
		do_send_rt_packet(audio, NULL);
		chime_call_audio_set_state(audio, audio->silent ? CHIME_AUDIO_STATE_AUDIOLESS :
					   (audio->local_mute ? CHIME_AUDIO_STATE_AUDIO_MUTED : CHIME_AUDIO_STATE_AUDIO),
					   NULL);
		if ((audio->silent || audio->local_mute) &&
		    !audio->send_rt_source)
			audio->send_rt_source = g_timeout_add(100, (GSourceFunc)timed_send_rt_packet, audio);
	}

	auth_message__free_unpacked(msg, NULL);
	return TRUE;
}
struct message_frag {
	struct message_frag *next;
	gint32 start;
	gint32 end;
};

struct message_buf {
	gint32 msg_id;
	gint32 len;
	uint8_t *buf;
	struct message_frag *frags;
};

static struct message_buf *find_msgbuf(ChimeCallAudio *audio, gint32 msg_id, gint32 msg_len)
{
	GSList **l = &audio->data_messages;
	struct message_buf *m;

	while (*l) {
		m = (*l)->data;
		if (m->msg_id == msg_id)
			return m;
		else if (m->msg_id > msg_id)
			break;
		else
			l = &((*l)->next);
	}
	m = g_new0(struct message_buf, 1);

	m->msg_id = msg_id;
	m->len = msg_len;
	m->buf = g_malloc0(msg_len);
	/* Insert into the correct place in the sorted list */
	*l = g_slist_prepend(*l, m);
	return m;
}

static void free_msgbuf(struct message_buf *m)
{
	while (m->frags) {
		struct message_frag *f = m->frags;
		m->frags = f->next;
		g_free(f);
	}
	g_free(m->buf);
	g_free(m);
}

void chime_call_audio_cleanup_datamsgs(ChimeCallAudio *audio)
{
	if (audio->data_ack_source) {
		g_source_remove(audio->data_ack_source);
		audio->data_ack_source = 0;
	}

	g_slist_free_full(audio->data_messages, (GDestroyNotify) free_msgbuf);
	audio->data_messages = NULL;

	audio->data_next_seq = 0;
	audio->data_ack_mask = 0;
	audio->data_next_logical_msg = 0;
}

static void do_send_ack(ChimeCallAudio *audio)
{
	DataMessage msg = DATA_MESSAGE__INIT;

	msg.ack = audio->data_next_seq - 1;
	msg.has_ack = TRUE;

	if (audio->data_ack_mask) {
		msg.has_ack_mask = TRUE;
		msg.ack_mask = audio->data_ack_mask;
		audio->data_ack_mask = 0;
	}

	chime_call_transport_send_packet(audio, XRP_DATA_MESSAGE, &msg.base);

}
static gboolean idle_send_ack(gpointer _audio)
{
	ChimeCallAudio *audio = _audio;
	do_send_ack(audio);
	audio->data_ack_source = 0;
	return FALSE;
}

static gboolean insert_frag(struct message_buf *m, gint32 start, gint32 end)
{
	struct message_frag **f = &m->frags, *nf;
	while (*f) {
		if (end < (*f)->start) {
			/* Insert before *f */
			break;
		} else if (start <= (*f)->end) {
			/* Overlap / touching *f so merge */
			if (start < (*f)->start)
				(*f)->start = start;
			/* ... and merge subsequent frags that we now touch */
			if (end > (*f)->end) {
				(*f)->end = end;
				nf = (*f)->next;
				while ((*f)->next && nf->start <= (*f)->end) {
					(*f)->end = nf->end;
					(*f)->next = nf->next;
					g_free(nf);
				}
			}
			goto done;
		} else {
			/* New frag lives after *f */
			f = &(*f)->next;
		}
	}
	nf = g_new0(struct message_frag, 1);
	nf->start = start;
	nf->end = end;
	nf->next = *f;
	*f = nf;
 done:
	return (m->frags->start == 0 &&
		m->frags->end == m->len);
}


static gboolean audio_receive_stream_msg(ChimeCallAudio *audio, gconstpointer pkt, gsize len)
{
	StreamMessage *msg = stream_message__unpack(NULL, len, pkt);
	if (!msg)
		return FALSE;

	ChimeConnection *cxn = chime_call_get_connection(audio->call);
	if (!cxn)
		return FALSE;

	int i;
	for (i = 0; i < msg->n_streams; i++) {
		if (!msg->streams[i]->profile_id || !msg->streams[i]->has_stream_id)
			continue;

		chime_debug("Stream %d: id %x uuid %s\n", i, msg->streams[i]->stream_id, msg->streams[i]->profile_id);
		g_hash_table_insert(audio->profiles, GUINT_TO_POINTER(msg->streams[i]->stream_id),
				    g_strdup(msg->streams[i]->profile_id));
	}
	/* XX: Find the ChimeContacts, put them into a hash table and use them for
	   emitting signals on receipt of ProfileMessages */

	stream_message__free_unpacked(msg, NULL);
	return TRUE;
}
static gboolean audio_receive_data_msg(ChimeCallAudio *audio, gconstpointer pkt, gsize len)
{
	gboolean ret = FALSE;
	DataMessage *msg = data_message__unpack(NULL, len, pkt);
	if (!msg)
		return FALSE;

	chime_debug("Got DataMessage seq %d msg_id %d offset %d\n", msg->seq, msg->msg_id, msg->offset);
	if (!msg->has_seq || !msg->has_msg_id || !msg->has_msg_len)
		goto fail;

	/* First process ACKs */

	/* If 'pending' then packat 'data_next_seq - 1' also needs to be acked. */
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

	/* Now process the incoming data packet. First, drop packets
	   that look like replays and are too old. */
	if (msg->msg_id < audio->data_next_logical_msg)
		goto drop;

	struct message_buf *m = find_msgbuf(audio, msg->msg_id, msg->msg_len);
	if (msg->msg_len != m->len ||
	    msg->offset + msg->data.len > m->len)
		goto fail;

	memcpy(m->buf + msg->offset, msg->data.data, msg->data.len);
	if (insert_frag(m, msg->offset, msg->offset + msg->data.len)) {
		struct xrp_header *hdr = (void *)m->buf;
		if (m->len > sizeof(*hdr) && ntohs(hdr->len) == m->len &&
		    ntohs(hdr->type) == XRP_STREAM_MESSAGE) {
			audio_receive_stream_msg(audio, m->buf + sizeof(*hdr), m->len - sizeof(*hdr));
			audio->data_next_logical_msg = m->msg_id + 1;
		}
		/* Now kill *all* pending messagse up to and including this one */
		while (audio->data_messages) {
			struct message_buf *m = audio->data_messages->data;

			if (m->msg_id >= audio->data_next_logical_msg)
				break;

			audio->data_messages = g_slist_remove(audio->data_messages, m);
			free_msgbuf(m);
		}
	}
 drop:
	ret = TRUE;
 fail:
	data_message__free_unpacked(msg, NULL);
	return ret;
}

gboolean audio_receive_packet(ChimeCallAudio *audio, gconstpointer pkt, gsize len)
{
	if (len < sizeof(struct xrp_header))
		return FALSE;

	const struct xrp_header *hdr = pkt;
	if (len != ntohs(hdr->len))
		return FALSE;

	audio->last_rx = g_get_monotonic_time();

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

static GstAppSinkCallbacks no_appsink_callbacks;
static GstAppSrcCallbacks no_appsrc_callbacks;

void chime_call_audio_close(ChimeCallAudio *audio, gboolean hangup)
{
	g_signal_handlers_disconnect_matched(G_OBJECT(audio->call), G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, audio);

	chime_debug("close audio\n");

	if (audio->audio_src)
		gst_app_src_set_callbacks(audio->audio_src, &no_appsrc_callbacks, NULL, NULL);
	if (audio->audio_sink)
		gst_app_sink_set_callbacks(audio->audio_sink, &no_appsink_callbacks, NULL, NULL);

	chime_call_transport_disconnect(audio, hangup);
	chime_call_audio_set_state(audio, CHIME_AUDIO_STATE_HANGUP, NULL);

	g_hash_table_destroy(audio->profiles);
	g_free(audio);
}

static GstFlowReturn chime_appsink_new_sample(GstAppSink* self, gpointer data)
{
	ChimeCallAudio *audio = (ChimeCallAudio*)data;
	GstSample *sample = gst_app_sink_pull_sample(self);

	if (!sample)
		return GST_FLOW_OK;

	if (audio->state == CHIME_AUDIO_STATE_AUDIO) {
		GstBuffer *buffer = gst_sample_get_buffer(sample);

		do_send_rt_packet(audio, buffer);
	}
	gst_sample_unref(sample);

	return GST_FLOW_OK;
}

static GstAppSinkCallbacks chime_appsink_callbacks = {
	.new_sample = chime_appsink_new_sample,
};

static void chime_appsrc_need_data(GstAppSrc *src, guint length, gpointer _audio)
{
	ChimeCallAudio *audio = _audio;
	audio->appsrc_need_data = TRUE;
}

static void chime_appsrc_enough_data(GstAppSrc *src, gpointer _audio)
{
	ChimeCallAudio *audio = _audio;
	audio->appsrc_need_data = FALSE;
}

static void chime_appsrc_destroy(gpointer _audio)
{
	ChimeCallAudio *audio = _audio;

	audio->audio_src = NULL;
}

static void chime_appsink_destroy(gpointer _audio)
{
	ChimeCallAudio *audio = _audio;

	audio->audio_sink = NULL;
}

static GstAppSrcCallbacks chime_appsrc_callbacks = {
	.need_data = chime_appsrc_need_data,
	.enough_data = chime_appsrc_enough_data,
};

void chime_call_audio_install_gst_app_callbacks(ChimeCallAudio *audio, GstAppSrc *appsrc, GstAppSink *appsink)
{
	audio->audio_sink = appsink;
	audio->audio_src = appsrc;

	audio->appsrc_need_data = TRUE;

	gst_app_src_set_callbacks(appsrc, &chime_appsrc_callbacks, audio, chime_appsrc_destroy);
	gst_app_sink_set_callbacks(appsink, &chime_appsink_callbacks, audio, chime_appsink_destroy);
}

ChimeCallAudio *chime_call_audio_open(ChimeConnection *cxn, ChimeCall *call, gboolean silent)
{
	ChimeCallAudio *audio = g_new0(ChimeCallAudio, 1);

	audio->call = call;
	audio->profiles = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
	g_mutex_init(&audio->transport_lock);
	g_mutex_init(&audio->rt_lock);

	audio->session_id = ((guint64)g_random_int() << 32) | g_random_int();

	rtmessage__init(&audio->rt_msg);
	audio_message__init(&audio->audio_msg);
	client_status_message__init(&audio->client_status_msg);
	audio->rt_msg.audio = &audio->audio_msg;
	audio->audio_msg.has_seq = 1;
	audio->audio_msg.seq = g_random_int_range(0, 0x10000);
	audio->audio_msg.has_sample_time = 1;
	audio->audio_msg.sample_time = g_random_int();

	chime_call_transport_connect(audio, silent);

	return audio;
}

/* Reopen the transport with/without audio enabled at all. */
void chime_call_audio_reopen(ChimeCallAudio *audio, gboolean silent)
{
	chime_call_audio_local_mute(audio, silent);
	if (silent != audio->silent) {
		chime_call_transport_disconnect(audio, TRUE);
		chime_call_transport_connect(audio, silent);
	}
}

gboolean chime_call_audio_get_silent(ChimeCallAudio *audio)
{
	return audio->silent;
}

/* Set client-side muting, when the audio is actually connected */
void chime_call_audio_local_mute(ChimeCallAudio *audio, gboolean muted)
{
	audio->local_mute = muted;

	if (muted) {
		if (audio->state == CHIME_AUDIO_STATE_AUDIO)
			chime_call_audio_set_state(audio, CHIME_AUDIO_STATE_AUDIO_MUTED, NULL);
		if (!audio->send_rt_source)
			audio->send_rt_source = g_timeout_add(100, (GSourceFunc)timed_send_rt_packet, audio);
	} else {
		if (audio->state == CHIME_AUDIO_STATE_AUDIO_MUTED)
			chime_call_audio_set_state(audio, CHIME_AUDIO_STATE_AUDIO, NULL);
		if (audio->send_rt_source) {
			g_source_remove(audio->send_rt_source);
			audio->send_rt_source = 0;
		}
	}
}
