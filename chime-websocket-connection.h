/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * soup-websocket-connection.h: This file was originally part of Cockpit.
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

#ifndef __CHIME_WEBSOCKET_CONNECTION_H__
#define __CHIME_WEBSOCKET_CONNECTION_H__

#include <libsoup/soup-types.h>
#include <libsoup/soup-websocket.h>

G_BEGIN_DECLS

#define CHIME_TYPE_WEBSOCKET_CONNECTION         (chime_websocket_connection_get_type ())
#define CHIME_WEBSOCKET_CONNECTION(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), CHIME_TYPE_WEBSOCKET_CONNECTION, ChimeWebsocketConnection))
#define CHIME_IS_WEBSOCKET_CONNECTION(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), CHIME_TYPE_WEBSOCKET_CONNECTION))
#define CHIME_WEBSOCKET_CONNECTION_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST ((k), CHIME_TYPE_WEBSOCKET_CONNECTION, ChimeWebsocketConnectionClass))
#define CHIME_WEBSOCKET_CONNECTION_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), CHIME_TYPE_WEBSOCKET_CONNECTION, ChimeWebsocketConnectionClass))
#define CHIME_IS_WEBSOCKET_CONNECTION_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), CHIME_TYPE_WEBSOCKET_CONNECTION))

typedef struct _ChimeWebsocketConnectionPrivate  ChimeWebsocketConnectionPrivate;

struct _ChimeWebsocketConnection {
	GObject parent;

	/*< private >*/
	ChimeWebsocketConnectionPrivate *pv;
};

typedef struct _ChimeWebsocketConnection ChimeWebsocketConnection;

typedef struct {
	GObjectClass parent;

	/* signals */
	void      (* message)     (ChimeWebsocketConnection *self,
				   SoupWebsocketDataType type,
				   GBytes *message);

	void      (* error)       (ChimeWebsocketConnection *self,
				   GError *error);

	void      (* closing)     (ChimeWebsocketConnection *self);

	void      (* closed)      (ChimeWebsocketConnection *self);
} ChimeWebsocketConnectionClass;

GType chime_websocket_connection_get_type (void) G_GNUC_CONST;

ChimeWebsocketConnection *chime_websocket_connection_new (GIOStream                    *stream,
							SoupURI                      *uri,
							SoupWebsocketConnectionType   type,
							const char                   *origin,
							const char                   *protocol);

GIOStream *         chime_websocket_connection_get_io_stream  (ChimeWebsocketConnection *self);

SoupWebsocketConnectionType chime_websocket_connection_get_connection_type (ChimeWebsocketConnection *self);

SoupURI *           chime_websocket_connection_get_uri        (ChimeWebsocketConnection *self);

const char *        chime_websocket_connection_get_origin     (ChimeWebsocketConnection *self);

const char *        chime_websocket_connection_get_protocol   (ChimeWebsocketConnection *self);

SoupWebsocketState  chime_websocket_connection_get_state      (ChimeWebsocketConnection *self);

gushort             chime_websocket_connection_get_close_code (ChimeWebsocketConnection *self);

const char *        chime_websocket_connection_get_close_data (ChimeWebsocketConnection *self);

void                chime_websocket_connection_send_text      (ChimeWebsocketConnection *self,
							      const char *text);
void                chime_websocket_connection_send_binary    (ChimeWebsocketConnection *self,
							      gconstpointer data,
							      gsize length);

void                chime_websocket_connection_close          (ChimeWebsocketConnection *self,
							      gushort code,
							      const char *data);

guint64             chime_websocket_connection_get_max_incoming_payload_size (ChimeWebsocketConnection *self);

void                chime_websocket_connection_set_max_incoming_payload_size (ChimeWebsocketConnection *self,
                                                                             guint64                  max_incoming_payload_size);

guint               chime_websocket_connection_get_keepalive_interval (ChimeWebsocketConnection *self);

void                chime_websocket_connection_set_keepalive_interval (ChimeWebsocketConnection *self,
                                                                      guint                    interval);


/* These are from soup-session.c, modified in chime-websocket.c */
void
chime_session_websocket_connect_async (SoupSession          *session,
				       SoupMessage          *msg,
				       const char           *origin,
				       char                **protocols,
				       GCancellable         *cancellable,
				       GAsyncReadyCallback   callback,
				       gpointer              user_data);

ChimeWebsocketConnection *
chime_session_websocket_connect_finish (SoupSession      *session,
					GAsyncResult     *result,
					GError          **error);

G_END_DECLS

#endif /* __CHIME_WEBSOCKET_CONNECTION_H__ */
