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

#include <string.h>

#include <glib/gi18n.h>
#include <glib/glist.h>

#include <prpl.h>
#include <blist.h>
#include <roomlist.h>
#include <debug.h>
#include <media.h>
#include <mediamanager.h>
#include <media-gst.h>

#include "chime.h"
#include "chime-connection-private.h"
#include "chime-room.h"
#include "chime-meeting.h"

#include <libsoup/soup.h>

#include <gst/gstelement.h>
#include <gst/gstpipeline.h>
#include <gst/gstutils.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>

struct chime_chat {
	/* msgs first as it's a "subclass". Really ought to do proper GTypes here... */
	struct chime_msgs m;
	PurpleConversation *conv;
	ChimeMeeting *meeting;
	ChimeCall *call;
	ChimeCallAudio *audio;
	void *participants_ui;
	PurpleMedia *media;

	GstElement *audio_inpipeline;
	GstElement *audio_outpipeline;
};

/*
 * Examples:
 *
 * <@all|All members> becomes All members
 * <@present|Present members> becomes Present members
 * <@75f50e24-d59d-40e4-996b-6ba3ff3f371f|Surname, Name> becomes Surname, Name
 */
#define MENTION_PATTERN "&lt;@([\\w\\-]+)\\|(.*?)&gt;"
#define MENTION_REPLACEMENT "<b>\\2</b>"

/*
 * Returns whether `me` was mentioned in the Chime `message`, and allocates a
 * new string in `*parsed`.
 */
static int parse_inbound_mentions(ChimeConnection *cxn, GRegex *mention_regex, const char *message, char **parsed)
{
	*parsed = g_regex_replace(mention_regex, message, -1, 0, MENTION_REPLACEMENT, 0, NULL);
	return strstr(message, chime_connection_get_profile_id(cxn)) || strstr(message, "&lt;@all|") ||
		strstr(message, "&lt;@present|");
}

static void replace(gchar **dst, const gchar *a, const gchar *b)
{
       gchar **parts = g_strsplit(*dst, a, 0);
       gchar *replaced = g_strjoinv(b, parts);
       g_strfreev(parts);
       g_free(*dst);
       *dst = replaced;
}

/*
 * This will simple look for all chat members mentions and replace them with
 * the Chime format for mentioning. As a special case we expand "@all" and
 * "@present".
 */
static gchar *parse_outbound_mentions(ChimeRoom *room, const gchar *message)
{
	GList *members = chime_room_get_members(room);

	gchar *parsed = g_strdup(message);
	replace(&parsed, "@all", "<@all|All Members>");
	replace(&parsed, "@present", "<@present|Present Members>");
	while (members) {
		ChimeRoomMember *member = members->data;
		const gchar *id = chime_contact_get_profile_id(member->contact);
		const gchar *display_name = chime_contact_get_display_name(member->contact);

		if (strstr(parsed, display_name)) {
			gchar *chime_mention = g_strdup_printf("<@%s|%s>", id, display_name);
			replace(&parsed, display_name, chime_mention);
			g_free(chime_mention);
		}

		members = g_list_remove(members, member);
	}
	return parsed;
}

