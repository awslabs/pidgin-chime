#ifndef USE_LIBSOUP_WEBSOCKETS

/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * soup-websocket-connection.c: This file was originally part of Cockpit.
 *
 * Copyright 2013, 2014 Red Hat, Inc.
 *
 * Cockpit is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * Cockpit is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>

#include <libsoup/soup.h>
#include "chime-websocket-connection.h"

/*
 * SECTION:websocketconnection
 * @title: SoupWebsocketConnection
 * @short_description: A WebSocket connection
 *
 * A #SoupWebsocketConnection is a WebSocket connection to a peer.
 * This API is modeled after the W3C API for interacting with
 * WebSockets.
 *
 * The #SoupWebsocketConnection:state property will indicate the
 * state of the connection.
 *
 * Use chime_websocket_connection_send() to send a message to the peer.
 * When a message is received the #SoupWebsocketConnection::message
 * signal will fire.
 *
 * The chime_websocket_connection_close() function will perform an
 * orderly close of the connection. The
 * #SoupWebsocketConnection::closed signal will fire once the
 * connection closes, whether it was initiated by this side or the
 * peer.
 *
 * Connect to the #ChimeWebsocketConnection::closing signal to detect
 * when either peer begins closing the connection.
 */

/**
 * ChimeWebsocketConnection:
 *
 * A class representing a WebSocket connection.
 *
 * Since: 2.50
 */

/**
 * ChimeWebsocketConnectionClass:
 * @message: default handler for the #ChimeWebsocketConnection::message signal
 * @error: default handler for the #ChimeWebsocketConnection::error signal
 * @closing: the default handler for the #ChimeWebsocketConnection:closing signal
 * @closed: default handler for the #ChimeWebsocketConnection::closed signal
 *
 * The abstract base class for #ChimeWebsocketConnection
 *
 * Since: 2.50
 */

enum {
	PROP_0,
	PROP_IO_STREAM,
	PROP_CONNECTION_TYPE,
	PROP_URI,
	PROP_ORIGIN,
	PROP_PROTOCOL,
	PROP_STATE,
	PROP_MAX_INCOMING_PAYLOAD_SIZE,
	PROP_KEEPALIVE_INTERVAL,
};

enum {
	MESSAGE,
	ERROR,
	CLOSING,
	CLOSED,
	PONG,
	NUM_SIGNALS
};

static guint signals[NUM_SIGNALS] = { 0, };

typedef struct {
	GBytes *data;
	gboolean last;
	gsize sent;
	gsize amount;
} Frame;

struct _ChimeWebsocketConnectionPrivate {
	GIOStream *io_stream;
	SoupWebsocketConnectionType connection_type;
	SoupURI *uri;
	char *origin;
	char *protocol;
	guint64 max_incoming_payload_size;
	guint keepalive_interval;

	gushort peer_close_code;
	char *peer_close_data;
	gboolean close_sent;
	gboolean close_received;
	gboolean dirty_close;
	GSource *close_timeout;

	GMainContext *main_context;

	gboolean io_closing;
	gboolean io_closed;

	GPollableInputStream *input;
	GSource *input_source;
	GByteArray *incoming;

	GPollableOutputStream *output;
	GSource *output_source;
	GQueue outgoing;

	/* Current message being assembled */
	guint8 message_opcode;
	GByteArray *message_data;

	GSource *keepalive_timeout;
};

#define MAX_INCOMING_PAYLOAD_SIZE_DEFAULT   128 * 1024

G_DEFINE_TYPE_WITH_PRIVATE (ChimeWebsocketConnection, chime_websocket_connection, G_TYPE_OBJECT)

typedef enum {
	CHIME_WEBSOCKET_QUEUE_NORMAL = 0,
	CHIME_WEBSOCKET_QUEUE_URGENT = 1 << 0,
	CHIME_WEBSOCKET_QUEUE_LAST = 1 << 1,
} ChimeWebsocketQueueFlags;

static void queue_frame (ChimeWebsocketConnection *self, ChimeWebsocketQueueFlags flags,
			 gpointer data, gsize len, gsize amount);

static void
frame_free (gpointer data)
{
	Frame *frame = data;

	if (frame) {
		g_bytes_unref (frame->data);
		g_slice_free (Frame, frame);
	}
}

static void
chime_websocket_connection_init (ChimeWebsocketConnection *self)
{
	ChimeWebsocketConnectionPrivate *pv;

	pv = self->pv = chime_websocket_connection_get_instance_private (self);

	pv->incoming = g_byte_array_sized_new (1024);
	g_queue_init (&pv->outgoing);
	pv->main_context = g_main_context_ref_thread_default ();
}

static void
on_iostream_closed (GObject *source,
                    GAsyncResult *result,
                    gpointer user_data)
{
	ChimeWebsocketConnection *self = user_data;
	ChimeWebsocketConnectionPrivate *pv = self->pv;
	GError *error = NULL;

	/* We treat connection as closed even if close fails */
	pv->io_closed = TRUE;
	g_io_stream_close_finish (pv->io_stream, result, &error);

	if (error) {
		g_debug ("error closing web socket stream: %s", error->message);
		if (!pv->dirty_close)
			g_signal_emit (self, signals[ERROR], 0, error);
		pv->dirty_close = TRUE;
		g_error_free (error);
	}

	g_assert (chime_websocket_connection_get_state (self) == SOUP_WEBSOCKET_STATE_CLOSED);
	g_debug ("closed: completed io stream close");
	g_signal_emit (self, signals[CLOSED], 0);

	g_object_unref (self);
}

static void
stop_input (ChimeWebsocketConnection *self)
{
	ChimeWebsocketConnectionPrivate *pv = self->pv;

	if (pv->input_source) {
		g_debug ("stopping input source");
		g_source_destroy (pv->input_source);
		g_source_unref (pv->input_source);
		pv->input_source = NULL;
	}
}

static void
stop_output (ChimeWebsocketConnection *self)
{
	ChimeWebsocketConnectionPrivate *pv = self->pv;

	if (pv->output_source) {
		g_debug ("stopping output source");
		g_source_destroy (pv->output_source);
		g_source_unref (pv->output_source);
		pv->output_source = NULL;
	}
}

static void
keepalive_stop_timeout (ChimeWebsocketConnection *self)
{
	ChimeWebsocketConnectionPrivate *pv = self->pv;

	if (pv->keepalive_timeout) {
		g_source_destroy (pv->keepalive_timeout);
		g_source_unref (pv->keepalive_timeout);
		pv->keepalive_timeout = NULL;
	}
}

static void
close_io_stop_timeout (ChimeWebsocketConnection *self)
{
	ChimeWebsocketConnectionPrivate *pv = self->pv;

	if (pv->close_timeout) {
		g_source_destroy (pv->close_timeout);
		g_source_unref (pv->close_timeout);
		pv->close_timeout = NULL;
	}
}

static void
close_io_stream (ChimeWebsocketConnection *self)
{
	ChimeWebsocketConnectionPrivate *pv = self->pv;

	keepalive_stop_timeout (self);
	close_io_stop_timeout (self);

	if (!pv->io_closing) {
		stop_input (self);
		stop_output (self);
		pv->io_closing = TRUE;
		g_debug ("closing io stream");
		g_io_stream_close_async (pv->io_stream, G_PRIORITY_DEFAULT,
					 NULL, on_iostream_closed, g_object_ref (self));
	}

	g_object_notify (G_OBJECT (self), "state");
}

