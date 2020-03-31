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

#include <glib.h>

#include <dbus-server.h>
#include <prpl.h>

#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

#include "chime-connection.h"
#include "chime-contact.h"
#include "chime-conversation.h"
#include "chime-room.h"
#include "chime-meeting.h"

struct purple_chime {
	ChimeConnection *cxn;

	GHashTable *ims_by_email;
	GHashTable *ims_by_profile_id;

	GRegex *mention_regex;
	GHashTable *chats_by_room;
	GHashTable *live_chats;
	int chat_id;

	void *convlist_handle;
	guint convlist_refresh_id;

	void *joinable_handle;
	guint joinable_refresh_id;

	/* Allow pin_join to abort a 'joinable meetings' popup */
	GSList *pin_joins;
};

#define PURPLE_CHIME_CXN(conn) (CHIME_CONNECTION(((struct purple_chime *)purple_connection_get_protocol_data(conn))->cxn))

/* authenticate.c */
void purple_request_credentials(PurpleConnection *conn, gpointer state, gboolean user_required);

/* chime.c */
/* BEWARE: msg_id is allocated, msg_time is const. I am going to hate myself
   for that one day, but it's convenient for now... */
gboolean chime_read_last_msg(PurpleConnection *conn, ChimeObject *obj,
			     const gchar **msg_time, gchar **msg_id);

/* buddy.c */
void on_chime_new_contact(ChimeConnection *cxn, ChimeContact *contact, PurpleConnection *conn);
void chime_purple_buddy_free(PurpleBuddy *buddy);
void chime_purple_add_buddy(PurpleConnection *conn, PurpleBuddy *buddy, PurpleGroup *group);
void chime_purple_remove_buddy(PurpleConnection *conn, PurpleBuddy *buddy, PurpleGroup *group);
void chime_purple_user_search(PurplePluginAction *action);
GList *chime_purple_buddy_menu(PurpleBuddy *buddy);

/* meeting.c */
void chime_purple_schedule_onetime(PurplePluginAction *action);
void chime_purple_schedule_personal(PurplePluginAction *action);
void chime_purple_pin_join(PurplePluginAction *action);
void chime_purple_show_joinable(PurplePluginAction *action);
void on_chime_new_meeting(ChimeConnection *cxn, ChimeMeeting *mtg, PurpleConnection *conn);
void purple_chime_init_meetings(PurpleConnection *conn);
void purple_chime_destroy_meetings(PurpleConnection *conn);
gboolean chime_purple_initiate_media(PurpleAccount *account, const char *who,
				     PurpleMediaSessionType type);
void chime_add_joinable_meeting(PurpleAccount *account, const char *pin);

/* rooms.c */
PurpleRoomlist *chime_purple_roomlist_get_list(PurpleConnection *conn);
GList *chime_purple_chat_info(PurpleConnection *conn);
GHashTable *chime_purple_chat_info_defaults(PurpleConnection *conn, const char *name);
char *chime_purple_get_chat_name(GHashTable *components);
gchar *chime_purple_roomlist_room_serialize(PurpleRoomlistRoom *room);

/* chat.c */
struct chime_chat;

void purple_chime_init_chats_post(PurpleConnection *conn);
void purple_chime_init_chats(PurpleConnection *conn);
void purple_chime_destroy_chats(PurpleConnection *conn);
void chime_destroy_chat(struct chime_chat *chat);
void chime_purple_join_chat(PurpleConnection *conn, GHashTable *data);
void chime_purple_chat_leave(PurpleConnection *conn, int id);
int chime_purple_chat_send(PurpleConnection *conn, int id, const char *message, PurpleMessageFlags flags);
char *chime_purple_cb_real_name(PurpleConnection *conn, int id, const char *who);
void on_chime_new_group_conv(ChimeConnection *cxn, ChimeConversation *conv, PurpleConnection *conn);
void chime_purple_chat_invite(PurpleConnection *conn, int id, const char *message, const char *who);
struct chime_chat *do_join_chat(PurpleConnection *conn, ChimeConnection *cxn, ChimeObject *obj, JsonNode *first_msg, ChimeMeeting *meeting);
void chime_purple_chat_join_audio(struct chime_chat *chat);
GList *chime_purple_chat_menu(PurpleChat *chat);
char *chime_purple_get_cb_alias(PurpleConnection *conn, int id, const gchar *who);

/* conversations.c */
void on_chime_new_conversation(ChimeConnection *cxn, ChimeConversation *conv, PurpleConnection *conn);
void purple_chime_init_conversations(PurpleConnection *conn);
void purple_chime_destroy_conversations(PurpleConnection *conn);
int chime_purple_send_im(PurpleConnection *gc, const char *who, const char *message, PurpleMessageFlags flags);
unsigned int chime_send_typing(PurpleConnection *conn, const char *name, PurpleTypingState state);
void chime_purple_recent_conversations(PurplePluginAction *action);

/* dbus.c */
extern PurpleDBusBinding chime_purple_dbus_bindings[];

/* messages.c */
struct chime_msgs;

typedef void (*chime_msg_cb)(ChimeConnection *cxn, struct chime_msgs *msgs,
			     JsonNode *node, time_t tm);
struct chime_msgs {
	PurpleConnection *conn;
	ChimeObject *obj;
	gchar *last_seen;
	gchar *fetch_until;
	GQueue *seen_msgs;
	gboolean unseen;
	GHashTable *msg_gather;
	chime_msg_cb cb;
	gboolean msgs_done, members_done, msgs_failed;
};

void fetch_messages(ChimeConnection *cxn, struct chime_msgs *msgs, const gchar *next_token);
void chime_complete_messages(ChimeConnection *cxn, struct chime_msgs *msgs);
void cleanup_msgs(struct chime_msgs *msgs);
void init_msgs(PurpleConnection *conn, struct chime_msgs *msgs, ChimeObject *obj, chime_msg_cb cb, const gchar *name, JsonNode *first_msg);
void purple_chime_init_messages(PurpleConnection *conn);
void purple_chime_destroy_messages(PurpleConnection *conn);

/* attachments.c */

/*
 * Attachments are located at `data.record.Attachment`. The same structure is
 * sometimes at `data.record.AttachmentVariants` (an array), for giving smaller
 * alternatives for images. (There may be other locations in the incoming JSON
 * messages, those are the ones I found.)
 */
typedef struct _ChimeAttachment {
	/* Not part of the incoming attachment record, but I'm using for getting unique filenames on disk. */
	gchar *message_id;

	gchar *filename;
	gchar *url; /* Valid for 1 hour */
	gchar *content_type;
} ChimeAttachment;

typedef struct _AttachmentContext {
	PurpleConnection *conn;
	const char *from; /* Sender email. May be the own user in case he/she sends from another client. */
	const char *im_email; /* Email identifying the IM conversation. May be the the same as `from` */
	time_t when;
	int chat_id; /* -1 for IM */
} AttachmentContext;

ChimeAttachment *extract_attachment(JsonNode *record);

void download_attachment(ChimeConnection *cxn, ChimeAttachment *att, AttachmentContext *ctx);
void chime_send_file(PurpleConnection *gc, const char *who, const char *filename);

#endif /* __CHIME_H__ */
