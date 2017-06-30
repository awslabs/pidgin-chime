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

#include "connection.h"
#include "chime.h"

#include <glib/gi18n.h>

enum
{
    PROP_0,
    PROP_PURPLE_CONNECTION,
    LAST_PROP
};

static GParamSpec *props[LAST_PROP];

G_DEFINE_QUARK(chime-connection-error-quark, chime_connection_error)
G_DEFINE_TYPE(ChimeConnection, chime_connection, G_TYPE_OBJECT)

static void
chime_connection_finalize(GObject *object)
{
	ChimeConnection *self = CHIME_CONNECTION(object);

	g_free(self->session_token);

	G_OBJECT_CLASS(chime_connection_parent_class)->finalize(object);
}

static void
chime_connection_dispose(GObject *object)
{
	ChimeConnection *self = CHIME_CONNECTION(object);
	GList *l;

	if (self->soup_sess) {
		soup_session_abort(self->soup_sess);
	}
	g_clear_object(&self->soup_sess);

	chime_destroy_juggernaut(self);
	chime_destroy_buddies(self);
	chime_destroy_rooms(self);
	chime_destroy_conversations(self);
	chime_destroy_chats(self);

	g_clear_pointer(&self->reg_node, json_node_unref);

	// FIXME: change to use g_list_free_full
	while ( (l = g_list_first(self->msg_queue)) ) {
		struct chime_msg *cmsg = l->data;

		g_object_unref(cmsg->msg);
		g_free(cmsg);
		self->msg_queue = g_list_remove(self->msg_queue, cmsg);
	}
	self->msg_queue = NULL;
	
	purple_connection_set_protocol_data(self->prpl_conn, NULL);

	G_OBJECT_CLASS(chime_connection_parent_class)->dispose(object);
}