static void do_chat_deliver_msg(ChimeConnection *cxn, struct chime_msgs *msgs,
				JsonNode *node, time_t msg_time)
{
	struct chime_chat *chat = (struct chime_chat *)msgs;
	PurpleConnection *conn = chat->conv->account->gc;
	struct purple_chime *pc = purple_connection_get_protocol_data(conn);
	int id = purple_conv_chat_get_id(PURPLE_CONV_CHAT(chat->conv));
	const gchar *content, *sender;

	if (!parse_string(node, "Content", &content) ||
	    !parse_string(node, "Sender", &sender))
		return;

	const gchar *from = _("Unknown sender");
	int msg_flags;

	if (!strcmp(sender, chime_connection_get_profile_id(cxn))) {
		from = chime_connection_get_email(cxn);
		msg_flags = PURPLE_MESSAGE_SEND;
	} else {
		ChimeContact *who = chime_connection_contact_by_id(cxn, sender);
		if (who)
			from = chime_contact_get_email(who);
		msg_flags = PURPLE_MESSAGE_RECV;
	}

	gchar *escaped = g_markup_escape_text(content, -1);

	gchar *parsed = NULL;
	if (CHIME_IS_ROOM(chat->m.obj)) {
		if (parse_inbound_mentions(cxn, pc->mention_regex, escaped, &parsed) &&
		    (msg_flags & PURPLE_MESSAGE_RECV)) {
			// Presumably this will trigger a notification.
			msg_flags |= PURPLE_MESSAGE_NICK;
		}
		g_free(escaped);
	} else
		parsed = escaped;

	ChimeAttachment *att = extract_attachment(node);
	if (att) {
		AttachmentContext *ctx = g_new(AttachmentContext, 1);
		ctx->conn = conn;
		ctx->chat_id = id;
		ctx->from = from;
		ctx->im_email = "";
		ctx->when = msg_time;
		/* The attachment and context structs will be owned by the code doing the download and will be disposed of at the end. */
		download_attachment(cxn, att, ctx);
	}

	serv_got_chat_in(conn, id, from, msg_flags, parsed, msg_time);
	/* If the conversation already had focus and unseen-count didn't change, fake
	   a PURPLE_CONV_UPDATE_UNSEEN notification anyway, so that we see that it's
	   (still) zero and tell the server it's read. */
	purple_conversation_update(chat->conv, PURPLE_CONV_UPDATE_UNSEEN);
	g_free(parsed);
}

static gint participant_sort(gconstpointer a, gconstpointer b)
{
	const ChimeCallParticipant *pa = a, *pb = b;

	if (pa->status == pb->status)
		return g_strcmp0(pa->full_name, pb->full_name);
	else
		return pa->status - pb->status;
}

static PurpleNotifySearchResults *generate_sr_participants(GHashTable *participants)
{
	PurpleNotifySearchResults *results = purple_notify_searchresults_new();
	PurpleNotifySearchColumn *column;

	column = purple_notify_searchresults_column_new(_("Name"));
	purple_notify_searchresults_column_add(results, column);
	column = purple_notify_searchresults_column_new(_("Status"));
	purple_notify_searchresults_column_add(results, column);
	column = purple_notify_searchresults_column_new("");
	purple_notify_searchresults_column_add(results, column);

	gpointer klass = g_type_class_ref(CHIME_TYPE_CALL_PARTICIPATION_STATUS);

	GList *pl = g_hash_table_get_values(participants);
	pl = g_list_sort(pl, participant_sort);
	while (pl) {
		ChimeCallParticipant *p = pl->data;
		GList *row = NULL;
		row = g_list_append(row, g_strdup(p->full_name));
		GEnumValue *val = g_enum_get_value(klass, p->status);
		row = g_list_append(row, g_strdup(_(val->value_nick)));

		const gchar *vol_icon;
		if (p->status != CHIME_PARTICIPATION_PRESENT)
			vol_icon = "";
		else if (p->volume == -128)
			vol_icon = "🔇";
		else if (p->volume < -64)
			vol_icon = "🔈";
		else if (p->volume < -32)
			vol_icon = "🔉";
		else
			vol_icon = "🔊";
		row = g_list_append(row, g_strdup(vol_icon));

		purple_notify_searchresults_row_add(results, row);

		pl = g_list_remove(pl, p);
	}
	g_type_class_unref(klass);
	return results;

}
static void on_call_participants(ChimeCall *call, GHashTable *participants, struct chime_chat *chat);

static void participants_closed_cb(gpointer _chat)
{
	struct chime_chat *chat = _chat;
	chat->participants_ui = NULL;
	g_signal_handlers_disconnect_matched(chat->call, G_SIGNAL_MATCH_FUNC|G_SIGNAL_MATCH_DATA,
					     0, 0, NULL, G_CALLBACK(on_call_participants), chat);
}

