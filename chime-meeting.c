/*
 * Pidgin/libpurple Chime client plugin
 *
 * Copyright © 2017 Amazon.com, Inc. or its affiliates.
 *
 * Authors: David Woodhouse <dwmw2@infradead.org>
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


#include "chime-connection-private.h"
#include "chime-meeting.h"

#include <glib/gi18n.h>

enum
{
	PROP_0,
	PROP_TYPE,
	PROP_JOINABLE,
	PROP_NOISY,
	PROP_ONGOING,
	PROP_CHAT_ROOM_ID,
	PROP_INTERNATIONAL_DIALIN_INFO_URL,
	PROP_MEETING_ID_FOR_DISPLAY,
	PROP_MEETING_JOIN_DISPLAY_NAME_URL,
	PROP_MEETING_JOIN_URL,
	PROP_SCREEN_SHARE_URL,
	PROP_PASSCODE,
	PROP_START_AT,
	PROP_CHANNEL,
	PROP_ROSTER_CHANNEL,
	PROP_ORGANISER,
	LAST_PROP,
};

static GParamSpec *props[LAST_PROP];

enum {
	ENDED,
	LAST_SIGNAL,
};

static guint signals[LAST_SIGNAL];

struct _ChimeMeeting {
	ChimeObject parent_instance;

	ChimeMeetingType type;
	gboolean joinable;
	gboolean noisy;
	gboolean ongoing;

	gchar *chat_room_id;
	ChimeRoom *chat_room;

	gchar *international_dialin_info_url;
	gchar *meeting_id_for_display;
	gchar *meeting_join_display_name_url;
	gchar *meeting_join_url;
	gchar *screen_share_url;

	gchar *passcode;
	gchar *channel;
	gchar *roster_channel;
	gchar *start_at;
	ChimeContact *organiser;

	/* For open meetings */
	guint opens;
	ChimeConnection *cxn;
};

G_DEFINE_TYPE(ChimeMeeting, chime_meeting, CHIME_TYPE_OBJECT)

CHIME_DEFINE_ENUM_TYPE(ChimeMeetingType, chime_meeting_type,				\
       CHIME_ENUM_VALUE(CHIME_MEETING_TYPE_ADHOC,		"AdHocMeeting")		\
       CHIME_ENUM_VALUE(CHIME_MEETING_TYPE_GOOGLE_CALENDAR,	"GoogleCalendarMeeting")\
       CHIME_ENUM_VALUE(CHIME_MEETING_TYPE_CONFERENCE_BRIDGE,	"ConferenceBridgeMeeting") \
       CHIME_ENUM_VALUE(CHIME_MEETING_TYPE_WEBINAR,		"Webinar"))

static void close_meeting(gpointer key, gpointer val, gpointer data);

static void
chime_meeting_dispose(GObject *object)
{
	ChimeMeeting *self = CHIME_MEETING(object);

	chime_debug("Meeting disposed: %p\n", self);

	close_meeting(NULL, self, NULL);
	g_signal_emit(self, signals[ENDED], 0, NULL);

	G_OBJECT_CLASS(chime_meeting_parent_class)->dispose(object);
}

static void
chime_meeting_finalize(GObject *object)
{
	ChimeMeeting *self = CHIME_MEETING(object);

	g_free(self->chat_room_id);
	g_free(self->international_dialin_info_url);
	g_free(self->meeting_id_for_display);
	g_free(self->meeting_join_display_name_url);
	g_free(self->meeting_join_url);
	g_free(self->screen_share_url);
	g_free(self->passcode);
	g_free(self->channel);
	g_free(self->roster_channel);
	g_free(self->start_at);
	g_object_unref(self->organiser);
	if (self->chat_room)
		g_object_unref(self->chat_room);

	G_OBJECT_CLASS(chime_meeting_parent_class)->finalize(object);
}

