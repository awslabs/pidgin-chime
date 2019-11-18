/*
 * Pidgin/libpurple Chime client plugin
 *
 * Copyright © 2017 Amazon.com, Inc. or its affiliates.
 *
 * Authors: David Woodhouse <dwmw2@infradead.org>
 *          Ignacio Casal Quinteiro <qignacio@amazon.com>
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
#include "chime-room.h"

#include <glib/gi18n.h>

#define BOOL_PROPS(x)				\
	x(is_open, OPEN, "Open", "open", "open", TRUE)

#define STRING_PROPS(x)				\
	x(channel, CHANNEL, "Channel", "channel", "channel", TRUE) \
	x(created_on, CREATED_ON, "CreatedOn", "created-on", "created on", TRUE) \
	x(updated_on, UPDATED_ON, "UpdatedOn", "updated-on", "updated on", TRUE) \
	x(last_sent, LAST_SENT, "LastSent", "last-sent", "last sent", FALSE) \
	x(last_read, LAST_READ, "LastRead", "last-read", "last read", FALSE) \
	x(last_mentioned, LAST_MENTIONED, "LastMentioned", "last-mentioned", "last mentioned", FALSE)
#define CHIME_PROP_OBJ_VAR room
#include "chime-props.h"


enum
{
	PROP_0,
	PROP_PRIVACY,
	PROP_TYPE,
	PROP_VISIBILITY,

	CHIME_PROPS_ENUM

	PROP_MOBILE_NOTIFICATION_PREFS,
	PROP_DESKTOP_NOTIFICATION_PREFS,
	LAST_PROP,
};

static GParamSpec *props[LAST_PROP];

enum {
	MESSAGE,
	MEMBERSHIP,
	MEMBERS_DONE,
	LAST_SIGNAL,
};

static guint signals[LAST_SIGNAL];

struct _ChimeRoom {
	ChimeObject parent_instance;

	gboolean privacy;
	ChimeRoomType type;
	gboolean visibility;

	CHIME_PROPS_VARS

	ChimeNotifyPref mobile_notification;
	ChimeNotifyPref desktop_notification;

	/* For open rooms */
	guint opens;
	GTask *open_task;
	ChimeConnection *cxn;
	GHashTable *members;
	gboolean members_done[2];
};

G_DEFINE_TYPE(ChimeRoom, chime_room, CHIME_TYPE_OBJECT)

CHIME_DEFINE_ENUM_TYPE(ChimeRoomType, chime_room_type,
       CHIME_ENUM_VALUE(CHIME_ROOM_TYPE_STANDARD,	"standard")
       CHIME_ENUM_VALUE(CHIME_ROOM_TYPE_MEETING,	"meeting")
       CHIME_ENUM_VALUE(CHIME_ROOM_TYPE_ORGANIZATION,	"organization"))

CHIME_DEFINE_ENUM_TYPE(ChimeNotifyPref, chime_notify_pref,
       CHIME_ENUM_VALUE(CHIME_NOTIFY_PREF_ALWAYS,	"always")
       CHIME_ENUM_VALUE(CHIME_NOTIFY_PREF_DIRECT_ONLY,	"directOnly")
       CHIME_ENUM_VALUE(CHIME_NOTIFY_PREF_NEVER,	"never"))

static void close_room(gpointer key, gpointer val, gpointer data);

static void
chime_room_dispose(GObject *object)
{
	ChimeRoom *self = CHIME_ROOM(object);

	chime_debug("Room disposed: %p\n", self);

	close_room(NULL, self, NULL);

	G_OBJECT_CLASS(chime_room_parent_class)->dispose(object);
}

static void
chime_room_finalize(GObject *object)
{
	ChimeRoom *self = CHIME_ROOM(object);

	CHIME_PROPS_FREE

	if (self->members)
		g_hash_table_destroy(self->members);

	G_OBJECT_CLASS(chime_room_parent_class)->finalize(object);
}

