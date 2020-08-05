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

#include <config.h>

#include "chime-connection-private.h"
#include "chime-call.h"
#include "chime-call-audio.h"

#include <arpa/inet.h>
#include <string.h>
#include <ctype.h>

#include <gnutls/dtls.h>

#define CHIME_DTLS_MTU 1196

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
	ChimeCallAudio *audio = _audio;

	chime_call_transport_disconnect(audio, FALSE);
	chime_call_transport_connect(audio, audio->silent);
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

	msg.session_id = audio->session_id;
	msg.has_session_id = TRUE;

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
		/* If it was cancelled, 'audio' may have been freed. */
		if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
			chime_debug("audio ws error %s\n", error->message);
			audio->state = CHIME_AUDIO_STATE_FAILED;
		}
		g_clear_error(&error);
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


static void chime_call_transport_connect_ws(ChimeCallAudio *audio)
{
	SoupURI *uri = soup_uri_new_printf(chime_call_get_audio_ws_url(audio->call), "/audio");
	SoupMessage *msg = soup_message_new_from_uri("GET", uri);

	char *protocols[] = { (char *)"opus-med", NULL };
	gchar *origin = g_strdup_printf("http://%s", soup_uri_get_host(uri));
	soup_uri_free(uri);

	ChimeConnection *cxn = chime_call_get_connection(audio->call);
	chime_connection_websocket_connect_async(g_object_ref(cxn), msg, origin, protocols,
						 audio->cancel, audio_ws_connect_cb, audio);
	g_free(origin);
}

static void set_gnutls_error (ChimeCallAudio *audio, GError *error)
{
	if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		gnutls_transport_set_errno (audio->dtls_sess, EINTR);
	else if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK))
		gnutls_transport_set_errno (audio->dtls_sess, EAGAIN);
	else if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT))
		gnutls_transport_set_errno (audio->dtls_sess, EINTR);
	else if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE))
		gnutls_transport_set_errno (audio->dtls_sess, EMSGSIZE);
	else
		gnutls_transport_set_errno (audio->dtls_sess, EIO);

	g_error_free(error);
}

static ssize_t
g_tls_connection_gnutls_pull_func (gnutls_transport_ptr_t  transport_data,
                                   void                   *buf,
                                   size_t                  buflen)
{
	ChimeCallAudio *audio = transport_data;
	GError *error = NULL;
	ssize_t ret;

	GInputVector vector = { buf, buflen };
	GInputMessage message = { NULL, &vector, 1, 0, 0, NULL, NULL };

	ret = g_datagram_based_receive_messages(G_DATAGRAM_BASED(audio->dtls_sock),
						&message, 1, 0, 0, NULL, &error);
	if (ret > 0)
		ret = message.bytes_received;
	else if (ret < 0)
		set_gnutls_error (audio, error);

	return ret;
}

static ssize_t
g_tls_connection_gnutls_push_func (gnutls_transport_ptr_t  transport_data,
                                   const void             *buf,
                                   size_t                  buflen)
{
	ChimeCallAudio *audio = transport_data;
	GError *error = NULL;
	ssize_t ret;

	GOutputVector vector = { buf, buflen };
	GOutputMessage message = { NULL, &vector, 1, 0, NULL, 0 };

	ret = g_datagram_based_send_messages(G_DATAGRAM_BASED(audio->dtls_sock),
					     &message, 1, 0, 0, NULL, &error);

	if (ret > 0)
		ret = message.bytes_sent;
	else if (ret < 0)
		set_gnutls_error(audio, error);

	return ret;
}