static void call_stream_info(PurpleMedia *media, PurpleMediaInfoType type, gchar *id, const gchar *participant, gboolean local, struct chime_chat *chat)
{
	printf("%s %d\n", __func__, type);
	if (type == PURPLE_MEDIA_INFO_MUTE) {
		chime_call_set_local_mute(chat->call, TRUE);
	} else if (type == PURPLE_MEDIA_INFO_UNMUTE) {
		chime_call_set_local_mute(chat->call, FALSE);
	}
}

static void call_media_changed(PurpleMedia *media, PurpleMediaState state, const gchar *id, const gchar *participant, struct chime_chat *chat)
{

	if (state == PURPLE_MEDIA_STATE_END && !id && !participant) {
		chat->media = NULL;
		chime_call_set_mute(chat->call, TRUE);
	}
}

static void on_audio_state(ChimeCall *call, ChimeAudioState audio_state, struct chime_chat *chat)
{
	purple_debug(PURPLE_DEBUG_INFO, "chime", "Audio state %d\n", audio_state);

	const gchar *name = chime_call_get_alert_body(chat->call);

	if (audio_state == CHIME_AUDIO_STATE_AUDIO_MUTED && chat->media) {
		purple_media_stream_info(chat->media, PURPLE_MEDIA_INFO_MUTE, "chime", name, FALSE);
	} else if (audio_state == CHIME_AUDIO_STATE_AUDIO && chat->media) {
		purple_media_stream_info(chat->media, PURPLE_MEDIA_INFO_UNMUTE, "chime", name, FALSE);
	} else if (audio_state == CHIME_AUDIO_STATE_AUDIO && !chat->media) {
		PurpleMediaManager *mgr = purple_media_manager_get();
		chat->media = purple_media_manager_create_media(purple_media_manager_get(),
								chat->conv->account,
								"fsrawconference",
								name,
								TRUE);
		if (chat->media) {
			gboolean r = purple_media_add_stream(chat->media, "chime", name,
							     PURPLE_MEDIA_AUDIO, TRUE,
							     "app", 0, NULL);
			gchar *srcname = g_strdup_printf("chime_src_%p", call);
			gchar *sinkname = g_strdup_printf("chime_sink_%p", call);
			gchar *srcpipe = g_strdup_printf("appsrc name=%s format=time caps=audio/x-opus,channel-mapping-family=0 ! opusdec ! audioconvert ! audioresample ", srcname);
			gchar *sinkpipe = g_strdup_printf("opusenc bitrate=16000 bitrate-type=vbr ! appsink name=%s async=false", sinkname);

			PurpleMediaCandidate *cand =
				purple_media_candidate_new(NULL, 1,
							   PURPLE_MEDIA_CANDIDATE_TYPE_HOST,
							   PURPLE_MEDIA_NETWORK_PROTOCOL_UDP,
							   sinkpipe, 0);
			g_object_set(cand, "username", srcpipe, NULL);
			g_free(sinkpipe);
			g_free(srcpipe);

			GList *cands = g_list_append (NULL, cand);
			GList *codecs = g_list_append(NULL,
						      purple_media_codec_new(1, "audio/x-raw, format=(string)S16LE, rate=(int)16000, layout=(string)interleaved, channels=(int)1", PURPLE_MEDIA_AUDIO, 0));
			purple_media_add_remote_candidates(chat->media, "chime", name, cands);
			purple_media_set_remote_codecs(chat->media, "chime", name, codecs);

			GstElement *pipeline = purple_media_manager_get_pipeline(mgr);
			GstElement *appsrc = gst_bin_get_by_name(GST_BIN(pipeline), srcname);
			GstElement *appsink = gst_bin_get_by_name(GST_BIN(pipeline), sinkname);
			g_free(srcname);
			g_free(sinkname);
			printf("Got src %p sink %p\n", appsrc, appsink);
			gst_app_src_set_size(GST_APP_SRC(appsrc), -1);
			gst_app_src_set_max_bytes(GST_APP_SRC(appsrc), 100);
			gst_app_src_set_stream_type(GST_APP_SRC(appsrc), GST_APP_STREAM_TYPE_STREAM);

			chime_call_install_gst_app_callbacks(chat->call, GST_APP_SRC(appsrc), GST_APP_SINK(appsink));
			g_signal_connect(chat->media, "state-changed", G_CALLBACK(call_media_changed), chat);
			g_signal_connect(chat->media, "stream-info", G_CALLBACK(call_stream_info), chat);

			purple_media_stream_info(chat->media, PURPLE_MEDIA_INFO_ACCEPT, "chime", name, FALSE);
			GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(purple_media_manager_get_pipeline(mgr)), GST_DEBUG_GRAPH_SHOW_ALL, "chime conference graph");
		}
	}


}

