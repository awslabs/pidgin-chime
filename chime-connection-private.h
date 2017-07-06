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

#include "chime-connection.h"
#include "chime-contact.h"

typedef enum {
	CHIME_STATE_CONNECTING,
	CHIME_STATE_CONNECTED,
	CHIME_STATE_DISCONNECTED
} ChimeConnectionState;

typedef enum {
	CHIME_SYNC_IDLE,
	CHIME_SYNC_STALE,
	CHIME_SYNC_FETCHING,
} ChimeSyncState;

typedef struct {
	ChimeConnectionState state;

	gchar *server;
	gchar *device_token;
	gchar *session_token;

	gboolean jugg_online, contacts_online, rooms_online, convs_online;

	/* Service config */
	JsonNode *reg_node;
	const gchar *display_name;

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

	SoupSession *soup_sess;

	/* Messages queued for resubmission */
	GQueue *msgs_queued;
	GQueue *msgs_pending_auth;

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
	gint64 contacts_generation;	/* Pureky internal revision number, indicating the
					 * generation last fetched or being fetched now. */
	ChimeSyncState contacts_sync;

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

} ChimeConnectionPrivate;

#define CHIME_CONNECTION_GET_PRIVATE(o) \
	(G_TYPE_INSTANCE_GET_PRIVATE ((o), CHIME_TYPE_CONNECTION, \
				      ChimeConnectionPrivate))

void chime_connection_fail(ChimeConnection *cxn, gint code,
			   const gchar *format, ...);
void chime_connection_fail_error(ChimeConnection *cxn, GError *error);
void chime_connection_new_contact(ChimeConnection *cxn, ChimeContact *contact);

/* chime-contact.c */
void chime_init_contacts(ChimeConnection *cxn);
void chime_destroy_contacts(ChimeConnection *cxn);
ChimeContact *chime_connection_parse_contact(ChimeConnection *cxn,
					     JsonNode *node, GError **error);
ChimeContact *chime_connection_parse_conversation_contact(ChimeConnection *cxn,
							  JsonNode *node,
							  GError **error);
void chime_connection_calculate_online(ChimeConnection *cxn);

#endif /* __CHIME_CONNECTION_PRIVATE_H__ */