static ssize_t
g_tls_connection_gnutls_vec_push_func (gnutls_transport_ptr_t  transport_data,
                                       const giovec_t         *iov,
                                       int                     iovcnt)
{
	ChimeCallAudio *audio = transport_data;
	GError *error = NULL;
	ssize_t ret;
	GOutputMessage message = { NULL, };
	GOutputVector *vectors;

	/* this entire expression will be evaluated at compile time */
	if (sizeof *iov == sizeof *vectors &&
	    sizeof iov->iov_base == sizeof vectors->buffer &&
	    G_STRUCT_OFFSET (giovec_t, iov_base) ==
	    G_STRUCT_OFFSET (GOutputVector, buffer) &&
	    sizeof iov->iov_len == sizeof vectors->size &&
	    G_STRUCT_OFFSET (giovec_t, iov_len) ==
	    G_STRUCT_OFFSET (GOutputVector, size)) {
		/* ABI is compatible */
		message.vectors = (GOutputVector *)iov;
		message.num_vectors = iovcnt;
	} else {
		/* ABI is incompatible */
		gint i;

		message.vectors = g_newa (GOutputVector, iovcnt);
		for (i = 0; i < iovcnt; i++) {
			message.vectors[i].buffer = (void *)iov[i].iov_base;
			message.vectors[i].size = iov[i].iov_len;
		}
		message.num_vectors = iovcnt;
	}

	ret = g_datagram_based_send_messages(G_DATAGRAM_BASED(audio->dtls_sock),
					     &message, 1, 0, 0, 0, &error);

	if (ret > 0)
		ret = message.bytes_sent;
	else if (ret < 0)
		set_gnutls_error(audio, error);

	return ret;
}

static gboolean
read_datagram_based_cb (GDatagramBased *datagram_based,
                        GIOCondition    condition,
                        gpointer        user_data)
{
	gboolean *read_done = user_data;

	*read_done = TRUE;

	return G_SOURCE_CONTINUE;
}

static gboolean
read_timeout_cb (gpointer user_data)
{
	gboolean *timed_out = user_data;

	*timed_out = TRUE;

	return G_SOURCE_REMOVE;
}

static int
g_tls_connection_gnutls_pull_timeout_func (gnutls_transport_ptr_t transport_data,
                                           unsigned int           ms)
{
	ChimeCallAudio *audio = transport_data;

	/* Fast path. */
	if (g_datagram_based_condition_check(G_DATAGRAM_BASED(audio->dtls_sock), G_IO_IN) ||
	    g_cancellable_is_cancelled (audio->cancel))
		return 1;

	/* If @ms is 0, GnuTLS wants an instant response, so there’s no need to
	 * construct and query a #GSource. */
	if (ms > 0) {
		GMainContext *ctx = NULL;
		GSource *read_source = NULL, *timeout_source = NULL;
		gboolean read_done = FALSE, timed_out = FALSE;

		ctx = g_main_context_new ();

		/* Create a timeout source. */
		timeout_source = g_timeout_source_new (ms);
		g_source_set_callback (timeout_source, (GSourceFunc) read_timeout_cb,
				       &timed_out, NULL);

		/* Create a read source. We cannot use g_source_set_ready_time() on this
		 * to combine it with the @timeout_source, as that could mess with the
		 * internals of the #GDatagramBased’s #GSource implementation. */
		read_source = g_datagram_based_create_source (G_DATAGRAM_BASED(audio->dtls_sock),
							      G_IO_IN, NULL);
		g_source_set_callback (read_source, (GSourceFunc) read_datagram_based_cb,
				       &read_done, NULL);

		g_source_attach (read_source, ctx);
		g_source_attach (timeout_source, ctx);

		while (!read_done && !timed_out)
			g_main_context_iteration (ctx, TRUE);

		g_source_destroy (read_source);
		g_source_destroy (timeout_source);

		g_main_context_unref (ctx);
		g_source_unref (read_source);
		g_source_unref (timeout_source);

		/* If @read_source was dispatched due to cancellation, the resulting error
		 * will be handled in g_tls_connection_gnutls_pull_func(). */
		if (g_datagram_based_condition_check(G_DATAGRAM_BASED(audio->dtls_sock), G_IO_IN) ||
		    g_cancellable_is_cancelled (audio->cancel))
			return 1;
	}

	return 0;
}

static gboolean dtls_timeout(ChimeCallAudio *audio);

