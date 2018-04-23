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
#include <request.h>
#include <debug.h>
#include <media.h>
#include <mediamanager.h>
#include <media-gst.h>

#include "chime.h"
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
	void *participants_ui;
	PurpleMedia *media;
	gboolean media_connected;

	gchar *presenter_id;
	void *screen_ask_ui;
	gchar *screen_title;
	PurpleMedia *screen_media;

	void *share_select_ui;
	PurpleMedia *share_media;
	PurpleMediaElementInfo *screen_src_info;
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

static void replace(gchar **dst, const gchar *pattern, const gchar *replacement)
{
	GRegex *regex = g_regex_new(pattern, 0, 0, NULL);
	gchar* replaced = g_regex_replace_literal(regex, *dst, -1, 0, replacement, 0, NULL);
	g_regex_unref(regex);
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

		if (member->active) {
			const gchar *id = chime_contact_get_profile_id(member->contact);
			const gchar *display_name = chime_contact_get_display_name(member->contact);

			if (strstr(parsed, display_name)) {
				gchar *display_name_escaped = g_regex_escape_string(display_name, -1);
				gchar *search = g_strdup_printf("(?<!\\|)\\b%s\\b", display_name_escaped);
				g_free(display_name_escaped);
				gchar *chime_mention = g_strdup_printf("<@%s|%s>", id, display_name);
				replace(&parsed, search, chime_mention);
				g_free(search);
				g_free(chime_mention);
			}
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

struct ccb {
	ChimeContact *contact;
	const gchar *name;
};

static void match_contact_cb(ChimeConnection *cxn, ChimeContact *contact, gpointer _d)
{
	struct ccb *d = _d;

	if (!strcmp(chime_contact_get_full_name(contact), d->name))
		d->contact = contact;
}

static void open_participant_im(PurpleConnection *conn, GList *row, gpointer _chat)
{
	ChimeConnection *cxn = PURPLE_CHIME_CXN(conn);
	struct ccb ccb = { NULL, row->data };

	chime_connection_foreach_contact(cxn, match_contact_cb, &ccb);
	if (ccb.contact) {
		PurpleConversation *pconv = purple_conversation_new(PURPLE_CONV_TYPE_IM,
								    purple_connection_get_account(conn),
								    chime_contact_get_email(ccb.contact));
		purple_conversation_present(pconv);
	}
}

static PurpleNotifySearchResults *generate_sr_participants(GHashTable *participants)
{
	PurpleNotifySearchResults *results = purple_notify_searchresults_new();
	PurpleNotifySearchColumn *column;

	column = purple_notify_searchresults_column_new(_("Name"));
	purple_notify_searchresults_column_add(results, column);
	column = purple_notify_searchresults_column_new(_("Status"));
	purple_notify_searchresults_column_add(results, column);
	column = purple_notify_searchresults_column_new("🗔");
	purple_notify_searchresults_column_add(results, column);
	column = purple_notify_searchresults_column_new("🔊");
	purple_notify_searchresults_column_add(results, column);

	purple_notify_searchresults_button_add(results, PURPLE_NOTIFY_BUTTON_IM, open_participant_im);

	gpointer klass = g_type_class_ref(CHIME_TYPE_CALL_PARTICIPATION_STATUS);

	GList *pl = g_hash_table_get_values(participants);
	pl = g_list_sort(pl, participant_sort);
	while (pl) {
		ChimeCallParticipant *p = pl->data;
		GList *row = NULL;
		row = g_list_append(row, g_strdup(p->full_name));
		GEnumValue *val = g_enum_get_value(klass, p->status);
		row = g_list_append(row, g_strdup(_(val->value_nick)));

		const gchar *screen_icon;
		if (p->shared_screen == CHIME_SHARED_SCREEN_VIEWING)
			screen_icon = "👁";
		else if (p->shared_screen == CHIME_SHARED_SCREEN_PRESENTING)
			screen_icon = "🗔";
		else
			screen_icon = "";
		row = g_list_append(row, g_strdup(screen_icon));

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
	purple_debug(PURPLE_DEBUG_INFO, "chime", "Call stream info %d\n", type);

	if (type == PURPLE_MEDIA_INFO_MUTE) {
		chime_call_set_local_mute(chat->call, TRUE);
	} else if (type == PURPLE_MEDIA_INFO_UNMUTE) {
		chime_call_set_local_mute(chat->call, FALSE);
	}
}

static void call_media_changed(PurpleMedia *media, PurpleMediaState state, const gchar *id, const gchar *participant, struct chime_chat *chat)
{
	purple_debug(PURPLE_DEBUG_INFO, "chime", "Call media state %d\n", state);

	if (state == PURPLE_MEDIA_STATE_CONNECTED) {
		PurpleMediaManager *mgr = purple_media_manager_get();
		GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(purple_media_manager_get_pipeline(mgr)), GST_DEBUG_GRAPH_SHOW_ALL, "chime connected");
	}

	if (state == PURPLE_MEDIA_STATE_END && !id && !participant) {
		if (chat->media) {
			chat->media = NULL;
			chat->media_connected = FALSE;
			chime_call_set_silent(chat->call, TRUE);
		}
	}
}


/* Set up Pidgin media streams while it's connecting... */
static void call_media_setup(ChimeCall *call, struct chime_chat *chat)
{
	const gchar *name = chime_call_get_alert_body(call);

	PurpleMediaManager *mgr = purple_media_manager_get();
	chat->media = purple_media_manager_create_media(purple_media_manager_get(),
							chat->conv->account,
							"fsrtpconference",
							name,
							TRUE);
	if (!chat->media) {
		/* XX: Report error, but not with purple_media_error()! */
		chime_call_set_silent(chat->call, TRUE);
		return;
	}
	chat->media_connected = FALSE;

	g_signal_connect(chat->media, "state-changed", G_CALLBACK(call_media_changed), chat);
	g_signal_connect(chat->media, "stream-info", G_CALLBACK(call_stream_info), chat);

	if (!purple_media_add_stream(chat->media, "chime", name,
				     PURPLE_MEDIA_AUDIO, TRUE,
				     "app", 0, NULL)) {

		purple_media_error(chat->media, _("Error adding media stream\n"));
		purple_media_end(chat->media, NULL, NULL);
		chat->media = NULL;
		chime_call_set_silent(chat->call, TRUE);
		return;
	}

	gchar *srcname = g_strdup_printf("chime_src_%p", call);
	gchar *sinkname = g_strdup_printf("chime_sink_%p", call);
	gchar *srcpipe = g_strdup_printf("appsrc name=%s format=time do-timestamp=TRUE is-live=TRUE", srcname);
	gchar *sinkpipe = g_strdup_printf("appsink name=%s async=false", sinkname);

	PurpleMediaCandidate *cand =
		purple_media_candidate_new(NULL, 1,
					   PURPLE_MEDIA_CANDIDATE_TYPE_HOST,
					   PURPLE_MEDIA_NETWORK_PROTOCOL_UDP,
					   sinkpipe, 16000);
	g_object_set(cand, "username", srcpipe, NULL);
	g_free(sinkpipe);
	g_free(srcpipe);

	GList *cands = g_list_append (NULL, cand);
	purple_media_add_remote_candidates(chat->media, "chime", name, cands);
	purple_media_candidate_list_free(cands);

	GList *codecs = g_list_append(NULL,
				      purple_media_codec_new(97, "CHIME", PURPLE_MEDIA_AUDIO, 0));
	g_object_set(codecs->data, "channels", 1, NULL);

	if (!purple_media_set_remote_codecs(chat->media, "chime", name, codecs)) {
		purple_media_codec_list_free(codecs);
		purple_media_error(chat->media, _("Error setting Chime OPUS codec\n"));
		purple_media_end(chat->media, NULL, NULL);
		chat->media = NULL;
		chime_call_set_silent(chat->call, TRUE);
		return;
	}
	purple_media_codec_list_free(codecs);

	GstElement *pipeline = purple_media_manager_get_pipeline(mgr);
	GstElement *appsrc = gst_bin_get_by_name(GST_BIN(pipeline), srcname);
	GstElement *appsink = gst_bin_get_by_name(GST_BIN(pipeline), sinkname);
	g_free(srcname);
	g_free(sinkname);

	gst_app_src_set_size(GST_APP_SRC(appsrc), -1);
	gst_app_src_set_max_bytes(GST_APP_SRC(appsrc), 100);
	gst_app_src_set_stream_type(GST_APP_SRC(appsrc), GST_APP_STREAM_TYPE_STREAM);
	chime_call_install_gst_app_callbacks(chat->call, GST_APP_SRC(appsrc), GST_APP_SINK(appsink));
	g_object_unref(appsrc);
	g_object_unref(appsink);

	GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(purple_media_manager_get_pipeline(mgr)),
				  GST_DEBUG_GRAPH_SHOW_ALL, "chime conference graph");
}

static void on_audio_state(ChimeCall *call, ChimeAudioState audio_state, struct chime_chat *chat)
{
	purple_debug(PURPLE_DEBUG_INFO, "chime", "Audio state %d\n", audio_state);

	const gchar *name = chime_call_get_alert_body(chat->call);
	if (audio_state == CHIME_AUDIO_STATE_CONNECTING && !chat->media &&
	    !chime_call_get_silent(call)) {
		call_media_setup(call, chat);
	} else if (audio_state == CHIME_AUDIO_STATE_AUDIO_MUTED && chat->media) {
		purple_media_stream_info(chat->media, PURPLE_MEDIA_INFO_MUTE, "chime", name, FALSE);
	} else if (audio_state == CHIME_AUDIO_STATE_AUDIO && chat->media) {
		if (!chat->media_connected) {
			chat->media_connected = TRUE;
			purple_media_stream_info(chat->media, PURPLE_MEDIA_INFO_ACCEPT, "chime", name, FALSE);
		}
		purple_media_stream_info(chat->media, PURPLE_MEDIA_INFO_UNMUTE, "chime", name, FALSE);
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

static void on_screen_state(ChimeCall *call, ChimeScreenState screen_state, struct chime_chat *chat)
{
	purple_debug(PURPLE_DEBUG_INFO, "chime", "Screen state %d\n", screen_state);

	if (screen_state == CHIME_SCREEN_STATE_CONNECTING)
		return;

	if (chat->share_media) {
		if (screen_state == CHIME_SCREEN_STATE_SENDING) {
			purple_media_stream_info(chat->share_media, PURPLE_MEDIA_INFO_ACCEPT, "chime", _("Sharing screen"), FALSE);
		} else {
			purple_debug(PURPLE_DEBUG_INFO, "chime", "Screen presentation ends\n");
			purple_media_end(chat->share_media, NULL, NULL);
			chat->share_media = NULL;
		}
	} else if (chat->screen_media) {
		if (screen_state == CHIME_SCREEN_STATE_VIEWING) {
			purple_media_stream_info(chat->screen_media, PURPLE_MEDIA_INFO_ACCEPT, "chime", chat->screen_title, FALSE);
		} else {
			purple_debug(PURPLE_DEBUG_INFO, "chime", "Screen viewing ends\n");
			purple_media_end(chat->screen_media, NULL, NULL);
			chat->screen_media = NULL;
		}
	}
}

static void share_media_changed(PurpleMedia *media, PurpleMediaState state, const gchar *id, const gchar *participant, struct chime_chat *chat)
{
	purple_debug(PURPLE_DEBUG_INFO, "chime", "Share media state %d\n", state);

	if (state == PURPLE_MEDIA_STATE_CONNECTED) {
		PurpleMediaManager *mgr = purple_media_manager_get();
		GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(purple_media_manager_get_pipeline(mgr)), GST_DEBUG_GRAPH_SHOW_ALL, "share connected");
	}

	if (state == PURPLE_MEDIA_STATE_END && !id && !participant) {
		if (chat->share_media) {
			chat->share_media = NULL;
		}
	}
}

static void on_call_presenter(ChimeCall *call, ChimeCallParticipant *presenter, struct chime_chat *chat);


static void share_screen(gpointer _chat, PurpleMediaElementInfo *info)
{
	struct chime_chat *chat = _chat;

	chat->share_select_ui = NULL;

	if (!info)
		return;

	g_clear_object(&chat->screen_src_info);
	chat->screen_src_info = info;

	/* Stop receiving so we can send */
	on_call_presenter(chat->call, NULL, chat);

	const gchar *name = _("Sharing screen");

	PurpleMediaManager *mgr = purple_media_manager_get();
	chat->share_media = purple_media_manager_create_media(purple_media_manager_get(),
							chat->conv->account,
							"fsrawconference",
							name,
							TRUE);
	if (!chat->share_media) {
		/* XX: Report error, but not with purple_media_error()! */
		return;
	}

	g_object_set_data(G_OBJECT(chat->share_media), "src-element", chat->screen_src_info);

	g_signal_connect(chat->share_media, "state-changed", G_CALLBACK(share_media_changed), chat);

	if (!purple_media_add_stream(chat->share_media, "chime", name,
				     PURPLE_MEDIA_SEND_VIDEO, TRUE,
				     "app", 0, NULL)) {

		purple_media_error(chat->share_media, _("Error adding media stream\n"));
		purple_media_end(chat->share_media, NULL, NULL);
		chat->share_media = NULL;
		return;
	}

	gchar *sinkname = g_strdup_printf("chime_screen_sink_%p", chat->call);
	gchar *sinkpipe = g_strdup_printf("videorate ! video/x-raw,framerate=3/1 ! videoconvert ! vp8enc min-quantizer=15 max-quantizer=25 target-bitrate=256000 deadline=1 ! appsink name=%s async=false", sinkname);
	PurpleMediaCandidate *cand =
		purple_media_candidate_new(NULL, 1,
					   PURPLE_MEDIA_CANDIDATE_TYPE_HOST,
					   PURPLE_MEDIA_NETWORK_PROTOCOL_UDP,
					   sinkpipe, 16000);
	g_free(sinkpipe);

	GList *cands = g_list_append (NULL, cand);
	purple_media_add_remote_candidates(chat->share_media, "chime", name, cands);
	purple_media_candidate_list_free(cands);

	GList *codecs = g_list_append(NULL,
				      purple_media_codec_new(97, "video/x-raw", PURPLE_MEDIA_VIDEO, 0));

	if (!purple_media_set_remote_codecs(chat->share_media, "chime", name, codecs)) {
		purple_media_codec_list_free(codecs);
		purple_media_error(chat->share_media, _("Error setting video codec\n"));
		purple_media_end(chat->share_media, NULL, NULL);
		chat->share_media = NULL;
		return;
	}

	purple_media_codec_list_free(codecs);

	GstElement *pipeline = purple_media_manager_get_pipeline(mgr);
	GstElement *appsink = gst_bin_get_by_name(GST_BIN(pipeline), sinkname);
	g_free(sinkname);

	chime_call_send_screen(PURPLE_CHIME_CXN(chat->conv->account->gc), chat->call, GST_APP_SINK(appsink));
	g_object_unref(appsink);

	GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(purple_media_manager_get_pipeline(mgr)),
				  GST_DEBUG_GRAPH_SHOW_ALL, "chime share graph");
}

static void select_screen_share(PurpleBuddy *buddy, gpointer _chat)
{
	struct chime_chat *chat = _chat;

	if (chat->share_media || chat->share_select_ui)
		return;

	char *secondary = g_strdup_printf(_("Select the window to share to %s"),
					  chat->conv->name);
	chat->share_select_ui = purple_request_screenshare_media(chat->conv->account->gc, _("Select screenshare"),
								 NULL, secondary, chat->conv->account,
								 (GCallback)share_screen, chat);
}

static void screen_media_changed(PurpleMedia *media, PurpleMediaState state, const gchar *id, const gchar *participant, struct chime_chat *chat)
{
	purple_debug(PURPLE_DEBUG_INFO, "chime", "Share media state %d\n", state);

	if (state == PURPLE_MEDIA_STATE_CONNECTED) {
		PurpleMediaManager *mgr = purple_media_manager_get();
		GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(purple_media_manager_get_pipeline(mgr)), GST_DEBUG_GRAPH_SHOW_ALL, "share connected");
	}

	if (state == PURPLE_MEDIA_STATE_END && !id && !participant) {
		if (chat->screen_media) {
			chat->screen_media = NULL;
		}
	}
}

