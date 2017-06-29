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

#include "connection.h"

#define CHIME_DEVICE_CAP_PUSH_DELIVERY_RECEIPTS		(1<<1)
#define CHIME_DEVICE_CAP_PRESENCE_PUSH			(1<<2)
#define CHIME_DEVICE_CAP_WEBINAR			(1<<3)
#define CHIME_DEVICE_CAP_PRESENCE_SUBSCRIPTION		(1<<4)

#define CHIME_MAX_STATUS 6
const gchar *chime_statuses[CHIME_MAX_STATUS];

typedef void (*ChimeSoupMessageCallback)(ChimeConnection *cxn,
					 SoupMessage *msg,
					 JsonNode *node,
					 gpointer cb_data);

/* SoupMessage handling for Chime communication, with retry on re-auth
 * and JSON parsing. XX: MAke this a proper superclass of SoupMessage */
struct chime_msg {
	ChimeConnection *cxn;
	ChimeSoupMessageCallback cb;
	gpointer cb_data;
	SoupMessage *msg;
	gboolean auto_renew;
};

struct chime_contact
{
	ChimeConnection *cxn;

	gchar *profile_id;
	gchar *presence_channel;
	gchar *profile_channel;
	gchar *email;
	gchar *full_name;
	gchar *display_name;

	int availability;
	gint64 avail_revision;
};

struct chime_msgs;
typedef void (*chime_msg_cb)(ChimeConnection *cxn, struct chime_msgs *msgs,
			     JsonNode *node, time_t tm);

struct chime_msgs {
	gboolean is_room;
	const gchar *id; /* of the conversation or room */
	gchar *last_msg; /* MessageId of last known message */
	GHashTable *messages; /* While fetching */
	SoupMessage *soup_msg; /* For cancellation */
	gboolean members_done, msgs_done;
	chime_msg_cb cb;
};

struct chime_chat;

struct chime_room {
	struct chime_chat *chat;

	gchar *channel;
	gchar *id;
	gchar *type;
	gchar *name;
	gchar *privacy;
	gchar *visibility;
};

extern GQuark pidgin_chime_error_quark(void);
#define CHIME_ERROR pidgin_chime_error_quark()

enum {
	CHIME_ERROR_REQUEST_FAILED = 1,
	CHIME_ERROR_BAD_RESPONSE,
};

#define CONNECT_STEPS 3

#define SIGNIN_DEFAULT "https://signin.id.ue1.app.chime.aws/"

/* login.c */
void chime_initial_login(ChimeConnection *cxn);

/* chime.c */
gboolean parse_int(JsonNode *node, const gchar *member, gint64 *val);
gboolean parse_string(JsonNode *parent, const gchar *name, const gchar **res);
gboolean parse_time(JsonNode *parent, const gchar *name, const gchar **time_str, GTimeVal *tv);
SoupURI *soup_uri_new_printf(const gchar *base, const gchar *format, ...);
SoupMessage *__chime_queue_http_request(ChimeConnection *cxn, JsonNode *node,
					SoupURI *uri, ChimeSoupMessageCallback callback,
					gpointer cb_data, gboolean auto_renew);
#define chime_queue_http_request(_c, _n, _u, _cb, _d)			\
	__chime_queue_http_request((_c), (_n), (_u), (_cb), (_d), TRUE)

void chime_update_last_msg(ChimeConnection *cxn, gboolean is_room,
			   const gchar *id, const gchar *msg_time,
			   const gchar *msg_id);
/* BEWARE: msg_id is allocated, msg_time is const. I am going to hate myself
   for that one day, but it's convenient for now... */
gboolean chime_read_last_msg(ChimeConnection *cxn, gboolean is_room,
			     const gchar *id, const gchar **msg_time,
			     gchar **msg_id);

/* jugg.c */
void chime_init_juggernaut(ChimeConnection *cxn);
void chime_destroy_juggernaut(ChimeConnection *cxn);
gboolean chime_jugg_send(ChimeConnection *cxn, JsonNode *node);

typedef gboolean (*JuggernautCallback)(ChimeConnection *cxn, gpointer cb_data, JsonNode *data_node);
void chime_jugg_subscribe(ChimeConnection *cxn, const gchar *channel, const gchar *klass, JuggernautCallback cb, gpointer cb_data);
void chime_jugg_unsubscribe(ChimeConnection *cxn, const gchar *channel, const gchar *klass, JuggernautCallback cb, gpointer cb_data);
void chime_purple_keepalive(PurpleConnection *conn);

/* buddy.c */
void fetch_buddies(ChimeConnection *cxn);
void chime_purple_buddy_free(PurpleBuddy *buddy);
void chime_purple_add_buddy(PurpleConnection *conn, PurpleBuddy *buddy, PurpleGroup *group);
void chime_purple_remove_buddy(PurpleConnection *conn, PurpleBuddy *buddy, PurpleGroup *group);
void chime_init_buddies(ChimeConnection *cxn);
void chime_destroy_buddies(ChimeConnection *cxn);
struct chime_contact *chime_contact_new(ChimeConnection *cxn, JsonNode *node, gboolean conv);

/* rooms.c */
PurpleRoomlist *chime_purple_roomlist_get_list(PurpleConnection *conn);
void chime_init_rooms(ChimeConnection *cxn);
void chime_destroy_rooms(ChimeConnection *cxn);
GList *chime_purple_chat_info(PurpleConnection *conn);
GHashTable *chime_purple_chat_info_defaults(PurpleConnection *conn, const char *name);

/* chat.c */
void chime_init_chats(ChimeConnection *cxn);
void chime_destroy_chats(ChimeConnection *cxn);
void chime_destroy_chat(struct chime_chat *chat);
void chime_purple_join_chat(PurpleConnection *conn, GHashTable *data);
void chime_purple_chat_leave(PurpleConnection *conn, int id);
int chime_purple_chat_send(PurpleConnection *conn, int id, const char *message, PurpleMessageFlags flags);

/* conversations.c */
void chime_init_conversations(ChimeConnection *cxn);
void chime_destroy_conversations(ChimeConnection *cxn);
int chime_purple_send_im(PurpleConnection *gc, const char *who, const char *message, PurpleMessageFlags flags);
unsigned int chime_send_typing(PurpleConnection *conn, const char *name, PurpleTypingState state);

/* messages.c */
void fetch_messages(ChimeConnection *cxn, struct chime_msgs *msgs, const gchar *next_token);
void chime_complete_messages(ChimeConnection *cxn, struct chime_msgs *msgs);
#endif /* __CHIME_H__ */