static void chime_room_get_property(GObject *object, guint prop_id,
				    GValue *value, GParamSpec *pspec)
{
	ChimeRoom *self = CHIME_ROOM(object);

	switch (prop_id) {
	case PROP_PRIVACY:
		g_value_set_boolean(value, self->privacy);
		break;
	case PROP_TYPE:
		g_value_set_enum(value, self->type);
		break;
	case PROP_VISIBILITY:
		g_value_set_boolean(value, self->visibility);
		break;

	CHIME_PROPS_GET

	case PROP_MOBILE_NOTIFICATION_PREFS:
		g_value_set_enum(value, self->mobile_notification);
		break;
	case PROP_DESKTOP_NOTIFICATION_PREFS:
		g_value_set_enum(value, self->desktop_notification);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void chime_room_set_property(GObject *object, guint prop_id,
				    const GValue *value, GParamSpec *pspec)
{
	ChimeRoom *self = CHIME_ROOM(object);

	switch (prop_id) {
	case PROP_PRIVACY:
		self->privacy = g_value_get_boolean(value);
		break;
	case PROP_TYPE:
		self->type = g_value_get_enum(value);
		break;
	case PROP_VISIBILITY:
		self->visibility = g_value_get_boolean(value);
		break;

	CHIME_PROPS_SET

	case PROP_MOBILE_NOTIFICATION_PREFS:
		self->mobile_notification = g_value_get_enum(value);
		break;
	case PROP_DESKTOP_NOTIFICATION_PREFS:
		self->desktop_notification = g_value_get_enum(value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void chime_room_class_init(ChimeRoomClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->finalize = chime_room_finalize;
	object_class->dispose = chime_room_dispose;
	object_class->get_property = chime_room_get_property;
	object_class->set_property = chime_room_set_property;

	props[PROP_PRIVACY] =
		g_param_spec_boolean("privacy",
				     "privacy",
				     "privacy",
				     FALSE,
				     G_PARAM_READWRITE |
				     G_PARAM_CONSTRUCT |
				     G_PARAM_STATIC_STRINGS);

	props[PROP_TYPE] =
		g_param_spec_enum("type",
				  "type",
				  "type",
				  CHIME_TYPE_ROOM_TYPE,
				  CHIME_ROOM_TYPE_STANDARD,
				  G_PARAM_READWRITE |
				  G_PARAM_CONSTRUCT |
				  G_PARAM_STATIC_STRINGS);

	props[PROP_VISIBILITY] =
		g_param_spec_boolean("visibility",
				     "visibility",
				     "visibility",
				     TRUE,
				     G_PARAM_READWRITE |
				     G_PARAM_CONSTRUCT |
				     G_PARAM_STATIC_STRINGS);


	CHIME_PROPS_REG

	props[PROP_MOBILE_NOTIFICATION_PREFS] =
		g_param_spec_enum("mobile-notification-prefs",
				  "mobile-notification-prefs",
				  "mobile-notification-prefs",
				  CHIME_TYPE_NOTIFY_PREF,
				  CHIME_NOTIFY_PREF_ALWAYS,
				  G_PARAM_READWRITE |
				  G_PARAM_CONSTRUCT |
				  G_PARAM_STATIC_STRINGS);

	props[PROP_DESKTOP_NOTIFICATION_PREFS] =
		g_param_spec_enum("desktop-notification-prefs",
				  "desktop-notification-prefs",
				  "desktop-notification-prefs",
				  CHIME_TYPE_NOTIFY_PREF,
				  CHIME_NOTIFY_PREF_ALWAYS,
				  G_PARAM_READWRITE |
				  G_PARAM_CONSTRUCT |
				  G_PARAM_STATIC_STRINGS);

	g_object_class_install_properties(object_class, LAST_PROP, props);

	signals[MESSAGE] =
		g_signal_new ("message",
			      G_OBJECT_CLASS_TYPE (object_class), G_SIGNAL_RUN_FIRST,
			      0, NULL, NULL, NULL, G_TYPE_NONE, 1, JSON_TYPE_NODE);

	signals[MEMBERSHIP] =
		g_signal_new ("membership",
			      G_OBJECT_CLASS_TYPE (object_class), G_SIGNAL_RUN_FIRST,
			      0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_POINTER);

	signals[MEMBERS_DONE] =
		g_signal_new ("members-done",
			      G_OBJECT_CLASS_TYPE (object_class), G_SIGNAL_RUN_FIRST,
			      0, NULL, NULL, NULL, G_TYPE_NONE, 0);

}

static void chime_room_init(ChimeRoom *self)
{
}

const gchar *chime_room_get_id(ChimeRoom *self)
{
	g_return_val_if_fail(CHIME_IS_ROOM(self), NULL);

	return chime_object_get_id(CHIME_OBJECT(self));
}

const gchar *chime_room_get_name(ChimeRoom *self)
{
	g_return_val_if_fail(CHIME_IS_ROOM(self), NULL);

	return chime_object_get_name(CHIME_OBJECT(self));
}

gboolean chime_room_get_privacy(ChimeRoom *self)
{
	g_return_val_if_fail(CHIME_IS_ROOM(self), FALSE);

	return self->privacy;
}

gboolean chime_room_get_visibility(ChimeRoom *self)
{
	g_return_val_if_fail(CHIME_IS_ROOM(self), FALSE);

	return self->visibility;
}

const gchar *chime_room_get_channel(ChimeRoom *self)
{
	g_return_val_if_fail(CHIME_IS_ROOM(self), NULL);

	return self->channel;
}

const gchar *chime_room_get_last_mentioned(ChimeRoom *self)
{
	g_return_val_if_fail(CHIME_IS_ROOM(self), NULL);

	return self->last_mentioned;
}

const gchar *chime_room_get_last_read(ChimeRoom *self)
{
	g_return_val_if_fail(CHIME_IS_ROOM(self), NULL);

	return self->last_read;
}

const gchar *chime_room_get_last_sent(ChimeRoom *self)
{
	g_return_val_if_fail(CHIME_IS_ROOM(self), NULL);

	return self->last_sent;
}

const gchar *chime_room_get_created_on(ChimeRoom *self)
{
	g_return_val_if_fail(CHIME_IS_ROOM(self), NULL);

	return self->created_on;
}

static gboolean cmp_time(const char *ev, const char *last_read)
{
	GTimeVal ev_time, read_time;

	if (!ev || !g_time_val_from_iso8601(ev, &ev_time))
		return FALSE;

	if (!last_read || !g_time_val_from_iso8601(last_read, &read_time))
		return TRUE;

	return (ev_time.tv_sec > read_time.tv_sec ||
		(ev_time.tv_sec == read_time.tv_sec && ev_time.tv_usec > read_time.tv_usec));
}

gboolean chime_room_has_mention(ChimeRoom *self)
{
	g_return_val_if_fail(CHIME_IS_ROOM(self), FALSE);

	return cmp_time(self->last_mentioned, self->last_read);
}

gboolean chime_room_has_unread(ChimeRoom *self)
{
	g_return_val_if_fail(CHIME_IS_ROOM(self), FALSE);

	return cmp_time(self->last_sent, self->last_read);
}


static gboolean parse_privacy(JsonNode *node, const gchar *member, gboolean *val)
{
	const gchar *str;

	if (!parse_string(node, member, &str))
		return FALSE;

	if (!strcmp(str, "private"))
		*val = TRUE;
	else if (!strcmp(str, "public"))
		*val = FALSE;
	else
		return FALSE;

	return TRUE;
}

static gboolean parse_room_type(JsonNode *node, const gchar *member, ChimeRoomType *type)
{
	const gchar *str;

	if (!parse_string(node, member, &str))
		return FALSE;

	gpointer klass = g_type_class_ref(CHIME_TYPE_ROOM_TYPE);
	GEnumValue *val = g_enum_get_value_by_nick(klass, str);
	g_type_class_unref(klass);

	if (!val)
		return FALSE;
	*type = val->value;
	return TRUE;
}

static ChimeRoom *chime_connection_parse_room(ChimeConnection *cxn, JsonNode *node,
					      GError **error)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE(cxn);
	const gchar *id, *name;
	gboolean privacy, visibility;
	ChimeRoomType type;
	ChimeNotifyPref desktop, mobile;
	CHIME_PROPS_PARSE_VARS

	if (!parse_string(node, "RoomId", &id) ||
	    !parse_string(node, "Name", &name) ||
	    !parse_privacy(node, "Privacy", &privacy) ||
	    !parse_room_type(node, "Type", &type) ||
	    CHIME_PROPS_PARSE ||
	    !parse_visibility(node, "Visibility", &visibility)) {
	eparse:
		g_set_error(error, CHIME_ERROR,
			    CHIME_ERROR_BAD_RESPONSE,
			    _("Failed to parse Room node"));
		return NULL;
	}

	JsonObject *obj = json_node_get_object(node);
	node = json_object_get_member(obj, "Preferences");
	if (!node)
		goto eparse;
	obj = json_node_get_object(node);
	node = json_object_get_member(obj, "NotificationPreferences");
	if (!node)
		goto eparse;
	if (!parse_notify_pref(node, "DesktopNotificationPreferences", &desktop) ||
	    !parse_notify_pref(node, "MobileNotificationPreferences", &mobile))
		goto eparse;

	ChimeRoom *room = g_hash_table_lookup(priv->rooms.by_id, id);
	if (!room) {
		room = g_object_new(CHIME_TYPE_ROOM,
				    "id", id,
				    "name", name,
				    "privacy", privacy,
				    "type", type,
				    "visibility", visibility,
				    CHIME_PROPS_NEWOBJ
				    "desktop-notification-prefs", desktop,
				    "mobile-notification-prefs", mobile,
				    NULL);

		chime_object_collection_hash_object(&priv->rooms, CHIME_OBJECT(room), TRUE);

		/* Emit signal on ChimeConnection to admit existence of new room */
		chime_connection_new_room(cxn, room);

		return room;
	}

	if (name && g_strcmp0(name, chime_object_get_name(CHIME_OBJECT(room)))) {
		chime_object_rename(CHIME_OBJECT(room), name);
		g_object_notify(G_OBJECT(room), "name");
	}
	if (privacy != room->privacy) {
		room->privacy = privacy;
		g_object_notify(G_OBJECT(room), "privacy");
	}
	if (type != room->type) {
		room->type = type;
		g_object_notify(G_OBJECT(room), "type");
	}
	if (visibility != room->visibility) {
		room->visibility = visibility;
		g_object_notify(G_OBJECT(room), "visibility");
	}

	CHIME_PROPS_UPDATE

	if (desktop != room->desktop_notification) {
		room->desktop_notification = desktop;
		g_object_notify(G_OBJECT(room), "desktop-notification-prefs");
	}
	if (mobile != room->mobile_notification) {
		room->mobile_notification = mobile;
		g_object_notify(G_OBJECT(room), "mobile-notification-prefs");
	}

	chime_object_collection_hash_object(&priv->rooms, CHIME_OBJECT(room), TRUE);

	return room;
}

static void fetch_rooms(ChimeConnection *cxn, const gchar *next_token);

static void rooms_cb(ChimeConnection *cxn, SoupMessage *msg, JsonNode *node,
			gpointer _unused)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	/* If it got invalidated while in transit, refetch */
	if (priv->rooms_sync != CHIME_SYNC_FETCHING) {
		priv->rooms_sync = CHIME_SYNC_IDLE;
		fetch_rooms(cxn, NULL);
		return;
	}

	if (SOUP_STATUS_IS_SUCCESSFUL(msg->status_code) && node) {
		JsonObject *obj = json_node_get_object(node);
		JsonNode *rooms_node = json_object_get_member(obj, "Rooms");
		if (!rooms_node) {
			chime_connection_fail(cxn, CHIME_ERROR_BAD_RESPONSE,
					      _("Failed to find Rooms node in response"));
			return;
		}
		JsonArray *arr = json_node_get_array(rooms_node);
		guint i, len = json_array_get_length(arr);

		for (i = 0; i < len; i++) {
			chime_connection_parse_room(cxn,
						    json_array_get_element(arr, i),
						    NULL);
		}

		const gchar *next_token;
		if (parse_string(node, "NextToken", &next_token))
			fetch_rooms(cxn, next_token);
		else {
			priv->rooms_sync = CHIME_SYNC_IDLE;

			chime_object_collection_expire_outdated(&priv->rooms);

			if (!priv->rooms_online) {
				priv->rooms_online = TRUE;
				chime_connection_calculate_online(cxn);
			}
		}
	} else {
		const gchar *reason = msg->reason_phrase;

		parse_string(node, "error", &reason);

		chime_connection_fail(cxn, CHIME_ERROR_NETWORK,
				      _("Failed to fetch rooms (%d): %s\n"),
				      msg->status_code, reason);
	}
}

static void fetch_rooms(ChimeConnection *cxn, const gchar *next_token)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	if (!next_token) {
		/* Actually we could listen for the 'starting' flag on the message,
		 * and as long as *that* hasn't happened yet we don't need to refetch
		 * as it'll get up-to-date information. */
		switch(priv->rooms_sync) {
		case CHIME_SYNC_FETCHING:
			priv->rooms_sync = CHIME_SYNC_STALE;
		case CHIME_SYNC_STALE:
			return;

		case CHIME_SYNC_IDLE:
			priv->rooms.generation++;
			priv->rooms_sync = CHIME_SYNC_FETCHING;
		}
	}

	SoupURI *uri = soup_uri_new_printf(priv->messaging_url, "/rooms");
	soup_uri_set_query_from_fields(uri, "max-results", "50",
				       next_token ? "next-token" : NULL, next_token,
				       NULL);
	chime_connection_queue_http_request(cxn, NULL, uri, "GET", rooms_cb,
					    NULL);
}

static gboolean visible_rooms_jugg_cb(ChimeConnection *cxn, gpointer _unused, JsonNode *data_node)
{
	fetch_rooms(cxn, NULL);
	return TRUE;
}

static gboolean room_msg_jugg_cb(ChimeConnection *cxn, gpointer _room, JsonNode *data_node)
{
	ChimeRoom *room = CHIME_ROOM(_room);
	JsonObject *obj = json_node_get_object(data_node);
	JsonNode *record = json_object_get_member(obj, "record");
	if (!record)
		return FALSE;

	const gchar *id;
	if (!parse_string(record, "MessageId", &id))
		return FALSE;

	g_signal_emit(room, signals[MESSAGE], 0, record);
	return TRUE;
}

static gboolean demux_room_msg_jugg_cb(ChimeConnection *cxn, gpointer _room, JsonNode *data_node);
static void demux_fetch_room_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
	ChimeConnection *cxn = CHIME_CONNECTION(source);
	ChimeRoom *room = chime_connection_fetch_room_finish(cxn, result, NULL);

	if (user_data) {
		/* Sanity check that demux_room_msg_jugg_cb() *will* be able to look it up... */
		if (room)
			demux_room_msg_jugg_cb(cxn, room, user_data);

		json_node_unref(user_data);
	}
}

static gboolean demux_room_msg_jugg_cb(ChimeConnection *cxn, gpointer _room, JsonNode *data_node)
{
	JsonObject *obj = json_node_get_object(data_node);
	JsonNode *record = json_object_get_member(obj, "record");
	if (!record)
		return FALSE;

	const gchar *room_id;
	if (!parse_string(record, "RoomId", &room_id))
		return FALSE;

	ChimeRoom *room = _room;
	if (!room)
		room = chime_connection_room_by_id(cxn, room_id);
	if (!room) {
		/* It seems they don't do the helpful thing and send the notification
		 * of a new room before they send the first message. So let's go
		 * looking for it... */
		/* XXX: We *do* get VisibleRooms first though, and could avoid this
		 * query if that one is already running... */
		chime_connection_fetch_room_async(cxn, room_id, NULL, demux_fetch_room_done, json_node_ref(data_node));
		return TRUE;
	}
	if (room->opens)
		return room_msg_jugg_cb(cxn, room, data_node);

	g_signal_emit_by_name(cxn, "room-mention", room, record);
	return TRUE;
}

static gboolean room_jugg_cb(ChimeConnection *cxn, gpointer _unused, JsonNode *data_node)
{
	const gchar *type;
	if (!parse_string(data_node, "type", &type))
		return FALSE;

	if (strcmp(type, "update"))
		return FALSE;

	JsonObject *obj = json_node_get_object(data_node);
	JsonNode *record_node = json_object_get_member(obj, "record");
	if (!record_node)
		return FALSE;

	if (chime_connection_parse_room(cxn, record_node, NULL))
		return TRUE;

	const gchar *room_id;
	if (!parse_string(record_node, "RoomId", &room_id))
		return FALSE;

	chime_connection_fetch_room_async(cxn, room_id, NULL, demux_fetch_room_done, NULL);
	return TRUE;
}

void chime_init_rooms(ChimeConnection *cxn)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	chime_object_collection_init(cxn, &priv->rooms);

	chime_jugg_subscribe(cxn, priv->profile_channel, "VisibleRooms",
			     visible_rooms_jugg_cb, NULL);
	if (0) chime_jugg_subscribe(cxn, priv->device_channel, "JoinableMeetings",
			     visible_rooms_jugg_cb, NULL);
	chime_jugg_subscribe(cxn, priv->device_channel, "Room",
			     room_jugg_cb, NULL);
	chime_jugg_subscribe(cxn, priv->device_channel, "RoomMessage",
			     demux_room_msg_jugg_cb, NULL);
	fetch_rooms(cxn, NULL);
}