static void
shutdown_wr_io_stream (ChimeWebsocketConnection *self)
{
	ChimeWebsocketConnectionPrivate *pv = self->pv;
	GSocket *socket;
	GError *error = NULL;

	stop_output (self);

	if (G_IS_SOCKET_CONNECTION (pv->io_stream)) {
		socket = g_socket_connection_get_socket (G_SOCKET_CONNECTION (pv->io_stream));
		g_socket_shutdown (socket, FALSE, TRUE, &error);
		if (error != NULL) {
			g_debug ("error shutting down io stream: %s", error->message);
			g_error_free (error);
		}
	}

	g_object_notify (G_OBJECT (self), "state");
}

static gboolean
on_timeout_close_io (gpointer user_data)
{
	ChimeWebsocketConnection *self = CHIME_WEBSOCKET_CONNECTION (user_data);
	ChimeWebsocketConnectionPrivate *pv = self->pv;

	pv->close_timeout = 0;

	g_debug ("peer did not close io when expected");
	close_io_stream (self);

	return FALSE;
}

static void
close_io_after_timeout (ChimeWebsocketConnection *self)
{
	ChimeWebsocketConnectionPrivate *pv = self->pv;
	const int timeout = 5;

	if (pv->close_timeout)
		return;

	g_debug ("waiting %d seconds for peer to close io", timeout);
	pv->close_timeout = g_timeout_source_new_seconds (timeout);
	g_source_set_callback (pv->close_timeout, on_timeout_close_io, self, NULL);
	g_source_attach (pv->close_timeout, pv->main_context);
}

static void
xor_with_mask (const guint8 *mask,
	       guint8 *data,
	       gsize len)
{
	gsize n;

	/* Do the masking */
	for (n = 0; n < len; n++)
		data[n] ^= mask[n & 3];
}

static void
send_message (ChimeWebsocketConnection *self,
	      ChimeWebsocketQueueFlags flags,
	      guint8 opcode,
	      const guint8 *data,
	      gsize length)
{
	gsize buffered_amount = length;
	GByteArray *bytes;
	gsize frame_len;
	guint8 *outer;
	guint8 *mask = 0;
	guint8 *at;

	if (!(chime_websocket_connection_get_state (self) == SOUP_WEBSOCKET_STATE_OPEN)) {
		g_debug ("Ignoring message since the connection is closed or is closing");
		return;
	}

	bytes = g_byte_array_sized_new (14 + length);
	outer = bytes->data;
	outer[0] = 0x80 | opcode;

	/* If control message, truncate payload */
	if (opcode & 0x08) {
		if (length > 125) {
			g_warning ("Truncating WebSocket control message payload");
			length = 125;
		}

		buffered_amount = 0;
	}

	if (length < 126) {
		outer[1] = (0xFF & length); /* mask | 7-bit-len */
		bytes->len = 2;
	} else if (length < 65536) {
		outer[1] = 126; /* mask | 16-bit-len */
		outer[2] = (length >> 8) & 0xFF;
		outer[3] = (length >> 0) & 0xFF;
		bytes->len = 4;
	} else {
		outer[1] = 127; /* mask | 64-bit-len */
#if GLIB_SIZEOF_SIZE_T > 4
		outer[2] = (length >> 56) & 0xFF;
		outer[3] = (length >> 48) & 0xFF;
		outer[4] = (length >> 40) & 0xFF;
		outer[5] = (length >> 32) & 0xFF;
#else
		outer[2] = outer[3] = outer[4] = outer[5] = 0;
#endif
		outer[6] = (length >> 24) & 0xFF;
		outer[7] = (length >> 16) & 0xFF;
		outer[8] = (length >> 8) & 0xFF;
		outer[9] = (length >> 0) & 0xFF;
		bytes->len = 10;
	}

	/* The server side doesn't need to mask, so we don't. There's
	 * probably a client somewhere that's not expecting it.
	 */
	if (self->pv->connection_type == SOUP_WEBSOCKET_CONNECTION_CLIENT) {
		outer[1] |= 0x80;
		mask = outer + bytes->len;
		* ((guint32 *)mask) = g_random_int ();
		bytes->len += 4;
	}

	at = bytes->data + bytes->len;
	g_byte_array_append (bytes, data, length);

	if (self->pv->connection_type == SOUP_WEBSOCKET_CONNECTION_CLIENT)
		xor_with_mask (mask, at, length);

	frame_len = bytes->len;
	queue_frame (self, flags, g_byte_array_free (bytes, FALSE),
		     frame_len, buffered_amount);
	g_debug ("queued %d frame of len %u", (int)opcode, (guint)frame_len);
}

static void
send_close (ChimeWebsocketConnection *self,
	    ChimeWebsocketQueueFlags flags,
	    gushort code,
	    const char *reason)
{
	/* Note that send_message truncates as expected */
	char buffer[128];
	gsize len = 0;

	if (code != 0) {
		buffer[len++] = code >> 8;
		buffer[len++] = code & 0xFF;
		if (reason)
			len += g_strlcpy (buffer + len, reason, sizeof (buffer) - len);
	}

	send_message (self, flags, 0x08, (guint8 *)buffer, len);
	self->pv->close_sent = TRUE;

	keepalive_stop_timeout (self);
}

static void
emit_error_and_close (ChimeWebsocketConnection *self,
		      GError *error,
		      gboolean prejudice)
{
	gboolean ignore = FALSE;
	gushort code;

	if (chime_websocket_connection_get_state (self) == SOUP_WEBSOCKET_STATE_CLOSED) {
		g_error_free (error);
		return;
	}

	if (error && error->domain == SOUP_WEBSOCKET_ERROR)
		code = error->code;
	else
		code = SOUP_WEBSOCKET_CLOSE_GOING_AWAY;

	self->pv->dirty_close = TRUE;
	g_signal_emit (self, signals[ERROR], 0, error);
	g_error_free (error);

	/* If already closing, just ignore this stuff */
	switch (chime_websocket_connection_get_state (self)) {
	case SOUP_WEBSOCKET_STATE_CLOSED:
		ignore = TRUE;
		break;
	case SOUP_WEBSOCKET_STATE_CLOSING:
		ignore = !prejudice;
		break;
	default:
		break;
	}

	if (ignore) {
		g_debug ("already closing/closed, ignoring error");
	} else if (prejudice) {
		g_debug ("forcing close due to error");
		close_io_stream (self);
	} else {
		g_debug ("requesting close due to error");
		send_close (self, CHIME_WEBSOCKET_QUEUE_URGENT | CHIME_WEBSOCKET_QUEUE_LAST, code, NULL);
	}
}

static void
protocol_error_and_close_full (ChimeWebsocketConnection *self,
                               gboolean prejudice)
{
	GError *error;

	error = g_error_new_literal (SOUP_WEBSOCKET_ERROR,
				     SOUP_WEBSOCKET_CLOSE_PROTOCOL_ERROR,
				     self->pv->connection_type == SOUP_WEBSOCKET_CONNECTION_SERVER ?
				     "Received invalid WebSocket response from the client" :
				     "Received invalid WebSocket response from the server");
	emit_error_and_close (self, error, prejudice);
}