static void on_call_participants(ChimeCall *call, GHashTable *participants, struct chime_chat *chat)
{
	PurpleNotifySearchResults *results = generate_sr_participants(participants);
	PurpleConnection *conn = chat->conv->account->gc;

	if (!chat->participants_ui) {
		chat->participants_ui = purple_notify_searchresults(conn, _("Call Participants"),
								    chime_meeting_get_name(chat->meeting),
								    NULL, results, participants_closed_cb, chat);
	} else {
		purple_notify_searchresults_new_rows(conn, results, chat->participants_ui);
	}
}

static void on_room_membership(ChimeRoom *room, ChimeRoomMember *member, struct chime_chat *chat)
{
	const gchar *who = chime_contact_get_email(member->contact);

	if (!member->active) {
		if (purple_conv_chat_find_user(PURPLE_CONV_CHAT(chat->conv), who))
			purple_conv_chat_remove_user(PURPLE_CONV_CHAT(chat->conv), who, NULL);
		return;
	}

	PurpleConvChatBuddyFlags flags = 0;
	if (member->admin)
		flags |= PURPLE_CBFLAGS_OP;
	if (!member->present)
		flags |= PURPLE_CBFLAGS_AWAY;

	if (purple_conv_chat_find_user(PURPLE_CONV_CHAT(chat->conv), who))
		purple_conv_chat_user_set_flags(PURPLE_CONV_CHAT(chat->conv), who, flags);
	else {
		purple_conv_chat_add_user(PURPLE_CONV_CHAT(chat->conv), who,
					  NULL, flags, FALSE);
		PurpleConvChatBuddy *cbuddy = purple_conv_chat_cb_find(PURPLE_CONV_CHAT(chat->conv), who);
		if (cbuddy) {
			g_free(cbuddy->alias);
			cbuddy->alias = g_strdup(chime_contact_get_display_name(member->contact));
		}
	}
}

void chime_destroy_chat(struct chime_chat *chat)
{
	PurpleConnection *conn = chat->conv->account->gc;
	struct purple_chime *pc = purple_connection_get_protocol_data(conn);
	ChimeConnection *cxn = PURPLE_CHIME_CXN(conn);

	int id = purple_conv_chat_get_id(PURPLE_CONV_CHAT(chat->conv));

	g_signal_handlers_disconnect_matched(chat->m.obj, G_SIGNAL_MATCH_DATA,
					     0, 0, NULL, NULL, chat);
	if (CHIME_IS_ROOM(chat->m.obj))
		chime_connection_close_room(cxn, CHIME_ROOM(chat->m.obj));

	serv_got_chat_left(conn, id);

	if (chat->meeting) {
		if (chat->participants_ui) {
			purple_notify_close(PURPLE_NOTIFY_SEARCHRESULTS, chat->participants_ui);
			chat->participants_ui = NULL;
		}

		g_signal_handlers_disconnect_matched(chat->call, G_SIGNAL_MATCH_DATA,
						     0, 0, NULL, NULL, chat);

		if (chat->media) {
			purple_media_end(chat->media, "chime", chime_call_get_alert_body(chat->call));
			purple_media_manager_remove_media(purple_media_manager_get(),
							  chat->media);
			chat->media = NULL;
		}

		if (chat->audio_inpipeline) {
			GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(chat->audio_inpipeline), GST_DEBUG_GRAPH_SHOW_ALL, "chime-inpipeline");

			gst_element_set_state(chat->audio_inpipeline, GST_STATE_NULL);
			gst_object_unref(chat->audio_inpipeline);
			chat->audio_inpipeline = NULL;
		}

		if (chat->audio_outpipeline) {
			GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(chat->audio_outpipeline), GST_DEBUG_GRAPH_SHOW_ALL, "chime-outpipeline");

			gst_element_set_state(chat->audio_outpipeline, GST_STATE_NULL);
			gst_object_unref(chat->audio_outpipeline);
			chat->audio_outpipeline = NULL;
		}

		chime_connection_close_meeting(cxn, chat->meeting);
		g_object_unref(chat->meeting);
	}
	g_hash_table_remove(pc->live_chats, GUINT_TO_POINTER(id));
	g_hash_table_remove(pc->chats_by_room, chat->m.obj);
	cleanup_msgs(&chat->m);
	g_free(chat);
	purple_debug(PURPLE_DEBUG_INFO, "chime", "Destroyed chat %p\n", chat);
}