static gboolean dtls_src_cb(GDatagramBased *dgram, GIOCondition condition, ChimeCallAudio *audio)
{
	if (!audio->dtls_handshaked) {
		int ret = gnutls_handshake(audio->dtls_sess);

		if (ret == GNUTLS_E_AGAIN) {
			if (audio->timeout_source)
				g_source_remove(audio->timeout_source);

			int timeo = gnutls_dtls_get_timeout(audio->dtls_sess);
			audio->timeout_source = g_timeout_add(timeo, (GSourceFunc)dtls_timeout, audio);

			return G_SOURCE_CONTINUE;
		}

		if (ret) {
			chime_debug("DTLS failed: %s\n", gnutls_strerror(ret));
			gnutls_deinit(audio->dtls_sess);
			audio->dtls_sess = NULL;
			g_source_destroy(audio->dtls_source);
			audio->dtls_source = NULL;
			g_object_unref(audio->dtls_sock);
			audio->dtls_sock = NULL;
			if (audio->timeout_source)
				g_source_remove(audio->timeout_source);
			audio->timeout_source = 0;

			chime_call_transport_connect_ws(audio);
			return G_SOURCE_REMOVE;
		}

		chime_debug("DTLS established\n");
		g_source_remove(audio->timeout_source);
		audio->timeout_source = 0;
		audio->dtls_handshaked = TRUE;
		audio_send_auth_packet(audio);
		/* Fall through and receive data, not that it should be there */
	}

	unsigned char pkt[CHIME_DTLS_MTU];
	ssize_t len = gnutls_record_recv(audio->dtls_sess, pkt, sizeof(pkt));
	if (len > 0) {
		if (getenv("CHIME_AUDIO_DEBUG")) {
			printf("incoming:\n");
			hexdump(pkt, len);
		}
		audio_receive_packet(audio, pkt, len);
	}

	return G_SOURCE_CONTINUE;
}

static gboolean dtls_timeout(ChimeCallAudio *audio)
{
	audio->timeout_source = 0;

	dtls_src_cb(NULL, 0, audio);

	return G_SOURCE_REMOVE;
}

static int dtls_verify_cb(gnutls_session_t sess)
{
	ChimeCallAudio *audio = gnutls_session_get_ptr(sess);
	unsigned int status;
	int ret;

	ret = gnutls_certificate_verify_peers3(sess, audio->dtls_hostname, &status);
	if (ret != GNUTLS_E_SUCCESS)
		return ret;

	if (status) {
		gnutls_datum_t reasons;
		if (gnutls_certificate_verification_status_print(status, GNUTLS_CRT_X509, &reasons, 0) != GNUTLS_E_SUCCESS)
			reasons.data = NULL;
		chime_debug("DTLS certificate verification failed (%u): %s\n", status, reasons.data);
		gnutls_free(reasons.data);
		return -1;
	}
	return 0;
}

