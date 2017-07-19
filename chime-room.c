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
#include "chime.h"

#include <glib/gi18n.h>

enum
{
	PROP_0,
	PROP_ID,
	PROP_NAME,
	PROP_PRIVACY,
	PROP_TYPE,
	PROP_VISIBILITY,
	PROP_CHANNEL,
	PROP_OPEN,
	PROP_LAST_SENT,
	PROP_LAST_READ,
	PROP_LAST_MENTIONED,
	PROP_CREATED_ON,
	PROP_UPDATED_ON,
	PROP_MOBILE_NOTIFICATION_PREFS,
	PROP_DESKTOP_NOTIFICATION_PREFS,
	PROP_DEAD,
	LAST_PROP,
};

static GParamSpec *props[LAST_PROP];

struct _ChimeRoom {
	GObject parent_instance;

	gchar *id;
	gchar *name;
	gboolean privacy;
	ChimeRoomType type;
	gboolean visibility;
	gchar *channel;
	gboolean open;
	gchar *last_sent;
	gchar *last_read;
	gchar *last_mentioned;
	gchar *created_on;
	gchar *updated_on;
	ChimeNotifyPref mobile_notification;
	ChimeNotifyPref desktop_notification;

	gint64 rooms_generation;

	/* Here be dragons!
	 * We need to cope with rooms for which we have a chat open (hence
	 * Pidgin is holding a ref on the ChimeRoom object, and we're
	 * removed from them. But then we apologise out-of-band and we're
	 * re-added to the room, and we need to have the *same* ChimeRoom
	 * object not a new one. So it needs to stay in the connection's
	 * hash table even while it's "dead", until the ChimeRoom object
	 * finally goes away.
	 *
	 * So.... we use the 'is_dead' flag. When a ChimeRoom is not dead,
	 * the hash table 'owns' a ref on it. When a ChimeRoom is dead,
	 * we drop that ref and set the is_dead flag. The object will then
	 * last only as long as it is being actively used by Pidgin, then
	 * be deleted.
	 */
	gboolean is_dead;
};

G_DEFINE_TYPE(ChimeRoom, chime_room, G_TYPE_OBJECT)

CHIME_DEFINE_ENUM_TYPE(ChimeRoomType, chime_room_type,			\
       CHIME_ENUM_VALUE(CHIME_ROOM_TYPE_STANDARD,	"standard")	\
       CHIME_ENUM_VALUE(CHIME_ROOM_TYPE_MEETING,	"meeting")	\
       CHIME_ENUM_VALUE(CHIME_ROOM_TYPE_ORGANIZATION,	"organization"))

CHIME_DEFINE_ENUM_TYPE(ChimeNotifyPref, chime_notify_pref,		\
       CHIME_ENUM_VALUE(CHIME_NOTIFY_PREF_ALWAYS,	"always")	\
       CHIME_ENUM_VALUE(CHIME_NOTIFY_PREF_DIRECT_ONLY,	"directOnly")	\
       CHIME_ENUM_VALUE(CHIME_NOTIFY_PREF_NEVER,	"nevers"))

static void
chime_room_finalize(GObject *object)
{
	ChimeRoom *self = CHIME_ROOM(object);

	g_free(self->id);
	g_free(self->name);
	g_free(self->channel);
	g_free(self->last_sent);
	g_free(self->last_read);
	g_free(self->last_mentioned);
	g_free(self->created_on);
	g_free(self->updated_on);

	G_OBJECT_CLASS(chime_room_parent_class)->finalize(object);
}