void chime_destroy_rooms(ChimeConnection *cxn)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	chime_jugg_unsubscribe(cxn, priv->profile_channel, "VisibleRooms",
			       visible_rooms_jugg_cb, NULL);
	chime_jugg_unsubscribe(cxn, priv->device_channel, "JoinableMeetings",
			     visible_rooms_jugg_cb, NULL);
	chime_jugg_unsubscribe(cxn, priv->device_channel, "Room",
			       room_jugg_cb, NULL);
	chime_jugg_unsubscribe(cxn, priv->device_channel, "RoomMessage",
			       demux_room_msg_jugg_cb, NULL);

	if (priv->rooms.by_id)
		g_hash_table_foreach(priv->rooms.by_id, close_room, NULL);

	chime_object_collection_destroy(&priv->rooms);
}

ChimeRoom *chime_connection_room_by_name(ChimeConnection *cxn,
					 const gchar *name)
{
	g_return_val_if_fail(CHIME_IS_CONNECTION(cxn), NULL);
	g_return_val_if_fail(name, NULL);

	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	return g_hash_table_lookup(priv->rooms.by_name, name);
}

ChimeRoom *chime_connection_room_by_id(ChimeConnection *cxn,
				       const gchar *id)
{
	g_return_val_if_fail(CHIME_IS_CONNECTION(cxn), NULL);
	g_return_val_if_fail(id, NULL);

	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	return g_hash_table_lookup(priv->rooms.by_id, id);
}