static void on_group_conv_msg(ChimeConversation *conv, JsonNode *node, PurpleConnection *conn);

static void on_chat_name(ChimeObject *obj, GParamSpec *ignored, struct chime_chat *chat)
{
	const gchar *name = chime_object_get_name(obj);

	if (name && chat->conv)
		purple_conversation_set_name(chat->conv, name);
}

struct chime_chat *do_join_chat(PurpleConnection *conn, ChimeConnection *cxn, ChimeObject *obj, JsonNode *first_msg, ChimeMeeting *meeting)
{
	if (!obj)
		return NULL;

	struct purple_chime *pc = purple_connection_get_protocol_data(conn);
	struct chime_chat *chat = g_hash_table_lookup(pc->chats_by_room, obj);
	if (chat)
		return chat;

	chat = g_new0(struct chime_chat, 1);

	if (meeting) {
		chat->meeting = g_object_ref(meeting);
		chat->call = chime_meeting_get_call(meeting);
		if (chat->call) {
			g_signal_connect(chat->call, "audio-state", G_CALLBACK(on_audio_state), chat);
			g_signal_connect(chat->call, "participants-changed", G_CALLBACK(on_call_participants), chat);
		}
	}

	int chat_id = ++pc->chat_id;
	const gchar *name = chime_object_get_name(obj);
	if (!name || !name[0])
		name = chime_object_get_id(obj);

	chat->conv = serv_got_joined_chat(conn, chat_id, name);

	g_hash_table_insert(pc->live_chats, GUINT_TO_POINTER(chat_id), chat);
	g_hash_table_insert(pc->chats_by_room, obj, chat);
	init_msgs(conn, &chat->m, obj, do_chat_deliver_msg, name, first_msg);

	g_signal_connect(obj, "notify::name", G_CALLBACK(on_chat_name), chat);

	if (CHIME_IS_ROOM(obj)) {
		g_signal_connect(obj, "membership", G_CALLBACK(on_room_membership), chat);
		chime_connection_open_room(cxn, CHIME_ROOM(obj));
	} else {
		g_signal_handlers_disconnect_matched(chat->m.obj, G_SIGNAL_MATCH_FUNC|G_SIGNAL_MATCH_DATA, 0, 0, NULL,
						     G_CALLBACK(on_group_conv_msg), conn);

		GList *members = chime_conversation_get_members(CHIME_CONVERSATION(obj));
		while (members) {
			ChimeContact *member = members->data;
			purple_conv_chat_add_user(PURPLE_CONV_CHAT(chat->conv), chime_contact_get_email(member),
						  NULL, 0, FALSE);
			members = g_list_remove(members, member);
		}
	}

	return chat;
}

static void on_group_conv_msg(ChimeConversation *conv, JsonNode *node, PurpleConnection *conn)
{
	struct purple_chime *pc = purple_connection_get_protocol_data(conn);
	struct chime_chat *chat = g_hash_table_lookup(pc->chats_by_room, conv);

	if (!chat)
		chat = do_join_chat(conn, pc->cxn, CHIME_OBJECT(conv), node, NULL);
}