static void
protocol_error_and_close (ChimeWebsocketConnection *self)
{
	protocol_error_and_close_full (self, FALSE);
}

static void
bad_data_error_and_close (ChimeWebsocketConnection *self)
{
	GError *error;

	error = g_error_new_literal (SOUP_WEBSOCKET_ERROR,
				     SOUP_WEBSOCKET_CLOSE_BAD_DATA,
				     self->pv->connection_type == SOUP_WEBSOCKET_CONNECTION_SERVER ?
				     "Received invalid WebSocket data from the client" :
				     "Received invalid WebSocket data from the server");
	emit_error_and_close (self, error, FALSE);
}

static void
too_big_error_and_close (ChimeWebsocketConnection *self,
                         guint64 payload_len)
{
	GError *error;

	error = g_error_new_literal (SOUP_WEBSOCKET_ERROR,
				     SOUP_WEBSOCKET_CLOSE_TOO_BIG,
				     self->pv->connection_type == SOUP_WEBSOCKET_CONNECTION_SERVER ?
				     "Received extremely large WebSocket data from the client" :
				     "Received extremely large WebSocket data from the server");
	g_debug ("%s is trying to frame of size %" G_GUINT64_FORMAT " or greater, but max supported size is %" G_GUINT64_FORMAT,
		 self->pv->connection_type == SOUP_WEBSOCKET_CONNECTION_SERVER ? "server" : "client",
	         payload_len, self->pv->max_incoming_payload_size);
	emit_error_and_close (self, error, TRUE);

	/* The input is in an invalid state now */
	stop_input (self);
}

static void
close_connection (ChimeWebsocketConnection *self,
                  gushort                  code,
                  const char              *data)
{
	ChimeWebsocketQueueFlags flags;
	ChimeWebsocketConnectionPrivate *pv;

	pv = self->pv;

	if (pv->close_sent) {
		g_debug ("close code already sent");
		return;
	}

	/* Validate the closing code received by the peer */
	switch (code) {
	case SOUP_WEBSOCKET_CLOSE_NORMAL:
	case SOUP_WEBSOCKET_CLOSE_GOING_AWAY:
	case SOUP_WEBSOCKET_CLOSE_PROTOCOL_ERROR:
	case SOUP_WEBSOCKET_CLOSE_UNSUPPORTED_DATA:
	case SOUP_WEBSOCKET_CLOSE_BAD_DATA:
	case SOUP_WEBSOCKET_CLOSE_POLICY_VIOLATION:
	case SOUP_WEBSOCKET_CLOSE_TOO_BIG:
		break;
	case SOUP_WEBSOCKET_CLOSE_NO_EXTENSION:
		if (pv->connection_type == SOUP_WEBSOCKET_CONNECTION_SERVER) {
			g_debug ("Wrong closing code %d received for a server connection",
			         code);
		}
		break;
	case SOUP_WEBSOCKET_CLOSE_SERVER_ERROR:
		if (pv->connection_type != SOUP_WEBSOCKET_CONNECTION_SERVER) {
			g_debug ("Wrong closing code %d received for a non server connection",
			         code);
		}
		break;
	default:
		g_debug ("Wrong closing code %d received", code);
	}

	g_signal_emit (self, signals[CLOSING], 0);

	if (pv->close_received)
		g_debug ("responding to close request");

	flags = 0;
	if (pv->connection_type == SOUP_WEBSOCKET_CONNECTION_SERVER && pv->close_received)
		flags |= CHIME_WEBSOCKET_QUEUE_LAST;
	send_close (self, flags, code, data);
	close_io_after_timeout (self);
}

static void
receive_close (ChimeWebsocketConnection *self,
	       const guint8 *data,
	       gsize len)
{
	ChimeWebsocketConnectionPrivate *pv = self->pv;

	pv->peer_close_code = 0;
	g_free (pv->peer_close_data);
	pv->peer_close_data = NULL;
	pv->close_received = TRUE;

	/* Store the code/data payload */
	if (len >= 2) {
		pv->peer_close_code = (guint16)data[0] << 8 | data[1];
	}
	if (len > 2) {
		data += 2;
		len -= 2;
		if (g_utf8_validate ((char *)data, len, NULL))
			pv->peer_close_data = g_strndup ((char *)data, len);
		else
			g_debug ("received non-UTF8 close data: %d '%.*s' %d", (int)len, (int)len, (char *)data, (int)data[0]);
	}

	/* Once we receive close response on server, close immediately */
	if (pv->close_sent) {
		shutdown_wr_io_stream (self);
		if (pv->connection_type == SOUP_WEBSOCKET_CONNECTION_SERVER)
			close_io_stream (self);
	} else {
		close_connection (self, pv->peer_close_code, NULL);
	}
}

static void
receive_ping (ChimeWebsocketConnection *self,
	      const guint8 *data,
	      gsize len)
{
	/* Send back a pong with same data */
	g_debug ("received ping, responding");
	send_message (self, CHIME_WEBSOCKET_QUEUE_URGENT, 0x0A, data, len);
}

static void
receive_pong (ChimeWebsocketConnection *self,
	      const guint8 *data,
	      gsize len)
{
	GByteArray *byte_array;
	GBytes *bytes;

	g_debug ("received pong message");

	byte_array = g_byte_array_sized_new (len + 1);
	g_byte_array_append (byte_array, data, len);
	/* Always null terminate, as a convenience */
	g_byte_array_append (byte_array, (guchar *)"\0", 1);
	/* But don't include the null terminator in the byte count */
	byte_array->len--;

	bytes = g_byte_array_free_to_bytes (byte_array);
	g_signal_emit (self, signals[PONG], 0, bytes);
	g_bytes_unref (bytes);
}