void chime_connection_foreach_room(ChimeConnection *cxn, ChimeRoomCB cb,
				   gpointer cbdata)
{
	g_return_if_fail(CHIME_IS_CONNECTION(cxn));
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE(cxn);

	chime_object_collection_foreach_object(cxn, &priv->rooms, (ChimeObjectCB)cb, cbdata);
}

static void free_member(gpointer _member)
{
	ChimeRoomMember *member = _member;

	g_object_unref(member->contact);
	g_free(member->last_read);
	g_free(member->last_delivered);
	g_free(member);
}

static gboolean add_room_member(ChimeConnection *cxn, ChimeRoom *room, JsonNode *node)
{
	JsonObject *obj = json_node_get_object(node);
	JsonNode *member_node = json_object_get_member(obj, "Member");
	if (!member_node)
		return FALSE;

	ChimeContact *contact = chime_connection_parse_conversation_contact(cxn, member_node, NULL);
	if (!contact)
		return FALSE;

	ChimeRoomMember *member = g_hash_table_lookup(room->members, chime_contact_get_profile_id(contact));
	if (!member) {
		member = g_new0(ChimeRoomMember, 1);
		member->contact = contact;
		g_hash_table_insert(room->members, (void *)chime_contact_get_profile_id(contact), member);
	} else {
		g_object_unref(contact);
	}

	const char *role, *presence, *status, *last_read, *last_delivered;

	if (parse_string(member_node, "LastRead", &last_read) &&
	    g_strcmp0(last_read, member->last_read)) {
		    g_free(member->last_read);
		    member->last_read = g_strdup(last_read);
	}
	if (parse_string(member_node, "LastDelivered", &last_delivered) &&
	    g_strcmp0(last_delivered, member->last_delivered)) {
		    g_free(member->last_delivered);
		    member->last_delivered = g_strdup(last_delivered);
	}
	member->admin = parse_string(node, "Role", &role) && !strcmp(role, "administrator");
	member->present = parse_string(node, "Presence", &presence) && !strcmp(presence, "present");
	member->active = parse_string(node, "Status", &status) && !strcmp(status, "active");

	g_signal_emit(room, signals[MEMBERSHIP], 0, member);
	return TRUE;
}