static void chime_meeting_get_property(GObject *object, guint prop_id,
				    GValue *value, GParamSpec *pspec)
{
	ChimeMeeting *self = CHIME_MEETING(object);

	switch (prop_id) {
	case PROP_TYPE:
		g_value_set_enum(value, self->type);
		break;
	case PROP_JOINABLE:
		g_value_set_boolean(value, self->joinable);
		break;
	case PROP_NOISY:
		g_value_set_boolean(value, self->noisy);
		break;
	case PROP_ONGOING:
		g_value_set_boolean(value, self->ongoing);
		break;
	case PROP_CHAT_ROOM_ID:
		g_value_set_string(value, self->chat_room_id);
		break;
	case PROP_INTERNATIONAL_DIALIN_INFO_URL:
		g_value_set_string(value, self->international_dialin_info_url);
		break;
	case PROP_MEETING_ID_FOR_DISPLAY:
		g_value_set_string(value, self->meeting_id_for_display);
		break;
	case PROP_MEETING_JOIN_DISPLAY_NAME_URL:
		g_value_set_string(value, self->meeting_join_display_name_url);
		break;
	case PROP_MEETING_JOIN_URL:
		g_value_set_string(value, self->meeting_join_url);
		break;
	case PROP_SCREEN_SHARE_URL:
		g_value_set_string(value, self->screen_share_url);
		break;
	case PROP_PASSCODE:
		g_value_set_string(value, self->passcode);
		break;
	case PROP_START_AT:
		g_value_set_string(value, self->start_at);
		break;
	case PROP_CHANNEL:
		g_value_set_string(value, self->channel);
		break;
	case PROP_ROSTER_CHANNEL:
		g_value_set_string(value, self->roster_channel);
		break;
	case PROP_ORGANISER:
		g_value_set_object(value, self->organiser);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void chime_meeting_set_property(GObject *object, guint prop_id,
				    const GValue *value, GParamSpec *pspec)
{
	ChimeMeeting *self = CHIME_MEETING(object);

	switch (prop_id) {
	case PROP_TYPE:
		self->type = g_value_get_enum(value);
		break;
	case PROP_JOINABLE:
		self->joinable = g_value_get_boolean(value);
		break;
	case PROP_NOISY:
		self->noisy = g_value_get_boolean(value);
		break;
	case PROP_ONGOING:
		self->ongoing = g_value_get_boolean(value);
		break;
	case PROP_CHAT_ROOM_ID:
		g_free(self->chat_room_id);
		self->chat_room_id = g_value_dup_string(value);
		break;
	case PROP_INTERNATIONAL_DIALIN_INFO_URL:
		g_free(self->international_dialin_info_url);
		self->international_dialin_info_url = g_value_dup_string(value);
		break;
	case PROP_MEETING_ID_FOR_DISPLAY:
		g_free(self->meeting_id_for_display);
		self->meeting_id_for_display = g_value_dup_string(value);
		break;
	case PROP_MEETING_JOIN_DISPLAY_NAME_URL:
		g_free(self->meeting_join_display_name_url);
		self->meeting_join_display_name_url = g_value_dup_string(value);
		break;
	case PROP_MEETING_JOIN_URL:
		g_free(self->meeting_join_url);
		self->meeting_join_url = g_value_dup_string(value);
		break;
	case PROP_SCREEN_SHARE_URL:
		g_free(self->screen_share_url);
		self->screen_share_url = g_value_dup_string(value);
		break;
	case PROP_PASSCODE:
		g_free(self->passcode);
		self->passcode = g_value_dup_string(value);
		break;
	case PROP_START_AT:
		g_free(self->start_at);
		self->start_at = g_value_dup_string(value);
		break;
	case PROP_CHANNEL:
		g_free(self->channel);
		self->channel = g_value_dup_string(value);
		break;
	case PROP_ROSTER_CHANNEL:
		g_free(self->roster_channel);
		self->roster_channel = g_value_dup_string(value);
		break;
	case PROP_ORGANISER:
		g_return_if_fail (self->organiser == NULL);
		self->organiser = g_value_dup_object(value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void chime_meeting_class_init(ChimeMeetingClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->finalize = chime_meeting_finalize;
	object_class->dispose = chime_meeting_dispose;
	object_class->get_property = chime_meeting_get_property;
	object_class->set_property = chime_meeting_set_property;

	props[PROP_TYPE] =
		g_param_spec_enum("type",
				  "type",
				  "type",
				  CHIME_TYPE_MEETING_TYPE,
				  CHIME_MEETING_TYPE_ADHOC,
				  G_PARAM_READWRITE |
				  G_PARAM_CONSTRUCT |
				  G_PARAM_STATIC_STRINGS);

	props[PROP_JOINABLE] =
		g_param_spec_boolean("joinable",
				     "joinable",
				     "joinable",
				     FALSE,
				     G_PARAM_READWRITE |
				     G_PARAM_CONSTRUCT |
				     G_PARAM_STATIC_STRINGS);

	props[PROP_NOISY] =
		g_param_spec_boolean("noisy",
				     "noisy",
				     "noisy",
				     FALSE,
				     G_PARAM_READWRITE |
				     G_PARAM_CONSTRUCT |
				     G_PARAM_STATIC_STRINGS);

	props[PROP_ONGOING] =
		g_param_spec_boolean("ongoing",
				     "ongoing",
				     "ongoing",
				     FALSE,
				     G_PARAM_READWRITE |
				     G_PARAM_CONSTRUCT |
				     G_PARAM_STATIC_STRINGS);

	props[PROP_CHAT_ROOM_ID] =
		g_param_spec_string("chat-room-id",
				    "chat room id",
				    "chat room id",
				    NULL,
				    G_PARAM_READWRITE |
				    G_PARAM_CONSTRUCT_ONLY |
				    G_PARAM_STATIC_STRINGS);
	props[PROP_INTERNATIONAL_DIALIN_INFO_URL] =
		g_param_spec_string("international-dialin-info-url",
				    "international dialin info url",
				    "international dialin info url",
				    NULL,
				    G_PARAM_READWRITE |
				    G_PARAM_CONSTRUCT_ONLY |
				    G_PARAM_STATIC_STRINGS);
	props[PROP_MEETING_ID_FOR_DISPLAY] =
		g_param_spec_string("meeting-id-for-display",
				    "meeting id for display",
				    "meeting id for display",
				    NULL,
				    G_PARAM_READWRITE |
				    G_PARAM_CONSTRUCT_ONLY |
				    G_PARAM_STATIC_STRINGS);
	props[PROP_MEETING_JOIN_DISPLAY_NAME_URL] =
		g_param_spec_string("meeting-join-display-name-url",
				    "meeting join display name url",
				    "meeting join display name url",
				    NULL,
				    G_PARAM_READWRITE |
				    G_PARAM_CONSTRUCT_ONLY |
				    G_PARAM_STATIC_STRINGS);
	props[PROP_MEETING_JOIN_URL] =
		g_param_spec_string("meeting-join-url",
				    "meeting join url",
				    "meeting join url",
				    NULL,
				    G_PARAM_READWRITE |
				    G_PARAM_CONSTRUCT_ONLY |
				    G_PARAM_STATIC_STRINGS);
	props[PROP_SCREEN_SHARE_URL] =
		g_param_spec_string("screen-share-url",
				    "screen share url",
				    "screen share url",
				    NULL,
				    G_PARAM_READWRITE |
				    G_PARAM_CONSTRUCT_ONLY |
				    G_PARAM_STATIC_STRINGS);
	props[PROP_PASSCODE] =
		g_param_spec_string("passcode",
				    "passcode",
				    "passcode",
				    NULL,
				    G_PARAM_READWRITE |
				    G_PARAM_CONSTRUCT_ONLY |
				    G_PARAM_STATIC_STRINGS);

	props[PROP_START_AT] =
		g_param_spec_string("start-at",
				    "start at",
				    "start at",
				    NULL,
				    G_PARAM_READWRITE |
				    G_PARAM_CONSTRUCT_ONLY |
				    G_PARAM_STATIC_STRINGS);

	props[PROP_CHANNEL] =
		g_param_spec_string("channel",
				    "channel",
				    "channel",
				    NULL,
				    G_PARAM_READWRITE |
				    G_PARAM_CONSTRUCT_ONLY |
				    G_PARAM_STATIC_STRINGS);

	props[PROP_ROSTER_CHANNEL] =
		g_param_spec_string("roster-channel",
				    "roster channel",
				    "roster channel",
				    NULL,
				    G_PARAM_READWRITE |
				    G_PARAM_CONSTRUCT_ONLY |
				    G_PARAM_STATIC_STRINGS);

	props[PROP_ORGANISER] =
		g_param_spec_object("organiser",
				    "organiser",
				    "organiser",
				    CHIME_TYPE_CONTACT,
				    G_PARAM_READWRITE |
				    G_PARAM_CONSTRUCT_ONLY |
				    G_PARAM_STATIC_STRINGS);

	g_object_class_install_properties(object_class, LAST_PROP, props);

	signals[ENDED] =
		g_signal_new ("ended",
			      G_OBJECT_CLASS_TYPE (object_class), G_SIGNAL_RUN_FIRST,
			      0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void chime_meeting_init(ChimeMeeting *self)
{
}

const gchar *chime_meeting_get_id(ChimeMeeting *self)
{
	g_return_val_if_fail(CHIME_IS_MEETING(self), NULL);

	return chime_object_get_id(CHIME_OBJECT(self));
}

const gchar *chime_meeting_get_name(ChimeMeeting *self)
{
	g_return_val_if_fail(CHIME_IS_MEETING(self), NULL);

	return chime_object_get_name(CHIME_OBJECT(self));
}

const gchar *chime_meeting_get_passcode(ChimeMeeting *self)
{
	g_return_val_if_fail(CHIME_IS_MEETING(self), FALSE);

	return self->passcode;
}

const gchar *chime_meeting_get_id_for_display(ChimeMeeting *self)
{
	g_return_val_if_fail(CHIME_IS_MEETING(self), FALSE);

	return self->meeting_id_for_display;
}

const gchar *chime_meeting_get_screen_share_url(ChimeMeeting *self)
{
	g_return_val_if_fail(CHIME_IS_MEETING(self), FALSE);

	return self->screen_share_url;
}

const gchar *chime_meeting_get_start_at(ChimeMeeting *self)
{
	g_return_val_if_fail(CHIME_IS_MEETING(self), FALSE);

	return self->start_at;
}

const gchar *chime_meeting_get_channel(ChimeMeeting *self)
{
	g_return_val_if_fail(CHIME_IS_MEETING(self), NULL);

	return self->channel;
}

const gchar *chime_meeting_get_roster_channel(ChimeMeeting *self)
{
	g_return_val_if_fail(CHIME_IS_MEETING(self), NULL);

	return self->roster_channel;
}

ChimeContact *chime_meeting_get_organiser(ChimeMeeting *self)
{
	g_return_val_if_fail(CHIME_IS_MEETING(self), NULL);

	return self->organiser;
}

ChimeRoom *chime_meeting_get_chat_room(ChimeMeeting *self)
{
	g_return_val_if_fail(CHIME_IS_MEETING(self), NULL);

	return self->chat_room;
}

static gboolean parse_meeting_type(JsonNode *node, const gchar *member, ChimeMeetingType *type)
{
	const gchar *str;

	if (!parse_string(node, member, &str))
		return FALSE;

	gpointer klass = g_type_class_ref(CHIME_TYPE_MEETING_TYPE);
	GEnumValue *val = g_enum_get_value_by_nick(klass, str);
	g_type_class_unref(klass);

	if (!val)
		return FALSE;
	*type = val->value;
	return TRUE;
}

static ChimeMeeting *chime_connection_parse_meeting(ChimeConnection *cxn, JsonNode *node,
						    GError **error)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE(cxn);
	const gchar *id, *name, *passcode, *channel, *roster_channel, *start_at,
		*chat_room_id, *international_dialin_info_url = NULL,
		*meeting_id_for_display = NULL,
		*meeting_join_display_name_url = NULL, *meeting_join_url = NULL,
		*screen_share_url = NULL;
	gboolean joinable, noisy, ongoing;
	ChimeMeetingType type;
	JsonObject *obj = json_node_get_object(node);
	JsonNode *call_node = json_object_get_member(obj, "call");
	JsonNode *chat_node = json_object_get_member(obj, "meeting_chat_room");

	if (!call_node || !chat_node)
		goto eparse;
	if (!parse_string(node, "id", &id) ||
	    !parse_string(node, "summary", &name) ||
	    !parse_boolean(node, "joinable?", &joinable) ||
	    !parse_boolean(node, "noisy?", &noisy) ||
	    !parse_boolean(node, "ongoing?", &ongoing) ||
	    !parse_string(chat_node, "id", &chat_room_id) ||
	    !parse_string(node, "passcode", &passcode) ||
	    !parse_string(call_node, "channel", &channel) ||
	    !parse_string(call_node, "roster_channel", &roster_channel) ||
	    !parse_string(node, "start_at", &start_at) ||
	    !parse_meeting_type(node, "klass", &type)) {
	eparse:
		g_set_error(error, CHIME_ERROR,
			    CHIME_ERROR_BAD_RESPONSE,
			    _("Failed to parse Meeting node"));
		return NULL;
	}

	parse_string(node, "international_dialin_info_url", &international_dialin_info_url);
	parse_string(node, "meeting_id_for_display", &meeting_id_for_display);
	parse_string(node, "meeting_join_display_name_url", &meeting_join_display_name_url);
	parse_string(node, "meeting_join_url", &meeting_join_url);
	parse_string(node, "screen_share_url", &screen_share_url);

	/* Get the personal passcode if we can */
	JsonNode *att_array_node = json_object_get_member(obj, "attendances");
	if (att_array_node) {
		JsonArray *arr = json_node_get_array(att_array_node);
		int i, len = json_array_get_length(arr);
		for (i = 0; i < len; i++) {
			JsonNode *att = json_array_get_element(arr, i);
			const gchar *profile_id;
			if (parse_string(att, "profile_id", &profile_id) &&
			    !strcmp(profile_id, priv->profile_id)) {
				parse_string(att, "passcode", &passcode);
				break;
			}
		}
	}

	JsonNode *org_node = json_object_get_member(obj, "organizer");
	if (!org_node)
		goto eparse;
	ChimeContact *organiser = chime_connection_parse_contact(cxn, FALSE,
								 org_node, NULL);
	if (!organiser)
		goto eparse;

	ChimeMeeting *meeting = g_hash_table_lookup(priv->meetings.by_id, id);
	if (!meeting) {
		meeting = g_object_new(CHIME_TYPE_MEETING,
				       "id", id,
				       "name", name,
				       "type", type,
				       "joinable", joinable,
				       "noisy", noisy,
				       "ongoing", ongoing,
				       "chat-room-id", chat_room_id,
				       "international-dialin-info-url", international_dialin_info_url,
				       "meeting-id-for-display", meeting_id_for_display,
				       "meeting-join-display-name-url", meeting_join_display_name_url,
				       "meeting-join-url", meeting_join_url,
				       "screen-share-url", screen_share_url,
				       "start-at", start_at,
				       "passcode", passcode,
				       "channel", channel,
				       "organiser", organiser,
				       "roster_channel", roster_channel,
				       NULL);

		chime_object_collection_hash_object(&priv->meetings, CHIME_OBJECT(meeting), TRUE);

		/* Emit signal on ChimeConnection to admit existence of new meeting */
		chime_connection_new_meeting(cxn, meeting);

		return meeting;
	}

	if (name && g_strcmp0(name, chime_object_get_name(CHIME_OBJECT(meeting)))) {
		chime_object_rename(CHIME_OBJECT(meeting), name);
		g_object_notify(G_OBJECT(meeting), "name");
	}
	if (type != meeting->type) {
		meeting->type = type;
		g_object_notify(G_OBJECT(meeting), "type");
	}
	if (joinable != meeting->joinable) {
		meeting->joinable = joinable;
		g_object_notify(G_OBJECT(meeting), "joinable");
	}
	if (noisy != meeting->noisy) {
		meeting->noisy = noisy;
		g_object_notify(G_OBJECT(meeting), "noisy");
	}
	if (ongoing != meeting->ongoing) {
		meeting->ongoing = ongoing;
		g_object_notify(G_OBJECT(meeting), "ongoing");
	}
	if (chat_room_id && g_strcmp0(chat_room_id, meeting->chat_room_id)) {
		g_free(meeting->chat_room_id);
		meeting->chat_room_id = g_strdup(chat_room_id);
		g_object_notify(G_OBJECT(meeting), "chat-room-id");
	}
	if (international_dialin_info_url && g_strcmp0(international_dialin_info_url, meeting->international_dialin_info_url)) {
		g_free(meeting->international_dialin_info_url);
		meeting->international_dialin_info_url = g_strdup(international_dialin_info_url);
		g_object_notify(G_OBJECT(meeting), "international-dialin-info-url");
	}
	if (meeting_id_for_display && g_strcmp0(meeting_id_for_display, meeting->meeting_id_for_display)) {
		g_free(meeting->meeting_id_for_display);
		meeting->meeting_id_for_display = g_strdup(meeting_id_for_display);
		g_object_notify(G_OBJECT(meeting), "meeting-id-for-display");
	}
	if (meeting_join_display_name_url && g_strcmp0(meeting_join_display_name_url, meeting->meeting_join_display_name_url)) {
		g_free(meeting->meeting_join_display_name_url);
		meeting->meeting_join_display_name_url = g_strdup(meeting_join_display_name_url);
		g_object_notify(G_OBJECT(meeting), "meeting-join-display-name-url");
	}
	if (meeting_join_url && g_strcmp0(meeting_join_url, meeting->meeting_join_url)) {
		g_free(meeting->meeting_join_url);
		meeting->meeting_join_url = g_strdup(meeting_join_url);
		g_object_notify(G_OBJECT(meeting), "meeting-join-url");
	}
	if (screen_share_url && g_strcmp0(screen_share_url, meeting->screen_share_url)) {
		g_free(meeting->screen_share_url);
		meeting->screen_share_url = g_strdup(screen_share_url);
		g_object_notify(G_OBJECT(meeting), "screen-share-url");
	}
	if (passcode && g_strcmp0(passcode, meeting->passcode) &&
	    (!meeting->passcode || !g_str_has_prefix(meeting->passcode, passcode))) {
		g_free(meeting->passcode);
		meeting->passcode = g_strdup(passcode);
		g_object_notify(G_OBJECT(meeting), "passcode");
	}
	if (start_at && g_strcmp0(start_at, meeting->start_at)) {
		g_free(meeting->start_at);
		meeting->start_at = g_strdup(start_at);
		g_object_notify(G_OBJECT(meeting), "start-at");
	}
	if (channel && g_strcmp0(channel, meeting->channel)) {
		g_free(meeting->channel);
		meeting->channel = g_strdup(channel);
		g_object_notify(G_OBJECT(meeting), "channel");
	}
	if (roster_channel && g_strcmp0(roster_channel, meeting->roster_channel)) {
		g_free(meeting->roster_channel);
		meeting->roster_channel = g_strdup(roster_channel);
		g_object_notify(G_OBJECT(meeting), "roster-channel");
	}
	if (organiser && organiser != meeting->organiser) {
		g_object_unref(meeting->organiser);
		meeting->organiser = organiser;
		g_object_notify(G_OBJECT(meeting), "organiser");
	}

	chime_object_collection_hash_object(&priv->meetings, CHIME_OBJECT(meeting), TRUE);

	return meeting;
}

static void meetings_cb(ChimeConnection *cxn, SoupMessage *msg, JsonNode *node,
			gpointer _unused)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	if (SOUP_STATUS_IS_SUCCESSFUL(msg->status_code) && node) {
		JsonArray *arr = json_node_get_array(node);
		guint i, len = json_array_get_length(arr);

		for (i = 0; i < len; i++) {
			chime_connection_parse_meeting(cxn,
						    json_array_get_element(arr, i),
						    NULL);
		}

		chime_object_collection_expire_outdated(&priv->meetings);

		if (!priv->meetings_online) {
			priv->meetings_online = TRUE;
			chime_connection_calculate_online(cxn);
		}
	} else {
		const gchar *reason = msg->reason_phrase;

		parse_string(node, "error", &reason);

		chime_connection_fail(cxn, CHIME_ERROR_NETWORK,
				      _("Failed to fetch meetings (%d): %s\n"),
				      msg->status_code, reason);
	}
}

static void fetch_meetings(ChimeConnection *cxn, const gchar *next_token)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	SoupURI *uri = soup_uri_new_printf(priv->conference_url, "/joinable_meetings");
	chime_connection_queue_http_request(cxn, NULL, uri, "GET", meetings_cb,
					    NULL);
}

static gboolean meeting_jugg_cb(ChimeConnection *cxn, gpointer _unused, JsonNode *data_node)
{
	JsonObject *obj = json_node_get_object(data_node);
	JsonNode *record = json_object_get_member(obj, "record");
	if (!record)
		return FALSE;

	return !!chime_connection_parse_meeting(cxn, record, NULL);
}

static gboolean joinable_meetings_jugg_cb(ChimeConnection *cxn, gpointer _unused, JsonNode *data_node)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);
	priv->meetings.generation++;

	JsonObject *obj = json_node_get_object(data_node);
	JsonNode *record = json_object_get_member(obj, "record");
	if (!record)
		return FALSE;

	obj = json_node_get_object(record);
	JsonNode *meetings = json_object_get_member(obj, "meetings");
	JsonArray *arr = json_node_get_array(meetings);
	int i, len = json_array_get_length(arr);

	for (i = 0; i < len; i++) {
		JsonNode *meet_node = json_array_get_element(arr, i);
		chime_connection_parse_meeting(cxn, meet_node, NULL);
	}

	chime_object_collection_expire_outdated(&priv->meetings);
	return TRUE;
}


void chime_init_meetings(ChimeConnection *cxn)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	chime_object_collection_init(&priv->meetings);

	chime_jugg_subscribe(cxn, priv->device_channel, "JoinableMeetings",
			     joinable_meetings_jugg_cb, NULL);
	chime_jugg_subscribe(cxn, priv->device_channel, "GoogleCalendarMeeting",
			     meeting_jugg_cb, NULL);
	chime_jugg_subscribe(cxn, priv->device_channel, "AdHocMeeting",
			     meeting_jugg_cb, NULL);
	chime_jugg_subscribe(cxn, priv->device_channel, "ConferenceBridgeMeeting",
			     meeting_jugg_cb, NULL);
	chime_jugg_subscribe(cxn, priv->device_channel, "Webinar",
			     meeting_jugg_cb, NULL);
	fetch_meetings(cxn, NULL);
}

void chime_destroy_meetings(ChimeConnection *cxn)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	chime_jugg_unsubscribe(cxn, priv->device_channel, "JoinableMeetings",
			     joinable_meetings_jugg_cb, NULL);
	chime_jugg_unsubscribe(cxn, priv->device_channel, "GoogleCalendarMeeting",
			     meeting_jugg_cb, NULL);
	chime_jugg_unsubscribe(cxn, priv->device_channel, "AdHocMeeting",
			     meeting_jugg_cb, NULL);
	chime_jugg_unsubscribe(cxn, priv->device_channel, "ConferenceBridgeMeeting",
			     meeting_jugg_cb, NULL);
	chime_jugg_unsubscribe(cxn, priv->device_channel, "Webinar",
			     meeting_jugg_cb, NULL);

	if (priv->meetings.by_id)
		g_hash_table_foreach(priv->meetings.by_id, close_meeting, NULL);

	chime_object_collection_destroy(&priv->meetings);
}