void chime_purple_join_chat(PurpleConnection *conn, GHashTable *data)
{
	ChimeConnection *cxn = PURPLE_CHIME_CXN(conn);
	const gchar *roomid = g_hash_table_lookup(data, "RoomId");
	const gchar *name = g_hash_table_lookup(data, "Name");

	purple_debug(PURPLE_DEBUG_INFO, "chime", "join_chat %p %s %s\n", data, roomid, name);

	ChimeRoom *room = NULL;
	if (roomid)
		room = chime_connection_room_by_id(cxn, roomid);
	if (!room && name)
		room = chime_connection_room_by_name(cxn, name);
	if (!room)
		return;

	/* Ensure all fields are populated and up to date even if we got here from a partial blist node */
	g_hash_table_insert(data, g_strdup("Name"), g_strdup(chime_room_get_name(room)));
	if (!roomid)
		g_hash_table_insert(data, g_strdup("RoomId"), g_strdup(chime_room_get_id(room)));

	do_join_chat(conn, cxn, CHIME_OBJECT(room), NULL, NULL);
}

void chime_purple_chat_leave(PurpleConnection *conn, int id)
{
	struct purple_chime *pc = purple_connection_get_protocol_data(conn);
	struct chime_chat *chat = g_hash_table_lookup(pc->live_chats, GUINT_TO_POINTER(id));

	/* If it's a group conversation, we need to subscribe to the 'message' signal
	 * in order to bring it back again if there's a new message. */
	if (CHIME_IS_CONVERSATION(chat->m.obj))
		g_signal_connect(chat->m.obj, "message", G_CALLBACK(on_group_conv_msg), conn);

	chime_destroy_chat(chat);
}

static void sent_msg_cb(GObject *source, GAsyncResult *result, gpointer _chat)
{
	struct chime_chat *chat = _chat;
	ChimeConnection *cxn = CHIME_CONNECTION(source);
	GError *error = NULL;

	JsonNode *msgnode = chime_connection_send_message_finish(cxn, result, &error);
	if (msgnode) {
		const gchar *msg_id;
		if (!parse_string(msgnode, "MessageId", &msg_id))
			purple_conversation_write(chat->conv, NULL, _("Failed to send message"), PURPLE_MESSAGE_ERROR, time(NULL));
		json_node_unref(msgnode);
	} else {
		purple_conversation_write(chat->conv, NULL, error->message, PURPLE_MESSAGE_ERROR, time(NULL));
		g_clear_error(&error);
	}
}

int chime_purple_chat_send(PurpleConnection *conn, int id, const char *message, PurpleMessageFlags flags)
{
	struct purple_chime *pc = purple_connection_get_protocol_data(conn);
	struct chime_chat *chat = g_hash_table_lookup(pc->live_chats, GUINT_TO_POINTER(id));

	/* Chime does not understand HTML. */
	gchar *expanded, *unescaped;

	purple_markup_html_to_xhtml(message, NULL, &unescaped);

	if (CHIME_IS_ROOM(chat->m.obj)) {
		/* Expand member names into the format Chime understands */
		expanded = parse_outbound_mentions(CHIME_ROOM(chat->m.obj), unescaped);
		g_free(unescaped);
	} else
		expanded = unescaped;

	chime_connection_send_message_async(pc->cxn, chat->m.obj, expanded, NULL, sent_msg_cb, chat);

	g_free(expanded);
	return 0;
}

void purple_chime_init_chats(struct purple_chime *pc)
{
	pc->live_chats = g_hash_table_new(g_direct_hash, g_direct_equal);
	pc->chats_by_room = g_hash_table_new(g_direct_hash, g_direct_equal);

	pc->mention_regex = g_regex_new(MENTION_PATTERN, G_REGEX_EXTENDED, 0, NULL);

}

void purple_chime_destroy_chats(struct purple_chime *pc)
{
	GList *chats = g_hash_table_get_values(pc->live_chats);
	while (chats) {
		chime_destroy_chat(chats->data);
		chats = g_list_remove(chats, chats->data);
	}
	g_clear_pointer(&pc->live_chats, g_hash_table_unref);
	g_clear_pointer(&pc->chats_by_room, g_hash_table_unref);
	g_clear_pointer(&pc->mention_regex, g_regex_unref);
}