static void connect_dtls(ChimeCallAudio *audio, GSocket *s)
{
	/* Not that "connected" means anything except that we think we can route to it. */
	chime_debug("UDP socket connected\n");

	audio->dtls_source = g_datagram_based_create_source(G_DATAGRAM_BASED(s), G_IO_IN, audio->cancel);
	audio->dtls_sock = s;
	g_source_set_callback(audio->dtls_source, (GSourceFunc)dtls_src_cb, audio, NULL);
	g_source_attach(audio->dtls_source, NULL);

	gnutls_init(&audio->dtls_sess, GNUTLS_CLIENT|GNUTLS_DATAGRAM|GNUTLS_NONBLOCK);
	gnutls_set_default_priority(audio->dtls_sess);
	gnutls_session_set_ptr(audio->dtls_sess, audio);
	if (!audio->dtls_cred) {
		gnutls_certificate_allocate_credentials(&audio->dtls_cred);
		gnutls_certificate_set_x509_system_trust(audio->dtls_cred);
		gnutls_certificate_set_x509_trust_dir(audio->dtls_cred,
						      CHIME_CERTS_DIR, GNUTLS_X509_FMT_PEM);
		gnutls_certificate_set_verify_function(audio->dtls_cred, dtls_verify_cb);
	}
	gnutls_credentials_set(audio->dtls_sess, GNUTLS_CRD_CERTIFICATE, audio->dtls_cred);

	if (!audio->dtls_hostname) {
		gchar *hostname = g_strdup(chime_call_get_media_host(audio->call));
		if (!hostname)
			goto err;
		char *colon = strrchr(hostname, ':');
		if (!colon) {
			g_free(hostname);
			goto err;
		}
		*colon = 0;
		audio->dtls_hostname = hostname;
	}
	/* We can't rely on the length argument to gnutls_server_name_set().
	   https://bugs.launchpad.net/ubuntu/+bug/1762710 */
	gnutls_server_name_set(audio->dtls_sess, GNUTLS_NAME_DNS, audio->dtls_hostname, strlen(audio->dtls_hostname));

	gnutls_transport_set_ptr(audio->dtls_sess, audio);
	gnutls_transport_set_push_function (audio->dtls_sess,
					    g_tls_connection_gnutls_push_func);
	gnutls_transport_set_pull_function (audio->dtls_sess,
					    g_tls_connection_gnutls_pull_func);
	gnutls_transport_set_pull_timeout_function (audio->dtls_sess,
						    g_tls_connection_gnutls_pull_timeout_func);
	gnutls_transport_set_vec_push_function (audio->dtls_sess,
						g_tls_connection_gnutls_vec_push_func);
	gnutls_dtls_set_timeouts(audio->dtls_sess, 250, 2500);
	gnutls_dtls_set_mtu(audio->dtls_sess, CHIME_DTLS_MTU);

	if (gnutls_handshake(audio->dtls_sess) != GNUTLS_E_AGAIN) {
		chime_debug("Initial DTLS handshake failed\n");

		gnutls_deinit(audio->dtls_sess);
		audio->dtls_sess = NULL;

		if (audio->dtls_source) {
			g_source_destroy(audio->dtls_source);
			audio->dtls_source = NULL;
		}
		goto err;
	}

	int timeo = gnutls_dtls_get_timeout(audio->dtls_sess);
	audio->timeout_source = g_timeout_add(timeo, (GSourceFunc)dtls_timeout, audio);

	return;

 err:
	g_clear_object(&audio->dtls_sock);
	chime_call_transport_connect_ws(audio);
}

static void audio_dtls_one(GObject *obj, GAsyncResult *res, gpointer user_data)
{
	GSocketAddressEnumerator *enumerator = G_SOCKET_ADDRESS_ENUMERATOR(obj);
	ChimeCallAudio *audio = user_data;
	GError *error = NULL;

	GSocketAddress *addr = g_socket_address_enumerator_next_finish(enumerator, res, &error);
	if (!addr) {
		/* If it was cancelled, 'audio' may have been freed. */
		if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			chime_call_transport_connect_ws(audio);
		g_clear_error(&error);
		g_object_unref(obj);
		return;
	}

	GInetAddress *inet = g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(addr));
	guint16 port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(addr));
	gchar *addr_str = g_inet_address_to_string(inet);

	chime_debug("DTLS address %s:%d\n", addr_str, port);
	g_free(addr_str);

	GSocket *s = g_socket_new(g_socket_address_get_family(addr), G_SOCKET_TYPE_DATAGRAM,
				  G_SOCKET_PROTOCOL_UDP, NULL);
	if (!s)
		goto err;

	g_socket_set_blocking(s, FALSE);

	/* This doesn't block as it's a UDP connect */
	if (g_socket_connect(s, addr, NULL, NULL)) {
		/* Ideally, we should keep the enumerator around and try the next
		   address if the actual DTLS connection fails. */
		g_object_unref(addr);
		g_object_unref(enumerator);

		connect_dtls(audio, s);
		return;
	}

	/* Failed to connect (i.e. we can't route to it. Try next addresses... */
	g_object_unref(s);

 err:
	g_object_unref(addr);
	g_socket_address_enumerator_next_async(enumerator, audio->cancel,
					       (GAsyncReadyCallback)audio_dtls_one, audio);
}