gboolean chime_meeting_match_pin(ChimeMeeting *self, const gchar *pin)
{
	return !strcmp(pin, self->passcode) ||
		!g_strcmp0(pin, self->meeting_id_for_display);
}

ChimeMeeting *chime_connection_meeting_by_name(ChimeConnection *cxn,
					 const gchar *name)
{
	g_return_val_if_fail(CHIME_IS_CONNECTION(cxn), NULL);
	g_return_val_if_fail(name, NULL);

	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	return g_hash_table_lookup(priv->meetings.by_name, name);
}

ChimeMeeting *chime_connection_meeting_by_id(ChimeConnection *cxn,
				       const gchar *id)
{
	g_return_val_if_fail(CHIME_IS_CONNECTION(cxn), NULL);
	g_return_val_if_fail(id, NULL);

	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	return g_hash_table_lookup(priv->meetings.by_id, id);
}

void chime_connection_foreach_meeting(ChimeConnection *cxn, ChimeMeetingCB cb,
				   gpointer cbdata)
{
	g_return_if_fail(CHIME_IS_CONNECTION(cxn));
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE(cxn);

	chime_object_collection_foreach_object(cxn, &priv->meetings, (ChimeObjectCB)cb, cbdata);
}

static void close_meeting(gpointer key, gpointer val, gpointer data)
{
	ChimeMeeting *meeting = CHIME_MEETING (val);

	if (meeting->cxn) {
		chime_jugg_unsubscribe(meeting->cxn, meeting->channel, NULL, NULL, NULL);
		chime_jugg_unsubscribe(meeting->cxn, meeting->roster_channel, NULL, NULL, NULL);
		meeting->cxn = NULL;
	}
}

