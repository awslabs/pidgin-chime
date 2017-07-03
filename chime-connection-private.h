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

#ifndef __CHIME_CONNECTION_PRIVATE_H__
#define __CHIME_CONNECTION_PRIVATE_H__

#include "connection.h"

typedef enum {
	CHIME_STATE_CONNECTING,
	CHIME_STATE_CONNECTED,
	CHIME_STATE_DISCONNECTED
} ChimeConnectionState;

typedef struct {
	ChimeConnectionState state;

	gchar *server;
	gchar *device_token;
	gchar *session_token;

	SoupSession *soup_sess;

	/* Messages queued for resubmission */
	GQueue *msg_queue;

	/* Juggernaut */
	SoupWebsocketConnection *ws_conn;
	gboolean jugg_connected;	/* For reconnecting, to abort on failed reconnect */
	gboolean jugg_resubscribe;	/* After reconnect we should use 'resubscribe' */
	gulong message_handler, closed_handler;
	gchar *ws_key;
	GHashTable *subscriptions;

	/* Contacts */
	GHashTable *contacts_by_id;
	GHashTable *contacts_by_email;
	GSList *contacts_needed;

	/* Rooms */
	gint64 rooms_generation;
	GHashTable *rooms_by_id;
	GHashTable *rooms_by_name;
	GHashTable *live_chats;
	int chat_id;
	GRegex *mention_regex;


} ChimeConnectionPrivate;

#define CHIME_CONNECTION_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), CHIME_TYPE_CONNECTION, ChimeConnectionPrivate))

void chime_connection_fail(ChimeConnection *cxn, gint code, const gchar *format, ...);
void chime_connection_fail_error(ChimeConnection *cxn, GError *error);

#endif /* __CHIME_CONNECTION_PRIVATE_H__ */