static void
process_contents (ChimeWebsocketConnection *self,
		  gboolean control,
		  gboolean fin,
		  guint8 opcode,
		  gconstpointer payload,
		  gsize payload_len)
{
	ChimeWebsocketConnectionPrivate *pv = self->pv;
	GBytes *message;

	if (control) {
		/* Control frames must never be fragmented */
		if (!fin) {
			g_debug ("received fragmented control frame");
			protocol_error_and_close (self);
			return;
		}

		g_debug ("received control frame %d with %d payload", (int)opcode, (int)payload_len);

		switch (opcode) {
		case 0x08:
			receive_close (self, payload, payload_len);
			break;
		case 0x09:
			receive_ping (self, payload, payload_len);
			break;
		case 0x0A:
			receive_pong (self, payload, payload_len);
			break;
		default:
			g_debug ("received unsupported control frame: %d", (int)opcode);
			break;
		}
	} else if (pv->close_received) {
		g_debug ("received message after close was received");
	} else {
		/* A message frame */

		if (!fin && opcode) {
			/* Initial fragment of a message */
			if (pv->message_data) {
				g_debug ("received out of order inital message fragment");
				protocol_error_and_close (self);
				return;
			}
			g_debug ("received inital fragment frame %d with %d payload", (int)opcode, (int)payload_len);
		} else if (!fin && !opcode) {
			/* Middle fragment of a message */
			if (!pv->message_data) {
				g_debug ("received out of order middle message fragment");
				protocol_error_and_close (self);
				return;
			}
			g_debug ("received middle fragment frame with %d payload", (int)payload_len);
		} else if (fin && !opcode) {
			/* Last fragment of a message */
			if (!pv->message_data) {
				g_debug ("received out of order ending message fragment");
				protocol_error_and_close (self);
				return;
			}
			g_debug ("received last fragment frame with %d payload", (int)payload_len);
		} else {
			/* An unfragmented message */
			g_assert (opcode != 0);
			if (pv->message_data) {
				g_debug ("received unfragmented message when fragment was expected");
				protocol_error_and_close (self);
				return;
			}
			g_debug ("received frame %d with %d payload", (int)opcode, (int)payload_len);
		}

		if (opcode) {
			pv->message_opcode = opcode;
			pv->message_data = g_byte_array_sized_new (payload_len + 1);
		}

		switch (pv->message_opcode) {
		case 0x01:
			if (!g_utf8_validate ((char *)payload, payload_len, NULL)) {
				g_debug ("received invalid non-UTF8 text data");

				/* Discard the entire message */
				g_byte_array_unref (pv->message_data);
				pv->message_data = NULL;
				pv->message_opcode = 0;

				bad_data_error_and_close (self);
				return;
			}
			/* fall through */
		case 0x02:
			g_byte_array_append (pv->message_data, payload, payload_len);
			break;
		default:
			g_debug ("received unknown data frame: %d", (int)opcode);
			break;
		}

		/* Actually deliver the message? */
		if (fin) {
			/* Always null terminate, as a convenience */
			g_byte_array_append (pv->message_data, (guchar *)"\0", 1);

			/* But don't include the null terminator in the byte count */
			pv->message_data->len--;

			opcode = pv->message_opcode;
			message = g_byte_array_free_to_bytes (pv->message_data);
			pv->message_data = NULL;
			pv->message_opcode = 0;
			g_debug ("message: delivering %d with %d length",
				 (int)opcode, (int)g_bytes_get_size (message));
			g_signal_emit (self, signals[MESSAGE], 0, (int)opcode, message);
			g_bytes_unref (message);
		}
	}
}

static gboolean
process_frame (ChimeWebsocketConnection *self)
{
	guint8 *header;
	guint8 *payload;
	guint64 payload_len;
	guint8 *mask;
	gboolean fin;
	gboolean control;
	gboolean masked;
	guint8 opcode;
	gsize len;
	gsize at;

	len = self->pv->incoming->len;
	if (len < 2)
		return FALSE; /* need more data */

	header = self->pv->incoming->data;
	fin = ((header[0] & 0x80) != 0);
	control = header[0] & 0x08;
	opcode = header[0] & 0x0f;
	masked = ((header[1] & 0x80) != 0);

	switch (header[1] & 0x7f) {
	case 126:
		at = 4;
		if (len < at)
			return FALSE; /* need more data */
		payload_len = (((guint16)header[2] << 8) |
			       ((guint16)header[3] << 0));
		break;
	case 127:
		at = 10;
		if (len < at)
			return FALSE; /* need more data */
		payload_len = (((guint64)header[2] << 56) |
			       ((guint64)header[3] << 48) |
			       ((guint64)header[4] << 40) |
			       ((guint64)header[5] << 32) |
			       ((guint64)header[6] << 24) |
			       ((guint64)header[7] << 16) |
			       ((guint64)header[8] << 8) |
			       ((guint64)header[9] << 0));
		break;
	default:
		payload_len = header[1] & 0x7f;
		at = 2;
		break;
	}

	/* Safety valve */
	if (self->pv->max_incoming_payload_size > 0 &&
	    payload_len >= self->pv->max_incoming_payload_size) {
		too_big_error_and_close (self, payload_len);
		return FALSE;
	}

	if (len < at + payload_len)
		return FALSE; /* need more data */

	payload = header + at;

	if (masked) {
		mask = header + at;
		payload += 4;
		at += 4;

		if (len < at + payload_len)
			return FALSE; /* need more data */

		xor_with_mask (mask, payload, payload_len);
	}

	/* Note that now that we've unmasked, we've modified the buffer, we can
	 * only return below via discarding or processing the message
	 */
	process_contents (self, control, fin, opcode, payload, payload_len);

	/* Move past the parsed frame */
	g_byte_array_remove_range (self->pv->incoming, 0, at + payload_len);
	return TRUE;
}

static void
process_incoming (ChimeWebsocketConnection *self)
{
	while (process_frame (self))
		;
}

static gboolean
on_web_socket_input (GObject *pollable_stream,
		     gpointer user_data)
{
	ChimeWebsocketConnection *self = CHIME_WEBSOCKET_CONNECTION (user_data);
	ChimeWebsocketConnectionPrivate *pv = self->pv;
	GError *error = NULL;
	gboolean end = FALSE;
	gssize count;
	gsize len;

	do {
		len = pv->incoming->len;
		g_byte_array_set_size (pv->incoming, len + 1024);

		count = g_pollable_input_stream_read_nonblocking (pv->input,
								  pv->incoming->data + len,
								  1024, NULL, &error);

		if (count < 0) {
			if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
				g_error_free (error);
				count = 0;
			} else {
				emit_error_and_close (self, error, TRUE);
				return TRUE;
			}
		} else if (count == 0) {
			end = TRUE;
		}

		pv->incoming->len = len + count;
	} while (count > 0);

	process_incoming (self);

	if (end) {
		if (!pv->close_sent || !pv->close_received) {
			pv->dirty_close = TRUE;
			g_debug ("connection unexpectedly closed by peer");
		} else {
			g_debug ("peer has closed socket");
		}

		close_io_stream (self);
	}

	return TRUE;
}

static gboolean
on_web_socket_output (GObject *pollable_stream,
		      gpointer user_data)
{
	ChimeWebsocketConnection *self = CHIME_WEBSOCKET_CONNECTION (user_data);
	ChimeWebsocketConnectionPrivate *pv = self->pv;
	const guint8 *data;
	GError *error = NULL;
	Frame *frame;
	gssize count;
	gsize len;

	if (chime_websocket_connection_get_state (self) == SOUP_WEBSOCKET_STATE_CLOSED) {
		g_debug ("Ignoring message since the connection is closed");
		stop_output (self);
		return TRUE;
	}

	frame = g_queue_peek_head (&pv->outgoing);

	/* No more frames to send */
	if (frame == NULL) {
		stop_output (self);
		return TRUE;
	}

	data = g_bytes_get_data (frame->data, &len);
	g_assert (len > 0);
	g_assert (len > frame->sent);

	count = g_pollable_output_stream_write_nonblocking (pv->output,
							    data + frame->sent,
							    len - frame->sent,
							    NULL, &error);

	if (count < 0) {
		if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
			g_clear_error (&error);
			count = 0;
		} else {
			emit_error_and_close (self, error, TRUE);
			return FALSE;
		}
	}

	frame->sent += count;
	if (frame->sent >= len) {
		g_debug ("sent frame");
		g_queue_pop_head (&pv->outgoing);

		if (frame->last) {
			if (pv->connection_type == SOUP_WEBSOCKET_CONNECTION_SERVER) {
				close_io_stream (self);
			} else {
				shutdown_wr_io_stream (self);
				close_io_after_timeout (self);
			}
		}
		frame_free (frame);
	}

	return TRUE;
}