static void chime_room_get_property(GObject *object, guint prop_id,
				    GValue *value, GParamSpec *pspec)
{
	ChimeRoom *self = CHIME_ROOM(object);

	switch (prop_id) {
	case PROP_ID:
		g_value_set_string(value, self->id);
		break;
	case PROP_NAME:
		g_value_set_string(value, self->name);
		break;
	case PROP_PRIVACY:
		g_value_set_boolean(value, self->privacy);
		break;
	case PROP_TYPE:
		g_value_set_enum(value, self->type);
		break;
	case PROP_VISIBILITY:
		g_value_set_boolean(value, self->visibility);
		break;
	case PROP_CHANNEL:
		g_value_set_string(value, self->channel);
		break;
	case PROP_OPEN:
		g_value_set_boolean(value, self->open);
		break;
	case PROP_LAST_SENT:
		g_value_set_string(value, self->last_sent);
		break;
	case PROP_LAST_READ:
		g_value_set_string(value, self->last_read);
		break;
	case PROP_LAST_MENTIONED:
		g_value_set_string(value, self->last_mentioned);
		break;
	case PROP_CREATED_ON:
		g_value_set_string(value, self->created_on);
		break;
	case PROP_UPDATED_ON:
		g_value_set_string(value, self->updated_on);
		break;
	case PROP_MOBILE_NOTIFICATION_PREFS:
		g_value_set_enum(value, self->mobile_notification);
		break;
	case PROP_DESKTOP_NOTIFICATION_PREFS:
		g_value_set_enum(value, self->desktop_notification);
		break;
	case PROP_DEAD:
		g_value_set_boolean(value, self->is_dead);
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
	case PROP_ID:
		g_free(self->id);
		self->id = g_value_dup_string(value);
		break;
	case PROP_NAME:
		g_free(self->name);
		self->name = g_value_dup_string(value);
		break;
	case PROP_PRIVACY:
		self->privacy = g_value_get_boolean(value);
		break;
	case PROP_TYPE:
		self->type = g_value_get_enum(value);
		break;
	case PROP_VISIBILITY:
		self->visibility = g_value_get_boolean(value);
		break;
	case PROP_CHANNEL:
		g_free(self->channel);
		self->channel = g_value_dup_string(value);
		break;
	case PROP_OPEN:
		self->open = g_value_get_boolean(value);
		break;
	case PROP_LAST_SENT:
		g_free(self->last_sent);
		self->last_sent = g_value_dup_string(value);
		break;
	case PROP_LAST_READ:
		g_free(self->last_read);
		self->last_read = g_value_dup_string(value);
		break;
	case PROP_LAST_MENTIONED:
		g_free(self->last_mentioned);
		self->last_mentioned = g_value_dup_string(value);
		break;
	case PROP_CREATED_ON:
		g_free(self->created_on);
		self->created_on = g_value_dup_string(value);
		break;
	case PROP_UPDATED_ON:
		g_free(self->updated_on);
		self->updated_on = g_value_dup_string(value);
		break;
	case PROP_MOBILE_NOTIFICATION_PREFS:
		self->mobile_notification = g_value_get_enum(value);
		break;
	case PROP_DESKTOP_NOTIFICATION_PREFS:
		self->desktop_notification = g_value_get_enum(value);
		break;
	case PROP_DEAD:
		self->is_dead = g_value_get_boolean(value);
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
	object_class->get_property = chime_room_get_property;
	object_class->set_property = chime_room_set_property;

	props[PROP_ID] =
		g_param_spec_string("id",
				    "id",
				    "id",
				    NULL,
				    G_PARAM_READWRITE |
				    G_PARAM_CONSTRUCT_ONLY |
				    G_PARAM_STATIC_STRINGS);

	props[PROP_NAME] =
		g_param_spec_string("name",
				    "name",
				    "name",
				    NULL,
				    G_PARAM_READWRITE |
				    G_PARAM_CONSTRUCT_ONLY |
				    G_PARAM_STATIC_STRINGS);

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

	props[PROP_CHANNEL] =
		g_param_spec_string("channel",
				    "channel",
				    "channel",
				    NULL,
				    G_PARAM_READWRITE |
				    G_PARAM_CONSTRUCT_ONLY |
				    G_PARAM_STATIC_STRINGS);

	props[PROP_OPEN] =
		g_param_spec_boolean("open",
				     "open",
				     "open",
				     TRUE,
				     G_PARAM_READWRITE |
				     G_PARAM_CONSTRUCT |
				     G_PARAM_STATIC_STRINGS);

	props[PROP_LAST_SENT] =
		g_param_spec_string("last-sent",
				    "last sent",
				    "last sent",
				    NULL,
				    G_PARAM_READWRITE |
				    G_PARAM_CONSTRUCT |
				    G_PARAM_STATIC_STRINGS);

	props[PROP_LAST_READ] =
		g_param_spec_string("last-read",
				    "last read",
				    "last read",
				    NULL,
				    G_PARAM_READWRITE |
				    G_PARAM_CONSTRUCT |
				    G_PARAM_STATIC_STRINGS);

	props[PROP_LAST_MENTIONED] =
		g_param_spec_string("last-mentioned",
				    "last mentioned",
				    "last mentioned",
				    NULL,
				    G_PARAM_READWRITE |
				    G_PARAM_CONSTRUCT |
				    G_PARAM_STATIC_STRINGS);

	props[PROP_CREATED_ON] =
		g_param_spec_string("created-on",
				    "created on",
				    "created on",
				    NULL,
				    G_PARAM_READWRITE |
				    G_PARAM_CONSTRUCT |
				    G_PARAM_STATIC_STRINGS);
	props[PROP_UPDATED_ON] =
		g_param_spec_string("updated-on",
				    "updated on",
				    "updated on",
				    NULL,
				    G_PARAM_READWRITE |
				    G_PARAM_CONSTRUCT |
				    G_PARAM_STATIC_STRINGS);

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

	props[PROP_DEAD] =
		g_param_spec_boolean("dead",
				     "dead",
				     "dead",
				     FALSE,
				     G_PARAM_READWRITE |
				     G_PARAM_CONSTRUCT |
				     G_PARAM_STATIC_STRINGS);

	g_object_class_install_properties(object_class, LAST_PROP, props);
}

static void chime_room_init(ChimeRoom *self)
{
}

const gchar *chime_room_get_id(ChimeRoom *self)
{
	g_return_val_if_fail(CHIME_IS_ROOM(self), NULL);

	return self->id;
}

const gchar *chime_room_get_name(ChimeRoom *self)
{
	g_return_val_if_fail(CHIME_IS_ROOM(self), NULL);

	return self->name;
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

static gboolean parse_boolean(JsonNode *node, const gchar *member, gboolean *val)
{
	gint64 intval;

	if (!parse_int(node, member, &intval))
		return FALSE;

	*val = !!intval;
	return TRUE;
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
	const gchar *id, *name, *channel, *created_on, *updated_on,
		*last_sent = NULL, *last_read = NULL, *last_mentioned = NULL;
	gboolean privacy, visibility, is_open;
	ChimeRoomType type;
	ChimeNotifyPref desktop, mobile;

	if (!parse_string(node, "RoomId", &id) ||
	    !parse_string(node, "Name", &name) ||
	    !parse_privacy(node, "Privacy", &privacy) ||
	    !parse_room_type(node, "Type", &type) ||
	    !parse_visibility(node, "Visibility", &visibility) ||
	    !parse_string(node, "Channel", &channel) ||
	    !parse_boolean(node, "Open", &is_open) ||
	    !parse_string(node, "CreatedOn", &created_on) ||
	    !parse_string(node, "UpdatedOn", &updated_on)) {
	eparse:
		g_set_error(error, CHIME_CONNECTION_ERROR,
			    CHIME_CONNECTION_ERROR_PARSE,
			    _("Failed to parse Room node"));
		return NULL;
	}
	parse_string(node, "LastSent", &last_sent);
	parse_string(node, "LastRead", &last_read);
	parse_string(node, "LastMentioned", &last_mentioned);

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

	ChimeRoom *room = g_hash_table_lookup(priv->rooms_by_id, id);
	if (!room) {
		room = g_object_new(CHIME_TYPE_ROOM,
				    "id", id,
				    "name", name,
				    "privacy", privacy,
				    "type", type,
				    "visibility", visibility,
				    "channel", channel,
				    "open", is_open,
				    "last-sent", last_sent,
				    "last-read", last_read,
				    "last-mentioned", last_mentioned,
				    "created-on", created_on,
				    "updated-on", updated_on,
				    "desktop-notification-prefs", desktop,
				    "mobile-notification-prefs", mobile,
				    NULL);

		g_hash_table_insert(priv->rooms_by_id, room->id, room);
		g_hash_table_insert(priv->rooms_by_name, room->name, room);

		/* Emit signal on ChimeConnection to admit existence of new room */
		chime_connection_new_room(cxn, room);

		return room;
	}

	if (name && g_strcmp0(name, room->name)) {
		g_hash_table_remove(priv->rooms_by_name, room->name);
		/* XX: If there is another room with the same name, we should add it */
		g_free(room->name);
		room->name = g_strdup(name);
		g_hash_table_insert(priv->rooms_by_name, room->name, room);
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
	if (channel && g_strcmp0(channel, room->channel)) {
		g_free(room->channel);
		room->channel = g_strdup(channel);
		g_object_notify(G_OBJECT(room), "channel");
	}
	if (is_open != room->open) {
		room->open = is_open;
		g_object_notify(G_OBJECT(room), "open");
	}
	if (last_sent && g_strcmp0(last_sent, room->last_sent)) {
		g_free(room->last_sent);
		room->last_sent = g_strdup(last_sent);
		g_object_notify(G_OBJECT(room), "last-sent");
	}
	if (last_read && g_strcmp0(last_read, room->last_read)) {
		g_free(room->last_read);
		room->last_read = g_strdup(last_read);
		g_object_notify(G_OBJECT(room), "last-read");
	}
	if (last_mentioned && g_strcmp0(last_mentioned, room->last_mentioned)) {
		g_free(room->last_mentioned);
		room->last_mentioned = g_strdup(last_mentioned);
		g_object_notify(G_OBJECT(room), "last-mentioned");
	}
	if (created_on && g_strcmp0(created_on, room->created_on)) {
		g_free(room->created_on);
		room->created_on = g_strdup(created_on);
		g_object_notify(G_OBJECT(room), "created-on");
	}
	if (updated_on && g_strcmp0(updated_on, room->updated_on)) {
		g_free(room->updated_on);
		room->updated_on = g_strdup(updated_on);
		g_object_notify(G_OBJECT(room), "updated-on");
	}
	if (desktop != room->desktop_notification) {
		room->desktop_notification = desktop;
		g_object_notify(G_OBJECT(room), "desktop-notification-prefs");
	}
	if (mobile != room->mobile_notification) {
		room->mobile_notification = mobile;
		g_object_notify(G_OBJECT(room), "mobile-notification-prefs");
	}
	if (room->is_dead) {
		g_object_ref(room);
		room->is_dead = FALSE;
		g_object_notify(G_OBJECT(room), "dead");
	}

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
			chime_connection_fail(cxn, CHIME_CONNECTION_ERROR_PARSE,
					      _("Failed to find Rooms node in response"));
			return;
		}
		JsonArray *arr = json_node_get_array(rooms_node);
		guint i, len = json_array_get_length(arr);
		ChimeRoom *room;

		for (i = 0; i < len; i++) {
			room = chime_connection_parse_room(cxn,
							   json_array_get_element(arr, i),
							   NULL);
			if (room)
				room->rooms_generation = priv->rooms_generation;
		}

		const gchar *next_token;
		if (parse_string(node, "NextToken", &next_token))
			fetch_rooms(cxn, next_token);
		else {
			priv->rooms_sync = CHIME_SYNC_IDLE;

			/* Anything which *wasn't* seen this time round, but which was previously
			   in the rooms list, needs to have its 'rooms-list' flag cleared */
			GList *rooms = g_hash_table_get_values(priv->rooms_by_id);
			while (rooms) {
				room = CHIME_ROOM(rooms->data);

				if (!room->is_dead &&
				    room->rooms_generation != priv->rooms_generation) {
					/* It'll remove itself from the hash table in its ->dispose() */
					room->is_dead = TRUE;
					g_object_unref(room);
				}
				rooms = g_list_remove(rooms, room);
			}
			if (!priv->rooms_online) {
				priv->rooms_online = TRUE;
				chime_connection_calculate_online(cxn);
			}
		}
	} else {
		const gchar *reason = msg->reason_phrase;

		parse_string(node, "error", &reason);

		chime_connection_fail(cxn, CHIME_CONNECTION_ERROR_NETWORK,
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
			priv->rooms_generation++;
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
	const gchar *type;
	if (!parse_string(data_node, "type", &type))
		return FALSE;

	if (strcmp(type, "update"))
		return FALSE;

	fetch_rooms(cxn, NULL);
	return TRUE;
}

static gboolean room_jugg_cb(ChimeConnection *cxn, gpointer _unused, JsonNode *data_node)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	const gchar *type;
	if (!parse_string(data_node, "type", &type))
		return FALSE;

	if (strcmp(type, "update"))
		return FALSE;

	JsonObject *obj = json_node_get_object(data_node);
	JsonNode *record_node = json_object_get_member(obj, "record");
	if (!record_node)
		return FALSE;

	ChimeRoom *room = chime_connection_parse_room(cxn, record_node, NULL);
	if (room) {
		room->rooms_generation = priv->rooms_generation;
		return TRUE;
	}

	return FALSE;
}

static void unhash_room(gpointer _room)
{
	ChimeRoom *room = CHIME_ROOM(_room);
	if (!room->is_dead) {
		room->is_dead = TRUE;
		g_object_unref(room);
	}
}

void chime_init_rooms(ChimeConnection *cxn)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	priv->rooms_by_id = g_hash_table_new_full(g_str_hash, g_str_equal,
						     NULL, unhash_room);
	priv->rooms_by_name = g_hash_table_new(g_str_hash, g_str_equal);

	chime_jugg_subscribe(cxn, priv->profile_channel, "VisibleRooms",
			     visible_rooms_jugg_cb, NULL);
	chime_jugg_subscribe(cxn, priv->device_channel, "Room",
			     room_jugg_cb, NULL);
	fetch_rooms(cxn, NULL);
}

void chime_destroy_rooms(ChimeConnection *cxn)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	g_clear_pointer(&priv->rooms_by_name, g_hash_table_unref);
	g_clear_pointer(&priv->rooms_by_id, g_hash_table_unref);

	chime_jugg_unsubscribe(cxn, priv->profile_channel, "VisibleRooms",
			       visible_rooms_jugg_cb, NULL);
	chime_jugg_unsubscribe(cxn, priv->device_channel, "Room",
			       room_jugg_cb, NULL);
}

ChimeRoom *chime_connection_room_by_name(ChimeConnection *cxn,
					 const gchar *name)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	return g_hash_table_lookup(priv->rooms_by_name, name);
}

ChimeRoom *chime_connection_room_by_id(ChimeConnection *cxn,
				       const gchar *id)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	return g_hash_table_lookup(priv->rooms_by_id, id);
}

struct foreach_room_st {
	ChimeConnection *cxn;
	ChimeRoomCB cb;
	gpointer cbdata;
};

static void foreach_room_cb(gpointer key, gpointer value, gpointer _data)
{
	struct foreach_room_st *data = _data;
	ChimeRoom *room = CHIME_ROOM(value);

	data->cb(data->cxn, room, data->cbdata);
}

void chime_connection_foreach_room(ChimeConnection *cxn, ChimeRoomCB cb,
				      gpointer cbdata)
{
	struct foreach_room_st data = {
		.cxn = cxn,
		.cb = cb,
		.cbdata = cbdata,
	};

	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE(cxn);
	g_hash_table_foreach(priv->rooms_by_id, foreach_room_cb, &data);
}