static void screen_ask_cb(gpointer _chat, int choice)
{
	struct chime_chat *chat = _chat;

	chat->screen_ask_ui = NULL;
	if (choice)
		return;

	const gchar *name = chat->screen_title;

	PurpleMediaManager *mgr = purple_media_manager_get();
	chat->screen_media = purple_media_manager_create_media(purple_media_manager_get(),
							chat->conv->account,
							"fsrawconference",
							name,
							TRUE);
	if (!chat->screen_media) {
		/* XX: Report error, but not with purple_media_error()! */
		return;
	}

	g_signal_connect(chat->screen_media, "state-changed", G_CALLBACK(screen_media_changed), chat);

	if (!purple_media_add_stream(chat->screen_media, "chime", name,
				     PURPLE_MEDIA_RECV_VIDEO, TRUE,
				     "app", 0, NULL)) {

		purple_media_error(chat->screen_media, _("Error adding media stream\n"));
		purple_media_end(chat->screen_media, NULL, NULL);
		chat->screen_media = NULL;
		return;
	}

	gchar *srcname = g_strdup_printf("chime_screen_src_%p", chat->call);
	gchar *srcpipe = g_strdup_printf("appsrc name=%s format=time do-timestamp=TRUE is-live=TRUE caps=video/x-vp8,width=1,height=1,framerate=30/1 ! vp8dec ! videoconvert", srcname);
	PurpleMediaCandidate *cand =
		purple_media_candidate_new(NULL, 1,
					   PURPLE_MEDIA_CANDIDATE_TYPE_HOST,
					   PURPLE_MEDIA_NETWORK_PROTOCOL_UDP,
					   NULL, 16000);
	g_object_set(cand, "username", srcpipe, NULL);
	g_free(srcpipe);

	GList *cands = g_list_append (NULL, cand);
	purple_media_add_remote_candidates(chat->screen_media, "chime", name, cands);
	purple_media_candidate_list_free(cands);

	GList *codecs = g_list_append(NULL,
				      purple_media_codec_new(97, "video/x-raw", PURPLE_MEDIA_VIDEO, 0));

	if (!purple_media_set_remote_codecs(chat->screen_media, "chime", name, codecs)) {
		purple_media_codec_list_free(codecs);
		purple_media_error(chat->screen_media, _("Error setting video codec\n"));
		purple_media_end(chat->screen_media, NULL, NULL);
		chat->screen_media = NULL;
		return;
	}
	purple_media_codec_list_free(codecs);

	GstElement *pipeline = purple_media_manager_get_pipeline(mgr);
	GstElement *appsrc = gst_bin_get_by_name(GST_BIN(pipeline), srcname);
	g_free(srcname);

	gst_app_src_set_size(GST_APP_SRC(appsrc), -1);
	//	gst_app_src_set_max_bytes(GST_APP_SRC(appsrc), 100);
	gst_app_src_set_stream_type(GST_APP_SRC(appsrc), GST_APP_STREAM_TYPE_STREAM);
	chime_call_view_screen(PURPLE_CHIME_CXN(chat->conv->account->gc), chat->call, GST_APP_SRC(appsrc));
	g_object_unref(appsrc);

	GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(purple_media_manager_get_pipeline(mgr)),
				  GST_DEBUG_GRAPH_SHOW_ALL, "chime screen graph");
}