static void on_chime_room_mentioned(ChimeConnection *cxn, ChimeObject *obj, JsonNode *node, PurpleConnection *conn)
{
	struct purple_chime *pc = purple_connection_get_protocol_data(conn);
	struct chime_chat *chat = g_hash_table_lookup(pc->chats_by_room, obj);

	if (!chat)
		chat = do_join_chat(conn, cxn, obj, node, NULL);
}

static void on_chime_new_room(ChimeConnection *cxn, ChimeRoom *room, PurpleConnection *conn)
{
	const gchar *last_mentioned;
	GTimeVal mention_tv;

	/* If no LastMentioned or we can't parse it, nothing to do */
	last_mentioned = chime_room_get_last_mentioned(room);
	if (!last_mentioned || !g_time_val_from_iso8601(last_mentioned, &mention_tv))
		return;

	const gchar *msg_time;
	GTimeVal msg_tv;

	if (chime_read_last_msg(conn, CHIME_OBJECT(room), &msg_time, NULL) &&
	    g_time_val_from_iso8601(msg_time, &msg_tv) &&
	    (mention_tv.tv_sec < msg_tv.tv_sec ||
	     (mention_tv.tv_sec == msg_tv.tv_sec && mention_tv.tv_usec <= msg_tv.tv_usec))) {
		/* LastMentioned is older than we've already seen. Nothing to do. */
		return;
	}

	/* We have been mentioned since we last looked at this room. Open it now. */
	do_join_chat(conn, cxn, CHIME_OBJECT(room), NULL, NULL);
}

void on_chime_new_group_conv(ChimeConnection *cxn, ChimeConversation *conv, PurpleConnection *conn)
{
	const gchar *last_sent;
	GTimeVal sent_tv;

	/* If no LastMentioned or we can't parse it, nothing to do */
	last_sent = chime_conversation_get_last_sent(conv);
	if (!last_sent || !g_time_val_from_iso8601(last_sent, &sent_tv) ||
	    (!sent_tv.tv_sec && !sent_tv.tv_usec))
		return;

	const gchar *seen_time;
	GTimeVal seen_tv;

	if (chime_read_last_msg(conn, CHIME_OBJECT(conv), &seen_time, NULL) &&
	    g_time_val_from_iso8601(seen_time, &seen_tv) &&
	    (sent_tv.tv_sec < seen_tv.tv_sec ||
	     (sent_tv.tv_sec == seen_tv.tv_sec && sent_tv.tv_usec <= seen_tv.tv_usec))) {
		/* LastSent is older than we've already seen. Nothing to do except
		 * hook up the signal to open the "chat" when a message comes in */
		g_signal_connect(conv, "message", G_CALLBACK(on_group_conv_msg), conn);
		return;
	}

	/* There is a recent message in this conversation. Open it now. */
	do_join_chat(conn, cxn, CHIME_OBJECT(conv), NULL, NULL);
}

void purple_chime_init_chats_post(PurpleConnection *conn)
{
	struct purple_chime *pc = purple_connection_get_protocol_data(conn);

	chime_connection_foreach_room(pc->cxn, (ChimeRoomCB)on_chime_new_room, conn);

	g_signal_connect(pc->cxn, "room-mention", G_CALLBACK(on_chime_room_mentioned), conn);
}

static void add_member_cb(GObject *source, GAsyncResult *result, gpointer _chat)
{
	struct chime_chat *chat = _chat;
	ChimeConnection *cxn = CHIME_CONNECTION(source);
	GError *error = NULL;

	if (!chime_connection_add_room_member_finish(cxn, result, &error)) {
		purple_conversation_write(chat->conv, NULL, error->message,
					  PURPLE_MESSAGE_ERROR, time(NULL));
	}
	/* If it succeeds, that's self-evident. */
}
struct member_add_data {
	struct chime_chat *chat;
	char *who;
};