void chime_connection_close_meeting(ChimeConnection *cxn, ChimeMeeting *meeting)
{
	g_return_if_fail(CHIME_IS_CONNECTION(cxn));
	g_return_if_fail(CHIME_IS_MEETING(meeting));
	g_return_if_fail(meeting->opens);

	if (!--meeting->opens)
		close_meeting(NULL, meeting, NULL);
}


void chime_scheduled_meeting_free(ChimeScheduledMeeting *mtg)
{
	g_slist_free_full(mtg->international_dialin_info, g_free);
	json_node_unref(mtg->_node);
}

static ChimeScheduledMeeting *parse_scheduled_meeting(JsonNode *node, GError **error)
{
	ChimeScheduledMeeting *mtg = g_new0(ChimeScheduledMeeting, 1);
	mtg->_node = json_node_ref(node);

	if (!parse_string(node, "bridge_screenshare_url", &mtg->bridge_screenshare_url) ||
	    !parse_string(node, "meeting_id_for_display", &mtg->meeting_id_for_display) ||
	    !parse_string(node, "vanity_url", &mtg->vanity_url) ||
	    !parse_string(node, "vanity_name", &mtg->vanity_name) ||
	    !parse_string(node, "meeting_join_url", &mtg->meeting_join_url) ||
	    !parse_string(node, "international_dialin_info_url", &mtg->international_dialin_info_url) ||
	    !parse_string(node, "delegate_scheduling_email", &mtg->delegate_scheduling_email) ||
	    !parse_string(node, "display_vanity_url", &mtg->display_vanity_url) ||
	    !parse_string(node, "bridge_passcode", &mtg->bridge_passcode) ||
	    !parse_string(node, "scheduling_address", &mtg->scheduling_address) ||
	    !parse_string(node, "display_vanity_url_prefix", &mtg->display_vanity_url_prefix)) {
	eparse:
		*error = g_error_new(CHIME_ERROR, CHIME_ERROR_BAD_RESPONSE,
				     _("Failed to parse scheduled meeting response"));
		chime_scheduled_meeting_free(mtg);
		return NULL;
	}
	parse_string(node, "toll_dialin", &mtg->toll_dialin);
	parse_string(node, "toll_free_dialin", &mtg->toll_free_dialin);

	JsonObject *obj = json_node_get_object(node);
	node = json_object_get_member(obj, "international_dialin_info");
	JsonArray *arr = json_node_get_array(node);
	if (!arr)
		goto eparse;

	int i, len = json_array_get_length(arr);
	for (i = len - 1; i >= 0; i--) {
		ChimeDialin *d = g_new0(ChimeDialin, 1);
		node = json_array_get_element(arr, i);

		mtg->international_dialin_info = g_slist_prepend(mtg->international_dialin_info, d);
		if (!parse_string(node, "number", &d->number) ||
		    !parse_string(node, "display_string", &d->display_string) ||
		    !parse_string(node, "country", &d->country) ||
		    !parse_string(node, "iso", &d->iso))
			goto eparse;
		parse_string(node, "toll", &d->toll);
		parse_string(node, "toll_free", &d->toll_free);
		parse_string(node, "city", &d->city);
		parse_string(node, "city_code", &d->city_code);
	}
	return mtg;
}