static void
chime_connection_get_property(GObject    *object,
                              guint       prop_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
	ChimeConnection *self = CHIME_CONNECTION(object);

	switch (prop_id) {
	case PROP_PURPLE_CONNECTION:
		g_value_set_pointer(value, self->prpl_conn);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void
chime_connection_set_property(GObject      *object,
                              guint         prop_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
	ChimeConnection *self = CHIME_CONNECTION(object);

	switch (prop_id) {
	case PROP_PURPLE_CONNECTION:
		self->prpl_conn = g_value_get_pointer(value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void
chime_connection_class_init(ChimeConnectionClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->finalize = chime_connection_finalize;
	object_class->dispose = chime_connection_dispose;
	object_class->get_property = chime_connection_get_property;
	object_class->set_property = chime_connection_set_property;

	props[PROP_PURPLE_CONNECTION] =
		g_param_spec_pointer("purple-connection",
				     "purple connection",
				     "purple connection",
				     G_PARAM_READWRITE |
				     G_PARAM_CONSTRUCT_ONLY |
				     G_PARAM_STATIC_STRINGS);

	g_object_class_install_properties(object_class, LAST_PROP, props);
}

static void
chime_connection_init(ChimeConnection *self)
{
	self->soup_sess = soup_session_new();

	if (getenv("CHIME_DEBUG") && atoi(getenv("CHIME_DEBUG")) > 0) {
		SoupLogger *l = soup_logger_new(SOUP_LOGGER_LOG_BODY, -1);
		soup_session_add_feature(self->soup_sess, SOUP_SESSION_FEATURE(l));
		g_object_unref(l);
		g_object_set(self->soup_sess, "ssl-strict", FALSE, NULL);
	}
}

ChimeConnection *
chime_connection_new(PurpleConnection *connection)
{
	return g_object_new (CHIME_TYPE_CONNECTION,
	                     "purple-connection", connection,
	                     NULL);
}

static gboolean parse_regnode(ChimeConnection *self, JsonNode *regnode)
{
	JsonObject *obj = json_node_get_object(self->reg_node);
	JsonNode *node, *sess_node = json_object_get_member(obj, "Session");
	const gchar *sess_tok;
	const gchar *display_name;

	if (!sess_node)
		return FALSE;
	if (!parse_string(sess_node, "SessionToken", &sess_tok))
		return FALSE;

	self->session_token = g_strdup(sess_tok);

	if (!parse_string(sess_node, "SessionId", &self->session_id))
		return FALSE;

	obj = json_node_get_object(sess_node);

	node = json_object_get_member(obj, "Profile");
	if (!parse_string(node, "profile_channel", &self->profile_channel) ||
	    !parse_string(node, "presence_channel", &self->presence_channel) ||
	    !parse_string(node, "id", &self->profile_id) ||
	    !parse_string(node, "display_name", &display_name))
		return FALSE;

	purple_connection_set_display_name(self->prpl_conn, display_name);

	node = json_object_get_member(obj, "Device");
	if (!parse_string(node, "DeviceId", &self->device_id) ||
	    !parse_string(node, "Channel", &self->device_channel))
		return FALSE;

	node = json_object_get_member(obj, "ServiceConfig");
	if (!node)
		return FALSE;
	obj = json_node_get_object(node);

	node = json_object_get_member(obj, "Presence");
	if (!parse_string(node, "RestUrl", &self->presence_url))
		return FALSE;

	node = json_object_get_member(obj, "Push");
	if (!parse_string(node, "ReachabilityUrl", &self->reachability_url) ||
	    !parse_string(node, "WebsocketUrl", &self->websocket_url))
		return FALSE;

	node = json_object_get_member(obj, "Profile");
	if (!parse_string(node, "RestUrl", &self->profile_url))
		return FALSE;

	node = json_object_get_member(obj, "Contacts");
	if (!parse_string(node, "RestUrl", &self->contacts_url))
		return FALSE;

	node = json_object_get_member(obj, "Messaging");
	if (!parse_string(node, "RestUrl", &self->messaging_url))
		return FALSE;

	node = json_object_get_member(obj, "Presence");
	if (!parse_string(node, "RestUrl", &self->presence_url))
		return FALSE;

	node = json_object_get_member(obj, "Conference");
	if (!parse_string(node, "RestUrl", &self->conference_url))
		return FALSE;

	return TRUE;
}

static JsonNode *
chime_device_register_req(const gchar *devtoken)
{
	JsonBuilder *jb;
	JsonNode *jn;

	jb = json_builder_new();
	jb = json_builder_begin_object(jb);
	jb = json_builder_set_member_name(jb, "Device");
	jb = json_builder_begin_object(jb);
	jb = json_builder_set_member_name(jb, "Platform");
	jb = json_builder_add_string_value(jb, "osx");
	jb = json_builder_set_member_name(jb, "DeviceToken");
	jb = json_builder_add_string_value(jb, devtoken);
	jb = json_builder_set_member_name(jb, "Capabilities");
	jb = json_builder_add_int_value(jb, CHIME_DEVICE_CAP_PUSH_DELIVERY_RECEIPTS |
					CHIME_DEVICE_CAP_PRESENCE_PUSH |
					CHIME_DEVICE_CAP_PRESENCE_SUBSCRIPTION);
	jb = json_builder_end_object(jb);
	jb = json_builder_end_object(jb);
	jn = json_builder_get_root(jb);
	g_object_unref(jb);

	return jn;
}

static void register_cb(ChimeConnection *self, SoupMessage *msg,
			JsonNode *node, gpointer user_data)
{
	GTask *task = G_TASK(user_data);

	if (!node) {
		g_task_return_new_error(task, CHIME_CONNECTION_ERROR, CHIME_CONNECTION_ERROR_NETWORK,
		                        _("Device registration failed"));
		g_object_unref(task);
		return;
	}

	self->reg_node = json_node_ref(node);
	if (!parse_regnode(self, self->reg_node)) {
		g_task_return_new_error(task, CHIME_CONNECTION_ERROR, CHIME_CONNECTION_ERROR_NETWORK,
		                        _("Failed to process registration response"));
		g_object_unref(task);
		return;
	}

	chime_init_juggernaut(self);

	chime_jugg_subscribe(self, self->profile_channel, NULL, NULL, NULL);
	chime_jugg_subscribe(self, self->presence_channel, NULL, NULL, NULL);
	chime_jugg_subscribe(self, self->device_channel, NULL, NULL, NULL);

	chime_init_buddies(self);
	chime_init_rooms(self);
	chime_init_conversations(self);
	chime_init_chats(self);

	g_task_return_boolean(task, TRUE);
	g_object_unref(task);
}

void
chime_connection_register_device_async(ChimeConnection    *self,
                                       const gchar        *server,
                                       const gchar        *token,
                                       const gchar        *devtoken,
                                       GCancellable       *cancellable,
                                       GAsyncReadyCallback callback,
                                       gpointer            user_data)
{
	g_return_if_fail(CHIME_IS_CONNECTION(self));
	g_return_if_fail(server != NULL);
	g_return_if_fail(token != NULL);
	g_return_if_fail(devtoken != NULL);

	GTask *task = g_task_new(self, cancellable, callback, user_data);

	JsonNode *node = chime_device_register_req(devtoken);

	SoupURI *uri = soup_uri_new_printf(server, "/sessions");
	soup_uri_set_query_from_fields(uri, "Token", token, NULL);

	chime_queue_http_request(self, node, uri, "POST", register_cb, task);
}

gboolean
chime_connection_register_device_finish(ChimeConnection  *self,
                                        GAsyncResult     *result,
                                        GError          **error)
{
	g_return_val_if_fail(CHIME_IS_CONNECTION(self), FALSE);
	g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

	return g_task_propagate_boolean(G_TASK(result), error);
}

const gchar *
chime_connection_get_session_token(ChimeConnection *self)
{
	g_return_val_if_fail(CHIME_IS_CONNECTION(self), NULL);

	return self->session_token;
}