static gboolean room_membership_jugg_cb(ChimeConnection *cxn, gpointer _room, JsonNode *data_node)
{
	ChimeRoom *room = CHIME_ROOM(_room);
	JsonObject *obj = json_node_get_object(data_node);
	JsonNode *record = json_object_get_member(obj, "record");
	if (!record)
		return FALSE;

	return add_room_member(cxn, room, record);
}


void fetch_room_memberships(ChimeConnection *cxn, ChimeRoom *room, gboolean active, const gchar *next_token);
gboolean chime_connection_open_room(ChimeConnection *cxn, ChimeRoom *room)
{
	g_return_val_if_fail(CHIME_IS_CONNECTION(cxn), FALSE);
	g_return_val_if_fail(CHIME_IS_ROOM(room), FALSE);

	if (!room->opens++) {
		room->members = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, free_member);
		room->cxn = cxn;
		chime_jugg_subscribe(cxn, room->channel, "Room", room_jugg_cb, NULL);
		chime_jugg_subscribe(cxn, room->channel, "RoomMessage", room_msg_jugg_cb, room);
		chime_jugg_subscribe(cxn, room->channel, "RoomMembership", room_membership_jugg_cb, room);
		fetch_room_memberships(cxn, room, TRUE, NULL);
		fetch_room_memberships(cxn, room, FALSE, NULL);
	}

	return room->members_done[0] && room->members_done[1];
}