static void schedule_meeting_cb(ChimeConnection *cxn, SoupMessage *msg,
				JsonNode *node, gpointer user_data)
{
	GTask *task = G_TASK(user_data);

	if (SOUP_STATUS_IS_SUCCESSFUL(msg->status_code) && node) {
		GError *error = NULL;
		ChimeScheduledMeeting *mtg = parse_scheduled_meeting(node, &error);
		if (mtg)
			g_task_return_pointer(task, mtg, (GDestroyNotify)chime_scheduled_meeting_free);
		else
			g_task_return_error(task, error);
	} else {
		const gchar *reason = msg->reason_phrase;

		if (node && !parse_string(node, "Message", &reason)) {
			JsonObject *obj = json_node_get_object(node);
			node = json_object_get_member(obj, "errors");
			if (node) {
				obj = json_node_get_object(node);
				node = json_object_get_member(obj, "attendees");
			}
			if (node) {
				JsonArray *arr = json_node_get_array(node);
				if (arr && json_array_get_length(arr) > 0) {
					node = json_array_get_element(arr, 0);
					parse_string(node, "message", &reason);
				}
			}
		}

		g_task_return_new_error(task, CHIME_ERROR,
					CHIME_ERROR_NETWORK,
					_("Failed to obtain meeting PIN info: %s"),
					reason);
	}

	g_object_unref(task);
}