static void
start_output (ChimeWebsocketConnection *self)
{
	ChimeWebsocketConnectionPrivate *pv = self->pv;

	if (pv->output_source)
		return;

	g_debug ("starting output source");
	pv->output_source = g_pollable_output_stream_create_source (pv->output, NULL);
	g_source_set_callback (pv->output_source, (GSourceFunc)on_web_socket_output, self, NULL);
	g_source_attach (pv->output_source, pv->main_context);
}

static void
queue_frame (ChimeWebsocketConnection *self,
	     ChimeWebsocketQueueFlags flags,
	     gpointer data,
	     gsize len,
	     gsize amount)
{
	ChimeWebsocketConnectionPrivate *pv = self->pv;
	Frame *frame;
	Frame *prev;

	g_return_if_fail (CHIME_IS_WEBSOCKET_CONNECTION (self));
	g_return_if_fail (pv->close_sent == FALSE);
	g_return_if_fail (data != NULL);
	g_return_if_fail (len > 0);

	frame = g_slice_new0 (Frame);
	frame->data = g_bytes_new_take (data, len);
	frame->amount = amount;
	frame->last = (flags & CHIME_WEBSOCKET_QUEUE_LAST) ? TRUE : FALSE;

	/* If urgent put at front of queue */
	if (flags & CHIME_WEBSOCKET_QUEUE_URGENT) {
		/* But we can't interrupt a message already partially sent */
		prev = g_queue_pop_head (&pv->outgoing);
		if (prev == NULL) {
			g_queue_push_head (&pv->outgoing, frame);
		} else if (prev->sent > 0) {
			g_queue_push_head (&pv->outgoing, frame);
			g_queue_push_head (&pv->outgoing, prev);
		} else {
			g_queue_push_head (&pv->outgoing, prev);
			g_queue_push_head (&pv->outgoing, frame);
		}
	} else {
		g_queue_push_tail (&pv->outgoing, frame);
	}

	start_output (self);
}

static void
chime_websocket_connection_constructed (GObject *object)
{
	ChimeWebsocketConnection *self = CHIME_WEBSOCKET_CONNECTION (object);
	ChimeWebsocketConnectionPrivate *pv = self->pv;
	GInputStream *is;
	GOutputStream *os;

	G_OBJECT_CLASS (chime_websocket_connection_parent_class)->constructed (object);

	g_return_if_fail (pv->io_stream != NULL);

	is = g_io_stream_get_input_stream (pv->io_stream);
	g_return_if_fail (G_IS_POLLABLE_INPUT_STREAM (is));
	pv->input = G_POLLABLE_INPUT_STREAM (is);
	g_return_if_fail (g_pollable_input_stream_can_poll (pv->input));

	os = g_io_stream_get_output_stream (pv->io_stream);
	g_return_if_fail (G_IS_POLLABLE_OUTPUT_STREAM (os));
	pv->output = G_POLLABLE_OUTPUT_STREAM (os);
	g_return_if_fail (g_pollable_output_stream_can_poll (pv->output));

	pv->input_source = g_pollable_input_stream_create_source (pv->input, NULL);
	g_source_set_callback (pv->input_source, (GSourceFunc)on_web_socket_input, self, NULL);
	g_source_attach (pv->input_source, pv->main_context);
}