static void close_room(gpointer key, gpointer val, gpointer data)
{
	ChimeRoom *room = CHIME_ROOM (val);
	if (room->cxn) {
		chime_jugg_unsubscribe(room->cxn, room->channel, "Room", room_jugg_cb, NULL);
		chime_jugg_unsubscribe(room->cxn, room->channel, "RoomMessage", room_msg_jugg_cb, room);
		chime_jugg_unsubscribe(room->cxn, room->channel, "RoomMembership", room_membership_jugg_cb, room);
		room->cxn = NULL;
	}
	if (room->members) {
		g_hash_table_destroy(room->members);
		room->members = NULL;
	}
	room->members_done[0] = room->members_done[1] = FALSE;
}

void chime_connection_close_room(ChimeConnection *cxn, ChimeRoom *room)
{
	g_return_if_fail(CHIME_IS_CONNECTION(cxn));
	g_return_if_fail(CHIME_IS_ROOM(room));
	g_return_if_fail(room->opens);

	if (!--room->opens)
		close_room(NULL, room, NULL);
}


static void fetch_members_cb(ChimeConnection *cxn, SoupMessage *msg, JsonNode *node, gpointer _roomx)
{
	ChimeRoom *room = CHIME_ROOM((void *)((unsigned long)_roomx & ~1UL));
	gboolean active = (unsigned long) _roomx & 1;
	const gchar *next_token;

	if (!SOUP_STATUS_IS_SUCCESSFUL(msg->status_code)) {
		const gchar *reason = msg->reason_phrase;

		if (node)
			parse_string(node, "error", &reason);

		g_warning("Failed to fetch room memberships: %d %s\n", msg->status_code, reason);
 	} else {
		JsonObject *obj = json_node_get_object(node);
		JsonNode *members_node = json_object_get_member(obj, "RoomMemberships");
		JsonArray *members_array = json_node_get_array(members_node);

		int i, len = json_array_get_length(members_array);
		for (i = 0; i < len; i++) {
			JsonNode *member_node = json_array_get_element(members_array, i);
			add_room_member(cxn, room, member_node);
		}

		if (parse_string(node, "NextToken", &next_token)) {
			fetch_room_memberships(cxn, room, active, next_token);
			return;
		}
	}
	room->members_done[active] = TRUE;
	if (room->members_done[!active])
		g_signal_emit(room, signals[MEMBERS_DONE], 0);
}