void chime_connection_meeting_schedule_info_async(ChimeConnection *cxn,
						  gboolean onetime,
						  GCancellable *cancellable,
						  GAsyncReadyCallback callback,
						  gpointer user_data)
{
	g_return_if_fail(CHIME_IS_CONNECTION(cxn));
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	GTask *task = g_task_new(cxn, cancellable, callback, user_data);

	SoupURI *uri = soup_uri_new_printf(priv->conference_url, "/schedule_meeting_support/%s/%s_pin_info",
					   chime_connection_get_profile_id(cxn),
					   onetime ? "onetime" : "personal");
	chime_connection_queue_http_request(cxn, NULL, uri, onetime ? "POST" : "GET", schedule_meeting_cb, task);
}

ChimeScheduledMeeting *chime_connection_meeting_schedule_info_finish(ChimeConnection *self,
								     GAsyncResult *result,
								     GError **error)
{
	g_return_val_if_fail(CHIME_IS_CONNECTION(self), FALSE);
	g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

	return g_task_propagate_pointer(G_TASK(result), error);
}

static void pin_join_cb(ChimeConnection *cxn, SoupMessage *msg,
			JsonNode *node, gpointer user_data)
{
	GTask *task = G_TASK(user_data);

	if (SOUP_STATUS_IS_SUCCESSFUL(msg->status_code) && node) {
		GError *error = NULL;
		JsonObject *obj = json_node_get_object(node);
		node = json_object_get_member(obj, "meeting");
		if (!node)
			goto eparse;

		ChimeMeeting *mtg = chime_connection_parse_meeting(cxn, node, &error);
		if (mtg)
			g_task_return_pointer(task, mtg, (GDestroyNotify)g_object_unref);
		else
			g_task_return_error(task, error);
		return;
	} else {
		const gchar *reason;
	eparse:
		reason = msg->reason_phrase;

		if (node)
			parse_string(node, "Message", &reason);

		g_task_return_new_error(task, CHIME_ERROR,
					CHIME_ERROR_NETWORK,
					_("Failed to obtain meeting details: %s"),
					reason);
	}

	g_object_unref(task);
}

