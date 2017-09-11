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
#include "chime-object.h"
#include "chime-contact.h"
#include "chime-room.h"
#include "chime-conversation.h"
#include "chime-meeting.h"
#include "chime-call.h"

#include <libsoup/soup.h>

#ifndef USE_LIBSOUP_WEBSOCKETS
#include "chime-websocket-connection.h"

#define soup_websocket_connection_new chime_websocket_connection_new
#define soup_websocket_connection_set_max_incoming_payload_size chime_websocket_connection_set_max_incoming_payload_size
#define soup_websocket_connection_set_keepalive_interval chime_websocket_connection_set_keepalive_interval
#define soup_websocket_connection_get_close_code chime_websocket_connection_get_close_code
#define soup_websocket_connection_get_close_data chime_websocket_connection_get_close_data
#define soup_websocket_connection_get_state chime_websocket_connection_get_state
#define soup_websocket_connection_send_text chime_websocket_connection_send_text
#define soup_websocket_connection_send_binary chime_websocket_connection_send_binary
#define soup_websocket_connection_close chime_websocket_connection_close
#define soup_session_websocket_connect_async chime_session_websocket_connect_async
#define soup_session_websocket_connect_finish chime_session_websocket_connect_finish
#define SoupWebsocketConnection ChimeWebsocketConnection
#endif