void fetch_room_memberships(ChimeConnection *cxn, ChimeRoom *room, gboolean active, const gchar *next_token)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	SoupURI *uri = soup_uri_new_printf(priv->messaging_url, "/rooms/%s/memberships",
					   chime_object_get_id(CHIME_OBJECT(room)));
	const gchar *opts[4] = {NULL};
	int i = 0;

	if (!active) {
		opts[i++] = "status";
		opts[i++] = "inActive";
	}
	if (next_token) {
		opts[i++] = "next-token";
		opts[i++] = next_token;
	}

	soup_uri_set_query_from_fields(uri, "max-results", "50", opts[0], opts[1], opts[2], opts[3], NULL);
	chime_connection_queue_http_request(cxn, NULL, uri, "GET", fetch_members_cb, (void *)((unsigned long)room | active));
}

GList *chime_room_get_members(ChimeRoom *room)
{
	return g_hash_table_get_values(room->members);
}

static void member_added_cb(ChimeConnection *cxn, SoupMessage *msg,
			    JsonNode *node, gpointer user_data)
{
	GTask *task = G_TASK(user_data);

	if (SOUP_STATUS_IS_SUCCESSFUL(msg->status_code) && node) {
		JsonObject *obj = json_node_get_object(node);

		node = json_object_get_member(obj, "RoomMembership");
		if (node) {
			add_room_member(cxn, CHIME_ROOM(g_task_get_task_data(task)), node);
			g_task_return_boolean(task, TRUE);
		} else
			g_task_return_new_error(task, CHIME_ERROR, CHIME_ERROR_NETWORK,
						_("Failed to add room member"));
	} else {
		const gchar *reason = msg->reason_phrase;

		if (node)
			parse_string(node, "Message", &reason);

		g_task_return_new_error(task, CHIME_ERROR,
					CHIME_ERROR_NETWORK,
					_("Failed to add room member: %s"),
					reason);
	}

	g_object_unref(task);
}