void chime_connection_lookup_meeting_by_pin_async(ChimeConnection *cxn,
						  const gchar *pin,
						  GCancellable *cancellable,
						  GAsyncReadyCallback callback,
						  gpointer user_data)
{
	g_return_if_fail(CHIME_IS_CONNECTION(cxn));
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	GTask *task = g_task_new(cxn, cancellable, callback, user_data);

	JsonBuilder *jb = json_builder_new();
	jb = json_builder_begin_object(jb);
	jb = json_builder_set_member_name(jb, "pin");
	jb = json_builder_add_string_value(jb, pin);
	jb = json_builder_end_object(jb);

	JsonNode *node = json_builder_get_root(jb);
	SoupURI *uri = soup_uri_new_printf(priv->conference_url, "/pin_joins");
	chime_connection_queue_http_request(cxn, node, uri, "POST", pin_join_cb, task);
	json_node_unref(node);
	g_object_unref(jb);

}

ChimeMeeting *chime_connection_lookup_meeting_by_pin_finish(ChimeConnection *self,
							    GAsyncResult *result,
							    GError **error)
{
	g_return_val_if_fail(CHIME_IS_CONNECTION(self), FALSE);
	g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

	return g_task_propagate_pointer(G_TASK(result), error);
}

#if 0
static void get_room_cb(ChimeConnection *cxn, SoupMessage *msg,
			JsonNode *node, gpointer user_data)
{
	GTask *task = G_TASK(user_data);

	if (SOUP_STATUS_IS_SUCCESSFUL(msg->status_code) && node) {
		GError *error = NULL;
		JsonObject *obj = json_node_get_object(node);
		node = json_object_get_member(obj, "meeting");
		if (!node)
			goto eparse;

		ChimeMeeting *mtg = chime_connection_parse_meeting(cxn, node, &error);
		if (mtg)
			g_task_return_pointer(task, mtg, (GDestroyNotify)g_object_unref);
		else
			g_task_return_error(task, error);
		return;
	} else {
		const gchar *reason;
	eparse:
		reason = msg->reason_phrase;

		if (node)
			parse_string(node, "Message", &reason);

		g_task_return_new_error(task, CHIME_ERROR,
					CHIME_ERROR_NETWORK,
					_("Failed to obtain meeting details: %s"),
					reason);
	}

	g_object_unref(task);
}
#endif
static void chime_connection_open_meeting(ChimeConnection *cxn, ChimeMeeting *meeting, GTask *task)
{
	if (!meeting->opens++) {
		meeting->cxn = cxn;
		chime_jugg_subscribe(cxn, meeting->channel, NULL, NULL, NULL);
		chime_jugg_subscribe(cxn, meeting->roster_channel, NULL, NULL, NULL);
	}

	g_task_return_pointer(task, g_object_ref(meeting), g_object_unref);
	g_object_unref(task);
}