static void autocomplete_mad_cb(GObject *source, GAsyncResult *result, gpointer _mad)
{
	ChimeConnection *cxn = CHIME_CONNECTION(source);
	struct member_add_data *mad = _mad;
	GSList *contacts = chime_connection_autocomplete_contact_finish(cxn, result, NULL);

	while (contacts) {
		ChimeContact *contact = contacts->data;
		if (!strcmp(mad->who, chime_contact_get_email(contact))) {
			chime_connection_add_room_member_async(cxn, CHIME_ROOM(mad->chat->m.obj), contact, NULL, add_member_cb, mad->chat);
			g_slist_free_full(contacts, g_object_unref);
			goto out;
		}
		g_object_unref(contact);
		contacts = g_slist_remove(contacts, contact);
	}
	purple_conversation_write(mad->chat->conv, NULL, _("Failed to find user to add"),
				  PURPLE_MESSAGE_ERROR, time(NULL));
 out:
	g_free(mad->who);
	g_free(mad);
}

void chime_purple_chat_invite(PurpleConnection *conn, int id, const char *message, const char *who)
{
	struct purple_chime *pc = purple_connection_get_protocol_data(conn);

	struct chime_chat *chat = g_hash_table_lookup(pc->live_chats, GUINT_TO_POINTER(id));
	if (!chat)
		return;

	if (!CHIME_IS_ROOM(chat->m.obj)) {
		purple_conversation_write(chat->conv, NULL, _("You only add people to chat rooms, not conversations"),
					  PURPLE_MESSAGE_ERROR, time(NULL));
		return;
	}

	ChimeContact *contact = chime_connection_contact_by_email(pc->cxn, who);
	if (contact) {
		chime_connection_add_room_member_async(pc->cxn, CHIME_ROOM(chat->m.obj), contact, NULL, add_member_cb, chat);
		return;
	}

	struct member_add_data *mad = g_new0(struct member_add_data, 1);
	mad->chat = chat;
	mad->who = g_strdup(who);
	chime_connection_autocomplete_contact_async(pc->cxn, who, NULL, autocomplete_mad_cb, mad);
}

static void show_participants (PurpleBuddy *buddy, gpointer _chat)
{
	struct chime_chat *chat = _chat;
	if (chat->call) {
		g_signal_handlers_disconnect_matched(chat->call, G_SIGNAL_MATCH_FUNC|G_SIGNAL_MATCH_DATA,
						     0, 0, NULL, G_CALLBACK(on_call_participants), chat);
		g_signal_connect(chat->call, "participants-changed", G_CALLBACK(on_call_participants), chat);
		chime_call_emit_participants(chat->call);
	}
}

static void join_audio(PurpleBuddy *buddy, gpointer _chat)
{
	struct chime_chat *chat = _chat;
	if (chat->call)
		chime_call_set_mute(chat->call, FALSE);
}

GList *chime_purple_chat_menu(PurpleChat *pchat)
{

	if (!pchat->components)
		return NULL;

	const gchar *roomid = g_hash_table_lookup(pchat->components, (char *)"RoomId");
	if (!roomid)
		return NULL;

	purple_debug_info("chime", "Chat menu for %s\n", roomid);

	PurpleConnection *conn = pchat->account->gc;
	if (!conn)
		return NULL;

	struct purple_chime *pc = purple_connection_get_protocol_data(conn);
	ChimeRoom *room = chime_connection_room_by_id(pc->cxn, roomid);
	if (!room)
		return NULL;

	struct chime_chat *chat = g_hash_table_lookup(pc->chats_by_room, room);
	if (!chat)
		return NULL;

	GList *items = NULL;
	if (chat->call) {
		items = g_list_append(items,
				      purple_menu_action_new(_("Show participants"),
							     PURPLE_CALLBACK(show_participants), chat, NULL));
		items = g_list_append(items,
				      purple_menu_action_new(_("Join audio call"),
							     PURPLE_CALLBACK(join_audio), chat, NULL));
	}

	return items;
}