#define CHIME_ENUM_VALUE(val, nick) { val, #val, nick },
#define CHIME_DEFINE_ENUM_TYPE(TypeName, type_name, values)		\
	GType type_name ## _get_type(void) {				\
		static volatile gsize chime_define_id__volatile = 0;	\
		if (g_once_init_enter(&chime_define_id__volatile)) {	\
			static const GEnumValue v[] = {			\
				values					\
				{ 0, NULL, NULL },			\
			};						\
			GType chime_define_id = g_enum_register_static(g_intern_static_string(#TypeName), v); \
			g_once_init_leave(&chime_define_id__volatile, chime_define_id); \
		}							\
		return chime_define_id__volatile;			\
	}

typedef enum {
	CHIME_NOTIFY_PREF_ALWAYS,
	CHIME_NOTIFY_PREF_DIRECT_ONLY,
	CHIME_NOTIFY_PREF_NEVER
} ChimeNotifyPref;

#define CHIME_TYPE_NOTIFY_PREF (chime_notify_pref_get_type ())
GType chime_notify_pref_get_type (void) G_GNUC_CONST;

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

#define CHIME_DEVICE_CAP_PUSH_DELIVERY_RECEIPTS		(1<<1)
#define CHIME_DEVICE_CAP_PRESENCE_PUSH			(1<<2)
#define CHIME_DEVICE_CAP_WEBINAR			(1<<3)
#define CHIME_DEVICE_CAP_PRESENCE_SUBSCRIPTION		(1<<4)

/* SoupMessage handling for Chime communication, with retry on re-auth
 * and JSON parsing. XX: MAke this a proper superclass of SoupMessage */
struct chime_msg {
	ChimeConnection *cxn;
	ChimeSoupMessageCallback cb;
	gpointer cb_data;
	SoupMessage *msg;
	gboolean auto_renew;
};

typedef struct {
	ChimeConnectionState state;

	gchar *server;
	gchar *device_token;
	gchar *session_token;

	gboolean jugg_online, contacts_online, rooms_online, convs_online, meetings_online;

	/* Service config */
	JsonNode *reg_node;
	const gchar *display_name;
	const gchar *email;

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
	guint keepalive_timer;
	gchar *ws_key;
	GHashTable *subscriptions;

	/* Contacts */
	ChimeObjectCollection contacts;
	ChimeSyncState contacts_sync;
	GSList *contacts_needed;

	/* Rooms */
	ChimeObjectCollection rooms;
	ChimeSyncState rooms_sync;

	/* Conversations */
	ChimeObjectCollection conversations;
	ChimeSyncState conversations_sync;

	/* Meetings */
	ChimeObjectCollection meetings;
	ChimeObjectCollection calls;
} ChimeConnectionPrivate;

#define CHIME_CONNECTION_GET_PRIVATE(o) \
	(G_TYPE_INSTANCE_GET_PRIVATE ((o), CHIME_TYPE_CONNECTION, \
				      ChimeConnectionPrivate))

#define chime_debug(...) do { if (getenv("CHIME_DEBUG")) printf(__VA_ARGS__); } while (0)

/* chime-connection.c */
void chime_connection_fail(ChimeConnection *cxn, gint code,
			   const gchar *format, ...);
void chime_connection_fail_error(ChimeConnection *cxn, GError *error);
void chime_connection_calculate_online(ChimeConnection *cxn);
void chime_connection_new_contact(ChimeConnection *cxn, ChimeContact *contact);
void chime_connection_new_room(ChimeConnection *cxn, ChimeRoom *room);
void chime_connection_new_conversation(ChimeConnection *cxn, ChimeConversation *conversation);
void chime_connection_new_meeting(ChimeConnection *cxn, ChimeMeeting *meeting);
void chime_connection_log(ChimeConnection *cxn, ChimeLogLevel level, const gchar *format, ...);
void chime_connection_progress(ChimeConnection *cxn, int percent, const gchar *message);
SoupMessage *chime_connection_queue_http_request(ChimeConnection *self, JsonNode *node,
						 SoupURI *uri, const gchar *method,
						 ChimeSoupMessageCallback callback,
						 gpointer cb_data);
SoupURI *soup_uri_new_printf(const gchar *base, const gchar *format, ...);
gboolean parse_notify_pref(JsonNode *node, const gchar *member, ChimeNotifyPref *type);
gboolean parse_visibility(JsonNode *node, const gchar *member, gboolean *val);


/* chime-contact.c */
void chime_init_contacts(ChimeConnection *cxn);
void chime_destroy_contacts(ChimeConnection *cxn);
ChimeContact *chime_connection_parse_conversation_contact(ChimeConnection *cxn,
							  JsonNode *node,
							  GError **error);
ChimeContact *chime_connection_parse_contact(ChimeConnection *cxn,
					     gboolean is_contact,
					     JsonNode *node, GError **error);


/* chime-juggernaut.c */
gboolean chime_connection_jugg_send(ChimeConnection *self, JsonNode *node);

/* chime-conversation.c */
void chime_init_conversations(ChimeConnection *cxn);
void chime_destroy_conversations(ChimeConnection *cxn);

/* chime-juggernaut.c */
void chime_init_juggernaut(ChimeConnection *cxn);
void chime_destroy_juggernaut(ChimeConnection *cxn);

typedef gboolean (*JuggernautCallback)(ChimeConnection *cxn,
				       gpointer cb_data, JsonNode *data_node);
void chime_jugg_subscribe(ChimeConnection *cxn, const gchar *channel,
			  const gchar *klass, JuggernautCallback cb,
			  gpointer cb_data);
void chime_jugg_unsubscribe(ChimeConnection *cxn, const gchar *channel,
			    const gchar *klass, JuggernautCallback cb,
			    gpointer cb_data);
void chime_purple_keepalive(PurpleConnection *conn);

/* chime-rooms.c */
void chime_init_rooms(ChimeConnection *cxn);
void chime_destroy_rooms(ChimeConnection *cxn);
gboolean chime_connection_fetch_room(ChimeConnection *cxn, const gchar *id,
				     JuggernautCallback cb, gpointer cb_data);

/* chime-meeting.c */
void chime_init_meetings(ChimeConnection *cxn);
void chime_destroy_meetings(ChimeConnection *cxn);

/* chime-call.c */
void chime_init_calls(ChimeConnection *cxn);
void chime_destroy_calls(ChimeConnection *cxn);
ChimeCall *chime_connection_parse_call(ChimeConnection *cxn, JsonNode *node,
				       GError **error);
ChimeConnection *chime_call_get_connection(ChimeCall *self);

/* login.c */
void chime_initial_login(ChimeConnection *cxn);

#endif /* __CHIME_CONNECTION_PRIVATE_H__ */
