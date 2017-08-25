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
	PROP_PASSCODE,
	PROP_START_AT,
	PROP_CHANNEL,
	PROP_ROSTER_CHANNEL,
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
       CHIME_ENUM_VALUE(CHIME_MEETING_TYPE_CONFERENCE_BRIDGE,	"ConferenceBridge")	\
       CHIME_ENUM_VALUE(CHIME_MEETING_TYPE_WEBINAR,		"Webinar"))

static void close_meeting(gpointer key, gpointer val, gpointer data);

static void
chime_meeting_dispose(GObject *object)
{
	ChimeMeeting *self = CHIME_MEETING(object);

	chime_debug("Meeting disposed: %p\n", self);

	close_meeting(NULL, self, NULL);

	G_OBJECT_CLASS(chime_meeting_parent_class)->dispose(object);
}

static void
chime_meeting_finalize(GObject *object)
{
	ChimeMeeting *self = CHIME_MEETING(object);

	g_free(self->passcode);
	g_free(self->channel);
	g_free(self->roster_channel);
	g_free(self->start_at);
	//	g_object_unref(self->organiser);

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

	return chime_object_get_id(CHIME_OBJECT(self));;
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
	const gchar *id, *name, *passcode, *channel, *roster_channel, *start_at;
	ChimeMeetingType type;
	JsonObject *obj = json_node_get_object(node);
	JsonNode *call_node = json_object_get_member(obj, "call");
	if (!call_node)
		goto eparse;
	if (!parse_string(node, "id", &id) ||
	    !parse_string(node, "summary", &name) ||
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

	node = json_object_get_member(obj, "organizer");
	if (!node)
		goto eparse;

	/* We have to make ChimeContact tolerate the absence of a presence channel first... */
	//ChimeContact *organizer = chime_connection_parse_contact(cxn, NULL, node, NULL);

	ChimeMeeting *meeting = g_hash_table_lookup(priv->meetings.by_id, id);
	if (!meeting) {
		meeting = g_object_new(CHIME_TYPE_MEETING,
				       "id", id,
				       "name", name,
				       "type", type,
				       "start-at", start_at,
				       "passcode", passcode,
				       "channel", channel,
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
	if (passcode && g_strcmp0(passcode, meeting->passcode)) {
		g_free(meeting->passcode);
		meeting->passcode = g_strdup(passcode);
		g_object_notify(G_OBJECT(meeting), "passcode");
	}
	if (start_at && g_strcmp0(start_at, meeting->start_at)) {
		g_free(meeting->start_at);
		meeting->start_at = g_strdup(start_at);
		g_object_notify(G_OBJECT(meeting), "start_at");
	}
	if (channel && g_strcmp0(channel, meeting->channel)) {
		g_free(meeting->channel);
		meeting->channel = g_strdup(channel);
		g_object_notify(G_OBJECT(meeting), "channel");
	}
	if (roster_channel && g_strcmp0(roster_channel, meeting->roster_channel)) {
		g_free(meeting->roster_channel);
		meeting->roster_channel = g_strdup(roster_channel);
		g_object_notify(G_OBJECT(meeting), "roster_channel");
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
	chime_jugg_subscribe(cxn, priv->device_channel, "ConferenceBridge",
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
	chime_jugg_unsubscribe(cxn, priv->device_channel, "ConferenceBridge",
			     meeting_jugg_cb, NULL);
	chime_jugg_unsubscribe(cxn, priv->device_channel, "Webinar",
			     meeting_jugg_cb, NULL);

	if (priv->meetings.by_id)
		g_hash_table_foreach(priv->meetings.by_id, close_meeting, NULL);

	chime_object_collection_destroy(&priv->meetings);
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

gboolean chime_connection_open_meeting(ChimeConnection *cxn, ChimeMeeting *meeting)
{
	g_return_val_if_fail(CHIME_IS_CONNECTION(cxn), FALSE);
	g_return_val_if_fail(CHIME_IS_MEETING(meeting), FALSE);

	if (!meeting->opens++) {
		meeting->cxn = cxn;
		chime_jugg_subscribe(cxn, meeting->channel, NULL, NULL, NULL);
		chime_jugg_subscribe(cxn, meeting->roster_channel, NULL, NULL, NULL);
	}

	return TRUE;
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


static void schedule_meeting_cb(ChimeConnection *cxn, SoupMessage *msg,
				JsonNode *node, gpointer user_data)
{
	GTask *task = G_TASK(user_data);

	if (SOUP_STATUS_IS_SUCCESSFUL(msg->status_code) && node)
		g_task_return_pointer(task, json_node_ref(node), (GDestroyNotify)json_node_unref);
	else {
		const gchar *reason = msg->reason_phrase;

		if (node)
			parse_string(node, "Message", &reason);

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

JsonNode *chime_connection_meeting_schedule_info_finish(ChimeConnection *self,
							GAsyncResult *result,
							GError **error)
{
	g_return_val_if_fail(CHIME_IS_CONNECTION(self), FALSE);
	g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

	return g_task_propagate_pointer(G_TASK(result), error);
}