static void
chime_websocket_connection_get_property (GObject *object,
					guint prop_id,
					GValue *value,
					GParamSpec *pspec)
{
	ChimeWebsocketConnection *self = CHIME_WEBSOCKET_CONNECTION (object);
	ChimeWebsocketConnectionPrivate *pv = self->pv;

	switch (prop_id) {
	case PROP_IO_STREAM:
		g_value_set_object (value, chime_websocket_connection_get_io_stream (self));
		break;

	case PROP_CONNECTION_TYPE:
		g_value_set_enum (value, chime_websocket_connection_get_connection_type (self));
		break;

	case PROP_URI:
		g_value_set_boxed (value, chime_websocket_connection_get_uri (self));
		break;

	case PROP_ORIGIN:
		g_value_set_string (value, chime_websocket_connection_get_origin (self));
		break;

	case PROP_PROTOCOL:
		g_value_set_string (value, chime_websocket_connection_get_protocol (self));
		break;

	case PROP_STATE:
		g_value_set_enum (value, chime_websocket_connection_get_state (self));
		break;

	case PROP_MAX_INCOMING_PAYLOAD_SIZE:
		g_value_set_uint64 (value, pv->max_incoming_payload_size);
		break;

	case PROP_KEEPALIVE_INTERVAL:
		g_value_set_uint (value, pv->keepalive_interval);
		break;

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
chime_websocket_connection_set_property (GObject *object,
					guint prop_id,
					const GValue *value,
					GParamSpec *pspec)
{
	ChimeWebsocketConnection *self = CHIME_WEBSOCKET_CONNECTION (object);
	ChimeWebsocketConnectionPrivate *pv = self->pv;

	switch (prop_id) {
	case PROP_IO_STREAM:
		g_return_if_fail (pv->io_stream == NULL);
		pv->io_stream = g_value_dup_object (value);
		break;

	case PROP_CONNECTION_TYPE:
		pv->connection_type = g_value_get_enum (value);
		break;

	case PROP_URI:
		g_return_if_fail (pv->uri == NULL);
		pv->uri = g_value_dup_boxed (value);
		break;

	case PROP_ORIGIN:
		g_return_if_fail (pv->origin == NULL);
		pv->origin = g_value_dup_string (value);
		break;

	case PROP_PROTOCOL:
		g_return_if_fail (pv->protocol == NULL);
		pv->protocol = g_value_dup_string (value);
		break;

	case PROP_MAX_INCOMING_PAYLOAD_SIZE:
		pv->max_incoming_payload_size = g_value_get_uint64 (value);
		break;

	case PROP_KEEPALIVE_INTERVAL:
		chime_websocket_connection_set_keepalive_interval (self,
		                                                  g_value_get_uint (value));
		break;

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
chime_websocket_connection_dispose (GObject *object)
{
	ChimeWebsocketConnection *self = CHIME_WEBSOCKET_CONNECTION (object);

	self->pv->dirty_close = TRUE;
	close_io_stream (self);

	G_OBJECT_CLASS (chime_websocket_connection_parent_class)->dispose (object);
}

static void
chime_websocket_connection_finalize (GObject *object)
{
	ChimeWebsocketConnection *self = CHIME_WEBSOCKET_CONNECTION (object);
	ChimeWebsocketConnectionPrivate *pv = self->pv;

	g_free (pv->peer_close_data);

	g_main_context_unref (pv->main_context);

	if (pv->incoming)
		g_byte_array_free (pv->incoming, TRUE);
	while (!g_queue_is_empty (&pv->outgoing))
		frame_free (g_queue_pop_head (&pv->outgoing));

	g_clear_object (&pv->io_stream);
	g_assert (!pv->input_source);
	g_assert (!pv->output_source);
	g_assert (pv->io_closing);
	g_assert (pv->io_closed);
	g_assert (!pv->close_timeout);
	g_assert (!pv->keepalive_timeout);

	if (pv->message_data)
		g_byte_array_free (pv->message_data, TRUE);

	if (pv->uri)
		soup_uri_free (pv->uri);
	g_free (pv->origin);
	g_free (pv->protocol);

	G_OBJECT_CLASS (chime_websocket_connection_parent_class)->finalize (object);
}

static void
chime_websocket_connection_class_init (ChimeWebsocketConnectionClass *klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

	gobject_class->constructed = chime_websocket_connection_constructed;
	gobject_class->get_property = chime_websocket_connection_get_property;
	gobject_class->set_property = chime_websocket_connection_set_property;
	gobject_class->dispose = chime_websocket_connection_dispose;
	gobject_class->finalize = chime_websocket_connection_finalize;

	/**
	 * ChimeWebsocketConnection:io-stream:
	 *
	 * The underlying IO stream the WebSocket is communicating
	 * over.
	 *
	 * The input and output streams must be pollable streams.
	 *
	 * Since: 2.50
	 */
	g_object_class_install_property (gobject_class, PROP_IO_STREAM,
					 g_param_spec_object ("io-stream",
							      "I/O Stream",
							      "Underlying I/O stream",
							      G_TYPE_IO_STREAM,
							      G_PARAM_READWRITE |
							      G_PARAM_CONSTRUCT_ONLY |
							      G_PARAM_STATIC_STRINGS));

	/**
	 * ChimeWebsocketConnection:connection-type:
	 *
	 * The type of connection (client/server).
	 *
	 * Since: 2.50
	 */
	g_object_class_install_property (gobject_class, PROP_CONNECTION_TYPE,
					 g_param_spec_enum ("connection-type",
							    "Connection type",
							    "Connection type (client/server)",
							    SOUP_TYPE_WEBSOCKET_CONNECTION_TYPE,
							    SOUP_WEBSOCKET_CONNECTION_UNKNOWN,
							    G_PARAM_READWRITE |
							    G_PARAM_CONSTRUCT_ONLY |
							    G_PARAM_STATIC_STRINGS));

	/**
	 * ChimeWebsocketConnection:uri:
	 *
	 * The URI of the WebSocket.
	 *
	 * For servers this represents the address of the WebSocket,
	 * and for clients it is the address connected to.
	 *
	 * Since: 2.50
	 */
	g_object_class_install_property (gobject_class, PROP_URI,
					 g_param_spec_boxed ("uri",
							     "URI",
							     "The WebSocket URI",
							     SOUP_TYPE_URI,
							     G_PARAM_READWRITE |
							     G_PARAM_CONSTRUCT_ONLY |
							     G_PARAM_STATIC_STRINGS));

	/**
	 * ChimeWebsocketConnection:origin:
	 *
	 * The client's Origin.
	 *
	 * Since: 2.50
	 */
	g_object_class_install_property (gobject_class, PROP_ORIGIN,
					 g_param_spec_string ("origin",
							      "Origin",
							      "The WebSocket origin",
							      NULL,
							      G_PARAM_READWRITE |
							      G_PARAM_CONSTRUCT_ONLY |
							      G_PARAM_STATIC_STRINGS));

	/**
	 * ChimeWebsocketConnection:protocol:
	 *
	 * The chosen protocol, or %NULL if a protocol was not agreed
	 * upon.
	 *
	 * Since: 2.50
	 */
	g_object_class_install_property (gobject_class, PROP_PROTOCOL,
					 g_param_spec_string ("protocol",
							      "Protocol",
							      "The chosen WebSocket protocol",
							      NULL,
							      G_PARAM_READWRITE |
							      G_PARAM_CONSTRUCT_ONLY |
							      G_PARAM_STATIC_STRINGS));

	/**
	 * ChimeWebsocketConnection:state:
	 *
	 * The current state of the WebSocket.
	 *
	 * Since: 2.50
	 */
	g_object_class_install_property (gobject_class, PROP_STATE,
					 g_param_spec_enum ("state",
							    "State",
							    "State ",
							    SOUP_TYPE_WEBSOCKET_STATE,
							    SOUP_WEBSOCKET_STATE_OPEN,
							    G_PARAM_READABLE |
							    G_PARAM_STATIC_STRINGS));

	/**
	 * ChimeWebsocketConnection:max-incoming-payload-size:
	 *
	 * The maximum payload size for incoming packets the protocol expects
	 * or 0 to not limit it.
	 *
	 * Since: 2.56
	 */
	g_object_class_install_property (gobject_class, PROP_MAX_INCOMING_PAYLOAD_SIZE,
					 g_param_spec_uint64 ("max-incoming-payload-size",
							      "Max incoming payload size",
							      "Max incoming payload size ",
							      0,
							      G_MAXUINT64,
							      MAX_INCOMING_PAYLOAD_SIZE_DEFAULT,
							      G_PARAM_READWRITE |
							      G_PARAM_CONSTRUCT |
							      G_PARAM_STATIC_STRINGS));

	/**
	 * ChimeWebsocketConnection:keepalive-interval:
	 *
	 * Interval in seconds on when to send a ping message which will
	 * serve as a keepalive message. If set to 0 the keepalive message is
	 * disabled.
	 *
	 * Since: 2.58
	 */
	g_object_class_install_property (gobject_class, PROP_KEEPALIVE_INTERVAL,
					 g_param_spec_uint ("keepalive-interval",
					                    "Keepalive interval",
					                    "Keepalive interval",
					                    0,
					                    G_MAXUINT,
					                    0,
					                    G_PARAM_READWRITE |
					                    G_PARAM_CONSTRUCT |
					                    G_PARAM_STATIC_STRINGS));

	/**
	 * ChimeWebsocketConnection::message:
	 * @self: the WebSocket
	 * @type: the type of message contents
	 * @message: the message data
	 *
	 * Emitted when we receive a message from the peer.
	 *
	 * As a convenience, the @message data will always be
	 * NUL-terminated, but the NUL byte will not be included in
	 * the length count.
	 *
	 * Since: 2.50
	 */
	signals[MESSAGE] = g_signal_new ("message",
					 CHIME_TYPE_WEBSOCKET_CONNECTION,
					 G_SIGNAL_RUN_FIRST,
					 G_STRUCT_OFFSET (ChimeWebsocketConnectionClass, message),
					 NULL, NULL, g_cclosure_marshal_generic,
					 G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_BYTES);

	/**
	 * ChimeWebsocketConnection::error:
	 * @self: the WebSocket
	 * @error: the error that occured
	 *
	 * Emitted when an error occurred on the WebSocket. This may
	 * be fired multiple times. Fatal errors will be followed by
	 * the #ChimeWebsocketConnection::closed signal being emitted.
	 *
	 * Since: 2.50
	 */
	signals[ERROR] = g_signal_new ("error",
				       CHIME_TYPE_WEBSOCKET_CONNECTION,
				       G_SIGNAL_RUN_FIRST,
				       G_STRUCT_OFFSET (ChimeWebsocketConnectionClass, error),
				       NULL, NULL, g_cclosure_marshal_generic,
				       G_TYPE_NONE, 1, G_TYPE_ERROR);

	/**
	 * ChimeWebsocketConnection::closing:
	 * @self: the WebSocket
	 *
	 * This signal will be emitted during an orderly close.
	 *
	 * Since: 2.50
	 */
	signals[CLOSING] = g_signal_new ("closing",
					 CHIME_TYPE_WEBSOCKET_CONNECTION,
					 G_SIGNAL_RUN_LAST,
					 G_STRUCT_OFFSET (ChimeWebsocketConnectionClass, closing),
					 NULL, NULL, g_cclosure_marshal_generic,
					 G_TYPE_NONE, 0);

	/**
	 * ChimeWebsocketConnection::closed:
	 * @self: the WebSocket
	 *
	 * Emitted when the connection has completely closed, either
	 * due to an orderly close from the peer, one initiated via
	 * chime_websocket_connection_close() or a fatal error
	 * condition that caused a close.
	 *
	 * This signal will be emitted once.
	 *
	 * Since: 2.50
	 */
	signals[CLOSED] = g_signal_new ("closed",
					CHIME_TYPE_WEBSOCKET_CONNECTION,
					G_SIGNAL_RUN_FIRST,
					G_STRUCT_OFFSET (ChimeWebsocketConnectionClass, closed),
					NULL, NULL, g_cclosure_marshal_generic,
					G_TYPE_NONE, 0);

	/**
	 * ChimeWebsocketConnection::pong:
	 * @self: the WebSocket
	 * @message: the application data (if any)
	 *
	 * Emitted when we receive a Pong frame (solicited or
	 * unsolicited) from the peer.
	 *
	 * As a convenience, the @message data will always be
	 * NUL-terminated, but the NUL byte will not be included in
	 * the length count.
	 *
	 * Since: 2.60
	 */
	signals[PONG] = g_signal_new ("pong",
				      CHIME_TYPE_WEBSOCKET_CONNECTION,
				      G_SIGNAL_RUN_FIRST,
				      0,
				      NULL, NULL, g_cclosure_marshal_generic,
				      G_TYPE_NONE, 1, G_TYPE_BYTES);
}

/**
 * chime_websocket_connection_new:
 * @stream: a #GIOStream connected to the WebSocket server
 * @uri: the URI of the connection
 * @type: the type of connection (client/side)
 * @origin: (allow-none): the Origin of the client
 * @protocol: (allow-none): the subprotocol in use
 *
 * Creates a #ChimeWebsocketConnection on @stream. This should be
 * called after completing the handshake to begin using the WebSocket
 * protocol.
 *
 * Returns: a new #ChimeWebsocketConnection
 *
 * Since: 2.50
 */
ChimeWebsocketConnection *
chime_websocket_connection_new (GIOStream                    *stream,
			       SoupURI                      *uri,
			       SoupWebsocketConnectionType   type,
			       const char                   *origin,
			       const char                   *protocol)
{
	g_return_val_if_fail (G_IS_IO_STREAM (stream), NULL);
	g_return_val_if_fail (uri != NULL, NULL);
	g_return_val_if_fail (type != SOUP_WEBSOCKET_CONNECTION_UNKNOWN, NULL);

	return g_object_new (CHIME_TYPE_WEBSOCKET_CONNECTION,
			     "io-stream", stream,
			     "uri", uri,
			     "connection-type", type,
			     "origin", origin,
			     "protocol", protocol,
			     NULL);
}

/**
 * chime_websocket_connection_get_io_stream:
 * @self: the WebSocket
 *
 * Get the I/O stream the WebSocket is communicating over.
 *
 * Returns: (transfer none): the WebSocket's I/O stream.
 *
 * Since: 2.50
 */
GIOStream *
chime_websocket_connection_get_io_stream (ChimeWebsocketConnection *self)
{
	g_return_val_if_fail (CHIME_IS_WEBSOCKET_CONNECTION (self), NULL);

	return self->pv->io_stream;
}

/**
 * chime_websocket_connection_get_connection_type:
 * @self: the WebSocket
 *
 * Get the connection type (client/server) of the connection.
 *
 * Returns: the connection type
 *
 * Since: 2.50
 */
SoupWebsocketConnectionType
chime_websocket_connection_get_connection_type (ChimeWebsocketConnection *self)
{
	g_return_val_if_fail (CHIME_IS_WEBSOCKET_CONNECTION (self), SOUP_WEBSOCKET_CONNECTION_UNKNOWN);

	return self->pv->connection_type;
}

/**
 * chime_websocket_connection_get_uri:
 * @self: the WebSocket
 *
 * Get the URI of the WebSocket.
 *
 * For servers this represents the address of the WebSocket, and
 * for clients it is the address connected to.
 *
 * Returns: (transfer none): the URI
 *
 * Since: 2.50
 */
SoupURI *
chime_websocket_connection_get_uri (ChimeWebsocketConnection *self)
{
	g_return_val_if_fail (CHIME_IS_WEBSOCKET_CONNECTION (self), NULL);

	return self->pv->uri;
}

/**
 * chime_websocket_connection_get_origin:
 * @self: the WebSocket
 *
 * Get the origin of the WebSocket.
 *
 * Returns: (nullable): the origin, or %NULL
 *
 * Since: 2.50
 */
const char *
chime_websocket_connection_get_origin (ChimeWebsocketConnection *self)
{
	g_return_val_if_fail (CHIME_IS_WEBSOCKET_CONNECTION (self), NULL);

	return self->pv->origin;
}

/**
 * chime_websocket_connection_get_protocol:
 * @self: the WebSocket
 *
 * Get the protocol chosen via negotiation with the peer.
 *
 * Returns: (nullable): the chosen protocol, or %NULL
 *
 * Since: 2.50
 */
const char *
chime_websocket_connection_get_protocol (ChimeWebsocketConnection *self)
{
	g_return_val_if_fail (CHIME_IS_WEBSOCKET_CONNECTION (self), NULL);

	return self->pv->protocol;
}

/**
 * chime_websocket_connection_get_state:
 * @self: the WebSocket
 *
 * Get the current state of the WebSocket.
 *
 * Returns: the state
 *
 * Since: 2.50
 */
SoupWebsocketState
chime_websocket_connection_get_state (ChimeWebsocketConnection *self)
{
	g_return_val_if_fail (CHIME_IS_WEBSOCKET_CONNECTION (self), 0);

	if (self->pv->io_closed)
		return SOUP_WEBSOCKET_STATE_CLOSED;
	else if (self->pv->io_closing || self->pv->close_sent)
		return SOUP_WEBSOCKET_STATE_CLOSING;
	else
		return SOUP_WEBSOCKET_STATE_OPEN;
}

/**
 * chime_websocket_connection_get_close_code:
 * @self: the WebSocket
 *
 * Get the close code received from the WebSocket peer.
 *
 * This only becomes valid once the WebSocket is in the
 * %CHIME_WEBSOCKET_STATE_CLOSED state. The value will often be in the
 * #ChimeWebsocketCloseCode enumeration, but may also be an application
 * defined close code.
 *
 * Returns: the close code or zero.
 *
 * Since: 2.50
 */
gushort
chime_websocket_connection_get_close_code (ChimeWebsocketConnection *self)
{
	g_return_val_if_fail (CHIME_IS_WEBSOCKET_CONNECTION (self), 0);

	return self->pv->peer_close_code;
}

/**
 * chime_websocket_connection_get_close_data:
 * @self: the WebSocket
 *
 * Get the close data received from the WebSocket peer.
 *
 * This only becomes valid once the WebSocket is in the
 * %CHIME_WEBSOCKET_STATE_CLOSED state. The data may be freed once
 * the main loop is run, so copy it if you need to keep it around.
 *
 * Returns: the close data or %NULL
 *
 * Since: 2.50
 */
const char *
chime_websocket_connection_get_close_data (ChimeWebsocketConnection *self)
{
	g_return_val_if_fail (CHIME_IS_WEBSOCKET_CONNECTION (self), NULL);

	return self->pv->peer_close_data;
}

/**
 * chime_websocket_connection_send_text:
 * @self: the WebSocket
 * @text: the message contents
 *
 * Send a text (UTF-8) message to the peer.
 *
 * The message is queued to be sent and will be sent when the main loop
 * is run.
 *
 * Since: 2.50
 */
void
chime_websocket_connection_send_text (ChimeWebsocketConnection *self,
				     const char *text)
{
	gsize length;

	g_return_if_fail (CHIME_IS_WEBSOCKET_CONNECTION (self));
	g_return_if_fail (chime_websocket_connection_get_state (self) == SOUP_WEBSOCKET_STATE_OPEN);
	g_return_if_fail (text != NULL);

	length = strlen (text);
	g_return_if_fail (g_utf8_validate (text, length, NULL));

	send_message (self, CHIME_WEBSOCKET_QUEUE_NORMAL, 0x01, (const guint8 *) text, length);
}

/**
 * chime_websocket_connection_send_binary:
 * @self: the WebSocket
 * @data: (array length=length) (element-type guint8): the message contents
 * @length: the length of @data
 *
 * Send a binary message to the peer.
 *
 * The message is queued to be sent and will be sent when the main loop
 * is run.
 *
 * Since: 2.50
 */
void
chime_websocket_connection_send_binary (ChimeWebsocketConnection *self,
				       gconstpointer data,
				       gsize length)
{
	g_return_if_fail (CHIME_IS_WEBSOCKET_CONNECTION (self));
	g_return_if_fail (chime_websocket_connection_get_state (self) == SOUP_WEBSOCKET_STATE_OPEN);
	g_return_if_fail (data != NULL);

	send_message (self, CHIME_WEBSOCKET_QUEUE_NORMAL, 0x02, data, length);
}

/**
 * chime_websocket_connection_close:
 * @self: the WebSocket
 * @code: close code
 * @data: (allow-none): close data
 *
 * Close the connection in an orderly fashion.
 *
 * Note that until the #ChimeWebsocketConnection::closed signal fires, the connection
 * is not yet completely closed. The close message is not even sent until the
 * main loop runs.
 *
 * The @code and @data are sent to the peer along with the close request.
 * Note that the @data must be UTF-8 valid.
 *
 * Since: 2.50
 */
void
chime_websocket_connection_close (ChimeWebsocketConnection *self,
				 gushort code,
				 const char *data)
{
	ChimeWebsocketConnectionPrivate *pv;

	g_return_if_fail (CHIME_IS_WEBSOCKET_CONNECTION (self));
	pv = self->pv;
	g_return_if_fail (!pv->close_sent);

	g_return_if_fail (code != SOUP_WEBSOCKET_CLOSE_NO_STATUS &&
			  code != SOUP_WEBSOCKET_CLOSE_ABNORMAL &&
			  code != SOUP_WEBSOCKET_CLOSE_TLS_HANDSHAKE);
	if (pv->connection_type == SOUP_WEBSOCKET_CONNECTION_SERVER)
		g_return_if_fail (code != SOUP_WEBSOCKET_CLOSE_NO_EXTENSION);
	else
		g_return_if_fail (code != SOUP_WEBSOCKET_CLOSE_SERVER_ERROR);

	close_connection (self, code, data);
}

/**
 * chime_websocket_connection_get_max_incoming_payload_size:
 * @self: the WebSocket
 *
 * Gets the maximum payload size allowed for incoming packets.
 *
 * Returns: the maximum payload size.
 *
 * Since: 2.56
 */
guint64
chime_websocket_connection_get_max_incoming_payload_size (ChimeWebsocketConnection *self)
{
	ChimeWebsocketConnectionPrivate *pv;

	g_return_val_if_fail (CHIME_IS_WEBSOCKET_CONNECTION (self), MAX_INCOMING_PAYLOAD_SIZE_DEFAULT);
	pv = self->pv;

	return pv->max_incoming_payload_size;
}

/**
 * chime_websocket_connection_set_max_incoming_payload_size:
 * @self: the WebSocket
 * @max_incoming_payload_size: the maximum payload size
 *
 * Sets the maximum payload size allowed for incoming packets. It
 * does not limit the outgoing packet size.
 *
 * Since: 2.56
 */
void
chime_websocket_connection_set_max_incoming_payload_size (ChimeWebsocketConnection *self,
                                                         guint64                  max_incoming_payload_size)
{
	ChimeWebsocketConnectionPrivate *pv;

	g_return_if_fail (CHIME_IS_WEBSOCKET_CONNECTION (self));
	pv = self->pv;

	if (pv->max_incoming_payload_size != max_incoming_payload_size) {
		pv->max_incoming_payload_size = max_incoming_payload_size;
		g_object_notify (G_OBJECT (self), "max-incoming-payload-size");
	}
}

/**
 * chime_websocket_connection_get_keepalive_interval:
 * @self: the WebSocket
 *
 * Gets the keepalive interval in seconds or 0 if disabled.
 *
 * Returns: the keepalive interval.
 *
 * Since: 2.58
 */
guint
chime_websocket_connection_get_keepalive_interval (ChimeWebsocketConnection *self)
{
	ChimeWebsocketConnectionPrivate *pv;

	g_return_val_if_fail (CHIME_IS_WEBSOCKET_CONNECTION (self), 0);
	pv = self->pv;

	return pv->keepalive_interval;
}

static gboolean
on_queue_ping (gpointer user_data)
{
	ChimeWebsocketConnection *self = CHIME_WEBSOCKET_CONNECTION (user_data);
	gchar *payload;
	GTimeVal now;

	g_debug ("sending ping message");

	g_get_current_time (&now);
	payload = g_time_val_to_iso8601 (&now);
	send_message (self, CHIME_WEBSOCKET_QUEUE_NORMAL, 0x09, (guint8 *) payload, strlen(payload));
	g_free (payload);

	return G_SOURCE_CONTINUE;
}

/**
 * chime_websocket_connection_set_keepalive_interval:
 * @self: the WebSocket
 * @interval: the interval to send a ping message or 0 to disable it
 *
 * Sets the interval in seconds on when to send a ping message which will serve
 * as a keepalive message. If set to 0 the keepalive message is disabled.
 *
 * Since: 2.58
 */
void
chime_websocket_connection_set_keepalive_interval (ChimeWebsocketConnection *self,
                                                  guint                    interval)
{
	ChimeWebsocketConnectionPrivate *pv;

	g_return_if_fail (CHIME_IS_WEBSOCKET_CONNECTION (self));
	pv = self->pv;

	if (pv->keepalive_interval != interval) {
		pv->keepalive_interval = interval;
		g_object_notify (G_OBJECT (self), "keepalive-interval");

		keepalive_stop_timeout (self);

		if (interval > 0) {
			pv->keepalive_timeout = g_timeout_source_new_seconds (interval);
			g_source_set_callback (pv->keepalive_timeout, on_queue_ping, self, NULL);
			g_source_attach (pv->keepalive_timeout, pv->main_context);
		}
	}
}
#endif
