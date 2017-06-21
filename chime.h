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

#ifndef __CHIME_H__
#define __CHIME_H__

#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

#define CHIME_DEVICE_CAP_PUSH_DELIVERY_RECEIPTS		(1<<1)
#define CHIME_DEVICE_CAP_PRESENCE_PUSH			(1<<2)
#define CHIME_DEVICE_CAP_WEBINAR			(1<<3)
#define CHIME_DEVICE_CAP_PRESENCE_SUBSCRIPTION		(1<<4)

struct chime_connection;

typedef void (*ChimeSoupMessageCallback)(struct chime_connection *cxn,
					 SoupMessage *msg,
					 JsonNode *node,
					 gpointer cb_data);

struct chime_msg {
	struct chime_connection *cxn;
	ChimeSoupMessageCallback cb;
	gpointer cb_data;
	SoupMessage *msg;
	gboolean auto_renew;
};

struct chime_connection {
	PurpleConnection *prpl_conn;

	SoupSession *soup_sess;
	gchar *session_token;

	/* Messages queued for resubmission */
	GList *msg_queue;

	/* Juggernaut */
	SoupWebsocketConnection *ws_conn;
	gboolean jugg_connected;
	gchar *ws_key;
	GHashTable *subscriptions;

	/* Buddies */
	GHashTable *buddies;

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


extern GQuark pidgin_chime_error_quark(void);
#define CHIME_ERROR pidgin_chime_error_quark()

enum {
	CHIME_ERROR_REQUEST_FAILED = 1,
	CHIME_ERROR_BAD_RESPONSE,
};

#define CONNECT_STEPS 3

/* chime.c */
gboolean parse_string(JsonNode *parent, const gchar *name, const gchar **res);
SoupURI *soup_uri_new_printf(const gchar *base, const gchar *format, ...);
SoupMessage *chime_queue_http_request(struct chime_connection *cxn, JsonNode *node,
				      SoupURI *uri, ChimeSoupMessageCallback callback,
				      gpointer cb_data, gboolean auto_renew);

/* jugg.c */
void chime_init_juggernaut(struct chime_connection *cxn);
void chime_destroy_juggernaut(struct chime_connection *cxn);

typedef void (*JuggernautCallback)(gpointer cb_data, JsonNode *node);
void chime_jugg_subscribe(struct chime_connection *cxn, const gchar *channel, JuggernautCallback cb, gpointer cb_data);
void chime_jugg_unsubscribe(struct chime_connection *cxn, const gchar *channel, JuggernautCallback cb, gpointer cb_data);
void jugg_dump_incoming(gpointer cb_data, JsonNode *node);
void chime_purple_keepalive(PurpleConnection *conn);

/* buddy.c */
void fetch_buddies(struct chime_connection *cxn);
void chime_purple_buddy_free(PurpleBuddy *buddy);
void chime_purple_add_buddy(PurpleConnection *conn, PurpleBuddy *buddy, PurpleGroup *group);
void chime_purple_remove_buddy(PurpleConnection *conn, PurpleBuddy *buddy, PurpleGroup *group);
void chime_init_buddies(struct chime_connection *cxn);
void chime_destroy_buddies(struct chime_connection *cxn);

#endif /* __CHIME_H__ */