static void view_screen(PurpleBuddy *buddy, gpointer _chat)
{
	struct chime_chat *chat = _chat;

	if (chat->screen_ask_ui) {
		purple_request_close(PURPLE_REQUEST_ACTION, chat->screen_ask_ui);
		chat->screen_ask_ui = NULL;
	}
	screen_ask_cb(chat, 0);
}

static void on_call_presenter(ChimeCall *call, ChimeCallParticipant *presenter, struct chime_chat *chat)
{
	/* If we are the sender, don't offer to receive... */
	if (chat->share_media && presenter &&
	    !g_strcmp0(presenter->participant_id, chime_connection_get_profile_id(PURPLE_CHIME_CXN(chat->conv->account->gc))))
		presenter = NULL;

	if (!presenter || g_strcmp0(chat->presenter_id, presenter->participant_id)) {
		if (chat->screen_ask_ui) {
			purple_request_close(PURPLE_REQUEST_ACTION, chat->screen_ask_ui);
			chat->screen_ask_ui = NULL;
		}

		PurpleMedia *media = chat->screen_media;
		if (media) {
			chat->screen_media = NULL;
			purple_media_end(media, "chime", chat->screen_title);
			purple_media_manager_remove_media(purple_media_manager_get(), media);
		}
		g_free(chat->presenter_id);
		g_free(chat->screen_title);
		chat->screen_title = chat->presenter_id = NULL;
	}
	if (presenter) {
		purple_debug(PURPLE_DEBUG_INFO, "chime", "New presenter %s\n", presenter->full_name);
		chat->presenter_id = g_strdup(presenter->participant_id);
		chat->screen_title = g_strdup_printf(_("%s's screen"), presenter->full_name);

		gchar *primary = g_strdup_printf(_("%s is now sharing a screen."), presenter->full_name);
		chat->screen_ask_ui = purple_request_action(chat, _("Screenshare available"), primary,
							    chime_call_get_alert_body(chat->call), 1,
							    chat->conv->account, presenter->email, chat->conv,
							    chat, 2,
							    _("Ignore"), screen_ask_cb,
							    _("View"), screen_ask_cb);
		g_free(primary);
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

	if (chat->call)
		on_call_presenter(chat->call, NULL, chat);

	if (chat->meeting) {
		if (chat->participants_ui) {
			purple_notify_close(PURPLE_NOTIFY_SEARCHRESULTS, chat->participants_ui);
			chat->participants_ui = NULL;
		}

		g_signal_handlers_disconnect_matched(chat->call, G_SIGNAL_MATCH_DATA,
						     0, 0, NULL, NULL, chat);

		if (chat->media) {
			PurpleMedia *media = chat->media;
			chat->media = NULL;

			purple_media_end(media, "chime", chime_call_get_alert_body(chat->call));
			purple_media_manager_remove_media(purple_media_manager_get(), media);
		}

		if (chat->share_media) {
			PurpleMedia *media = chat->share_media;
			chat->share_media = NULL;

			purple_media_end(media, "chime", _("Sharing screen"));
			purple_media_manager_remove_media(purple_media_manager_get(), media);
		}
		g_clear_object(&chat->screen_src_info);

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
	if (chat) {
		purple_conversation_present(chat->conv);
		return chat;
	}

	chat = g_new0(struct chime_chat, 1);

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

	if (meeting) {
		chat->meeting = g_object_ref(meeting);
		chat->call = chime_meeting_get_call(meeting);
		if (chat->call) {
			g_signal_connect(chat->call, "screen-state", G_CALLBACK(on_screen_state), chat);
			g_signal_connect(chat->call, "audio-state", G_CALLBACK(on_audio_state), chat);
			g_signal_connect(chat->call, "participants-changed", G_CALLBACK(on_call_participants), chat);
			g_signal_connect(chat->call, "new-presenter", G_CALLBACK(on_call_presenter), chat);

			/* We'll probably miss the first audio-state signal when it
			 * starts connecting. Set up the call media now if needed. */
			if (!chime_call_get_silent(chat->call))
				call_media_setup(chat->call, chat);
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

void purple_chime_init_chats(PurpleConnection *conn)
{
	struct purple_chime *pc = purple_connection_get_protocol_data(conn);

	pc->live_chats = g_hash_table_new(g_direct_hash, g_direct_equal);
	pc->chats_by_room = g_hash_table_new(g_direct_hash, g_direct_equal);

	pc->mention_regex = g_regex_new(MENTION_PATTERN, G_REGEX_EXTENDED, 0, NULL);

}

void purple_chime_destroy_chats(PurpleConnection *conn)
{
	struct purple_chime *pc = purple_connection_get_protocol_data(conn);
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
		chime_call_set_silent(chat->call, FALSE);
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
		if (chat->screen_title)
			items = g_list_append(items,
					      purple_menu_action_new(chat->screen_title,
								     PURPLE_CALLBACK(view_screen), chat, NULL));
		items = g_list_append(items,
				      purple_menu_action_new(_("Share screen (well, camera)"),
							     PURPLE_CALLBACK(select_screen_share), chat, NULL));
	}

	return items;
}

char *chime_purple_get_cb_alias(PurpleConnection *conn, int id, const gchar *who)
{
	ChimeContact *contact = chime_connection_contact_by_email(PURPLE_CHIME_CXN(conn), who);

	if (contact)
		return g_strdup(chime_contact_get_display_name(contact));
	else
		return NULL;
}
