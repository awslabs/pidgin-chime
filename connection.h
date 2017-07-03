/*
 * Pidgin/libpurple Chime client plugin
 *
 * Copyright © 2017 Amazon.com, Inc. or its affiliates.
 *
 * Author: David Woodhouse <dwmw2@infradead.org>
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

#ifndef __CHIME_CONNECTION_H__
#define __CHIME_CONNECTION_H__

#include <glib-object.h>
#include <prpl.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define CHIME_TYPE_CONNECTION (chime_connection_get_type ())
G_DECLARE_FINAL_TYPE (ChimeConnection, chime_connection, CHIME, CONNECTION, GObject)

struct _ChimeConnection {
	GObject parent_instance;

	PurpleConnection *prpl_conn;

	/* Rooms */
	gint64 rooms_generation;
	GHashTable *rooms_by_id;
	GHashTable *rooms_by_name;
	GHashTable *live_chats;
	int chat_id;
	GRegex *mention_regex;

	/* Conversations */
	GHashTable *im_conversations_by_peer_id;
	GHashTable *conversations_by_id;
	GHashTable *conversations_by_name;

	/* Service config */
	JsonNode *reg_node;
	const gchar *session_id;
	const gchar *profile_id;
	const gchar *profile_channel;
	const gchar *presence_channel;

	const gchar *device_id;
	const gchar *device_channel;

	const gchar *presence_url;
	const gchar *websocket_url;
	const gchar *reachability_url;
	const gchar *profile_url;
	const gchar *contacts_url;
	const gchar *messaging_url;
	const gchar *conference_url;
};

typedef enum
{
	CHIME_CONNECTION_ERROR_NETWORK
} ChimeConnectionErrorEnum;

typedef void (*ChimeSoupMessageCallback)(ChimeConnection *cxn,
					 SoupMessage *msg,
					 JsonNode *node,
					 gpointer cb_data);

#define CHIME_CONNECTION_ERROR (chime_connection_error_quark())
GQuark chime_connection_error_quark (void);

ChimeConnection *chime_connection_new                        (PurpleConnection *connection,
							      const gchar *server,
							      const gchar *device_token,
							      const gchar *session_token);

void             chime_connection_set_presence_async         (ChimeConnection    *self,
                                                              const gchar        *availability,
                                                              const gchar        *visibility,
                                                              GCancellable       *cancellable,
                                                              GAsyncReadyCallback callback,
                                                              gpointer            user_data);

gboolean         chime_connection_set_presence_finish        (ChimeConnection  *self,
                                                              GAsyncResult     *result,
                                                              GError          **error);

void             chime_connection_set_device_status_async    (ChimeConnection    *self,
                                                              const gchar        *status,
                                                              GCancellable       *cancellable,
                                                              GAsyncReadyCallback callback,
                                                              gpointer            user_data);

gboolean         chime_connection_set_device_status_finish   (ChimeConnection  *self,
                                                              GAsyncResult     *result,
                                                              GError          **error);

const gchar     *chime_connection_get_session_token          (ChimeConnection  *self);

/* Internal functions */
SoupMessage     *chime_connection_queue_http_request         (ChimeConnection         *self,
                                                              JsonNode                *node,
                                                              SoupURI                 *uri,
                                                              const gchar             *method,
                                                              ChimeSoupMessageCallback callback,
                                                              gpointer                 cb_data);


void chime_connection_connect(ChimeConnection *cxn);
void chime_connection_disconnect(ChimeConnection *cxn);

G_END_DECLS

#endif /* __CHIME_CONNECTION_H__ */