static void join_got_room(GObject *source, GAsyncResult *result, gpointer user_data)
{
	ChimeConnection *cxn = CHIME_CONNECTION(source);
	ChimeRoom *room = chime_connection_fetch_room_finish(cxn, result, NULL);
	GTask *task = G_TASK(user_data);
	ChimeMeeting *meeting = CHIME_MEETING(g_task_get_task_data(task));

	meeting->chat_room = room;

	chime_connection_open_meeting(cxn, meeting, task);
}

void chime_connection_join_meeting_async(ChimeConnection *cxn,
					 ChimeMeeting *meeting,
					 GCancellable *cancellable,
					 GAsyncReadyCallback callback,
					 gpointer user_data)
{
	g_return_if_fail(CHIME_IS_CONNECTION(cxn));

	GTask *task = g_task_new(cxn, cancellable, callback, user_data);
	g_task_set_task_data(task, g_object_ref(meeting), g_object_unref);

	if (meeting->chat_room_id) {
		ChimeRoom *room = chime_connection_room_by_id(cxn, meeting->chat_room_id);
		if (room) {
			meeting->chat_room = g_object_ref(room);
		} else {
			/* Not yet known; need to go fetch it explicitly */
			chime_connection_fetch_room_async(cxn, meeting->chat_room_id,
							  NULL, join_got_room, task);
			return;
		}
	}

	chime_connection_open_meeting(cxn, meeting, task);
}

ChimeMeeting *chime_connection_join_meeting_finish(ChimeConnection *self,
						   GAsyncResult *result,
						   GError **error)
{
	g_return_val_if_fail(CHIME_IS_CONNECTION(self), FALSE);
	g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

	return g_task_propagate_pointer(G_TASK(result), error);
}