void chime_call_transport_connect(ChimeCallAudio *audio, gboolean silent)
{
	audio->silent = silent;
	audio->cancel = g_cancellable_new();
	audio->dtls_handshaked = FALSE;
	audio->recv_ssrc = g_random_int();

	chime_call_audio_set_state(audio, CHIME_AUDIO_STATE_CONNECTING, NULL);

	GSocketConnectable *addr = g_network_address_parse(chime_call_get_media_host(audio->call),
							   0, NULL);
	if (!addr) {
		chime_call_transport_connect_ws(audio);
		return;
	}
	GSocketAddressEnumerator *enumerator = g_socket_connectable_enumerate(addr);
	g_object_unref(addr);

	g_socket_address_enumerator_next_async(enumerator, audio->cancel,
					       (GAsyncReadyCallback)audio_dtls_one, audio);
}


static void on_final_audiows_close(SoupWebsocketConnection *ws, gpointer _unused)
{
	chime_debug("audio ws close\n");
	g_object_unref(ws);
}


void chime_call_transport_disconnect(ChimeCallAudio *audio, gboolean hangup)
{
	if (audio->send_rt_source) {
		g_source_remove(audio->send_rt_source);
		audio->send_rt_source = 0;
	}

	g_hash_table_remove_all(audio->profiles);

	chime_call_audio_cleanup_datamsgs(audio);

	if (hangup && audio->state >= CHIME_AUDIO_STATE_AUDIOLESS)
		audio_send_hangup_packet(audio);

	g_mutex_lock(&audio->transport_lock);

	if (audio->cancel) {
		g_cancellable_cancel(audio->cancel);
		g_object_unref(audio->cancel);
		audio->cancel = NULL;
	}
	if (audio->ws) {
		g_signal_handlers_disconnect_matched(G_OBJECT(audio->ws), G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, audio);
		g_signal_connect(G_OBJECT(audio->ws), "closed", G_CALLBACK(on_final_audiows_close), NULL);
		soup_websocket_connection_close(audio->ws, 0, NULL);
		audio->ws = NULL;
	} else if (audio->dtls_sess) {
		gnutls_deinit(audio->dtls_sess);
		audio->dtls_sess = NULL;

		if (audio->dtls_source) {
			g_source_destroy(audio->dtls_source);
			audio->dtls_source = NULL;
		}
		g_clear_object(&audio->dtls_sock);
	}

	if (audio->dtls_hostname) {
		g_free(audio->dtls_hostname);
		audio->dtls_hostname = NULL;
	}

	if (audio->timeout_source) {
		g_source_remove(audio->timeout_source);
		audio->timeout_source = 0;
	}

	if (hangup && audio->dtls_cred) {
		gnutls_certificate_free_credentials(audio->dtls_cred);
		audio->dtls_cred = NULL;
	}

	g_mutex_unlock(&audio->transport_lock);
}

void chime_call_transport_send_packet(ChimeCallAudio *audio, enum xrp_pkt_type type, const ProtobufCMessage *message)
{
	if (!audio->ws && !audio->dtls_sess)
		return;

	size_t len = protobuf_c_message_get_packed_size(message);

	len += sizeof(struct xrp_header);
	struct xrp_header *hdr = g_malloc0(len);
	hdr->type = htons(type);
	hdr->len = htons(len);
	protobuf_c_message_pack(message, (void *)(hdr + 1));
	if (getenv("CHIME_AUDIO_DEBUG")) {
		printf("sending protobuf of len %"G_GSIZE_FORMAT"\n", len);
		hexdump(hdr, len);
	}
	g_mutex_lock(&audio->transport_lock);
	if (audio->dtls_sess)
		gnutls_record_send(audio->dtls_sess, hdr, len);
	else if (audio->ws)
		soup_websocket_connection_send_binary(audio->ws, hdr, len);
	g_mutex_unlock(&audio->transport_lock);
	g_free(hdr);
}