void chime_connection_add_room_member_async(ChimeConnection *cxn,
					    ChimeRoom *room,
					    ChimeContact *contact,
					    GCancellable *cancellable,
					    GAsyncReadyCallback callback,
					    gpointer user_data)
{
	g_return_if_fail(CHIME_IS_CONNECTION(cxn));
	g_return_if_fail(CHIME_IS_ROOM(room));
	g_return_if_fail(CHIME_IS_CONTACT(contact));
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	GTask *task = g_task_new(cxn, cancellable, callback, user_data);
	g_task_set_task_data(task, g_object_ref(room), g_object_unref);

	JsonBuilder *jb = json_builder_new();
 	jb = json_builder_begin_object(jb);
	jb = json_builder_set_member_name(jb, "ProfileId");
	jb = json_builder_add_string_value(jb, chime_contact_get_profile_id(contact));
	jb = json_builder_end_object(jb);

	SoupURI *uri = soup_uri_new_printf(priv->messaging_url, "/rooms/%s/memberships", chime_room_get_id(room));
	JsonNode *node = json_builder_get_root(jb);
	chime_connection_queue_http_request(cxn, node, uri, "POST", member_added_cb, task);

	json_node_unref(node);
	g_object_unref(jb);
}

gboolean chime_connection_add_room_member_finish(ChimeConnection *self,
						 GAsyncResult *result,
						 GError **error)
{
	g_return_val_if_fail(CHIME_IS_CONNECTION(self), FALSE);
	g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

	return g_task_propagate_boolean(G_TASK(result), error);
}


static void member_removed_cb(ChimeConnection *cxn, SoupMessage *msg,
			      JsonNode *node, gpointer user_data)
{
	GTask *task = G_TASK(user_data);

	if (!SOUP_STATUS_IS_SUCCESSFUL(msg->status_code)) {
		const gchar *reason = msg->reason_phrase;

		if (node)
			parse_string(node, "Message", &reason);

		g_task_return_new_error(task, CHIME_ERROR,
					CHIME_ERROR_NETWORK,
					_("Failed to remove room member: %s"),
					reason);
	} else {
		g_task_return_boolean(task, TRUE);
	}

	g_object_unref(task);
}

void chime_connection_remove_room_member_async(ChimeConnection *cxn,
					       ChimeRoom *room,
					       ChimeContact *contact,
					       GCancellable *cancellable,
					       GAsyncReadyCallback callback,
					       gpointer user_data)
{
	g_return_if_fail(CHIME_IS_CONNECTION(cxn));
	g_return_if_fail(CHIME_IS_ROOM(room));
	g_return_if_fail(CHIME_IS_CONTACT(contact));
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	GTask *task = g_task_new(cxn, cancellable, callback, user_data);
	g_task_set_task_data(task, g_object_ref(room), g_object_unref);

	SoupURI *uri = soup_uri_new_printf(priv->messaging_url, "/rooms/%s/memberships/%s",
					   chime_room_get_id(room), chime_contact_get_profile_id(contact));
	chime_connection_queue_http_request(cxn, NULL, uri, "DELETE", member_removed_cb, task);
}

gboolean chime_connection_remove_room_member_finish(ChimeConnection *self,
						    GAsyncResult *result,
						    GError **error)
{
	g_return_val_if_fail(CHIME_IS_CONNECTION(self), FALSE);
	g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

	return g_task_propagate_boolean(G_TASK(result), error);
}

static void fetch_new_room_cb(ChimeConnection *cxn, SoupMessage *msg, JsonNode *node,
			      gpointer _task)
{
	GTask *task = G_TASK(_task);

	if (SOUP_STATUS_IS_SUCCESSFUL(msg->status_code)) {
		JsonObject *obj = json_node_get_object(node);
		node = json_object_get_member(obj, "Room");
		if (!node)
			goto bad;

		ChimeRoom *room = chime_connection_parse_room(cxn, node, NULL);
		if (!room)
			goto bad;

		g_task_return_pointer(task, g_object_ref(room), g_object_unref);
	} else {
 bad:
		g_task_return_new_error(task, CHIME_ERROR, CHIME_ERROR_NETWORK,
					_("Failed to fetch room details"));
	}
	g_object_unref(task);
}

void chime_connection_fetch_room_async(ChimeConnection *cxn, const gchar *room_id,
				       GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
	g_return_if_fail(CHIME_IS_CONNECTION(cxn));

	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);
	GTask *task = g_task_new(cxn, cancellable, callback, user_data);

	SoupURI *uri = soup_uri_new_printf(priv->messaging_url, "/rooms/%s", room_id);
	chime_connection_queue_http_request(cxn, NULL, uri, "GET", fetch_new_room_cb, task);
}

ChimeRoom *chime_connection_fetch_room_finish(ChimeConnection *cxn, GAsyncResult *result, GError **error)
{
	g_return_val_if_fail(CHIME_IS_CONNECTION(cxn), NULL);
	g_return_val_if_fail(g_task_is_valid(result, cxn), FALSE);

	return g_task_propagate_pointer(G_TASK(result), error);
}

