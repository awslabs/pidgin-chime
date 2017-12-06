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

#include "protobuf/auth_message.pb-c.h"
#include "protobuf/rt_message.pb-c.h"
#include "protobuf/data_message.pb-c.h"

#include <arpa/inet.h>
#include <string.h>
#include <ctype.h>



static gboolean audio_receive_rt_msg(ChimeCallAudio *audio, gconstpointer pkt, gsize len)
{
	RTMessage *msg = rtmessage__unpack(NULL, len, pkt);
	if (!msg)
		return FALSE;
	gint64 now = g_get_monotonic_time();

	if (msg->audio) {
		if (msg->audio->has_server_time) {
			audio->last_server_time_offset = msg->audio->server_time - now;
			audio->echo_server_time = TRUE;
		}
	}
	gboolean send_sig = FALSE;
	int i;
	for (i=0; i < msg->n_profiles; i++) {
		if (!msg->profiles[i]->has_stream_id)
			continue;

		const gchar *profile_id = g_hash_table_lookup(audio->profiles,
							      GUINT_TO_POINTER(msg->profiles[i]->stream_id));
		if (!profile_id)
			continue;

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

		if (chime_call_participant_audio_stats(audio->call, profile_id, vol, signal_strength))
			send_sig = TRUE;
	}
	if (send_sig)
		chime_call_emit_participants(audio->call);

	rtmessage__free_unpacked(msg, NULL);
	return TRUE;
}

static gboolean do_send_rt_packet(ChimeCallAudio *audio)
{
	audio->audio_msg.seq = (audio->audio_msg.seq + 1) & 0xffff;
	audio->audio_msg.sample_time += 320;

	if (audio->last_server_time_offset) {
		gint64 t = audio->last_server_time_offset + g_get_monotonic_time();
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
	audio->audio_msg.audio.len = 0;

	chime_call_transport_send_packet(audio, XRP_RT_MESSAGE, &audio->rt_msg.base);

	return TRUE;
}
static gboolean audio_receive_auth_msg(ChimeCallAudio *audio, gconstpointer pkt, gsize len)
{
	AuthMessage *msg = auth_message__unpack(NULL, len, pkt);
	if (!msg)
		return FALSE;

	chime_debug("Got AuthMessage authorised %d %d\n", msg->has_authorized, msg->authorized);
	if (msg->has_authorized && msg->authorized) {
		do_send_rt_packet(audio);

		audio->send_rt_source = g_timeout_add(100, (GSourceFunc)do_send_rt_packet, audio);

		g_signal_emit_by_name(audio->call, "call-connected");
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
	DataMessage *msg = data_message__unpack(NULL, len, pkt);
	if (!msg)
		return FALSE;

	chime_debug("Got DataMessage seq %d msg_id %d offset %d\n", msg->seq, msg->msg_id, msg->offset);
	if (!msg->has_seq || !msg->has_msg_id || !msg->has_msg_len)
		return FALSE;

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
		return TRUE;

	struct message_buf *m = find_msgbuf(audio, msg->msg_id, msg->msg_len);
	if (msg->msg_len != m->len)
		return FALSE; /* WTF? */
	if (msg->offset + msg->data.len > m->len)
		return FALSE;

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

			free_msgbuf(m);
			audio->data_messages = g_slist_remove(audio->data_messages, m);
		}
	}
	return TRUE;
}

gboolean audio_receive_packet(ChimeCallAudio *audio, gconstpointer pkt, gsize len)
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

void chime_call_audio_close(ChimeCallAudio *audio, gboolean hangup)
{
	g_signal_handlers_disconnect_matched(G_OBJECT(audio->call), G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, audio);

	if (audio->data_ack_source)
		g_source_remove(audio->data_ack_source);

	if (audio->send_rt_source)
		g_source_remove(audio->send_rt_source);
	chime_debug("close audio\n");
#ifdef AUDIO_HACKS
	close(audio->audio_fd);
	opus_decoder_destroy(audio->opus_dec);
#endif
	g_hash_table_destroy(audio->profiles);
	g_slist_free_full(audio->data_messages, (GDestroyNotify) free_msgbuf);
	chime_call_transport_disconnect(audio, hangup);
	g_signal_emit_by_name(audio->call, "call-disconnected");
	g_free(audio);
}

ChimeCallAudio *chime_call_audio_open(ChimeConnection *cxn, ChimeCall *call, gboolean muted)
{
	ChimeCallAudio *audio = g_new0(ChimeCallAudio, 1);
	audio->call = call;
	audio->profiles = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
#ifdef AUDIO_HACKS
	int opuserr;
	audio->opus_dec = opus_decoder_create(16000, 1, &opuserr);
	gchar *fname = g_strdup_printf("chime-call-%s-audio.s16", chime_call_get_uuid(call));
	audio->audio_fd = open(fname, O_WRONLY|O_TRUNC|O_CREAT, 0644);
	g_free(fname);
#endif

	rtmessage__init(&audio->rt_msg);
	audio_message__init(&audio->audio_msg);
	audio->rt_msg.audio = &audio->audio_msg;
	audio->audio_msg.has_seq = 1;
	audio->audio_msg.seq = g_random_int_range(0, 0x10000);
	audio->audio_msg.has_sample_time = 1;
	audio->audio_msg.sample_time = g_random_int();

	chime_call_transport_connect(audio, muted);

	return audio;
}

