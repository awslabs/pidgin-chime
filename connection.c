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
    PROP_TOKEN,
    LAST_PROP
};

static GParamSpec *props[LAST_PROP];

G_DEFINE_TYPE(ChimeConnection, chime_connection, G_TYPE_OBJECT)

static void
chime_connection_finalize(GObject *object)
{
	ChimeConnection *self = CHIME_CONNECTION(object);

	g_free(self->token);
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
	case PROP_TOKEN:
		g_value_set_string(value, self->token);
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
	case PROP_TOKEN:
		self->token = g_value_dup_string(value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static JsonNode *chime_device_register_req(PurpleAccount *account)
{
	JsonBuilder *jb;
	JsonNode *jn;
	const gchar *devtoken = purple_account_get_string(account, "devtoken", NULL);

	if (!devtoken || !devtoken[0]) {
		gchar *uuid = purple_uuid_random();
		purple_account_set_string(account, "devtoken", uuid);
		g_free(uuid);
		devtoken = purple_account_get_string(account, "devtoken", NULL);
	}

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
	purple_account_set_string(self->prpl_conn->account, "token", sess_tok);
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

static void register_cb(ChimeConnection *self, SoupMessage *msg,
			JsonNode *node, gpointer _unused)
{
	if (!node) {
		purple_connection_error_reason(self->prpl_conn, PURPLE_CONNECTION_ERROR_NETWORK_ERROR, _("Device registration failed"));
		return;
	}

	self->reg_node = json_node_ref(node);
	if (!parse_regnode(self, self->reg_node)) {
		purple_connection_error_reason(self->prpl_conn, PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
					       _("Failed to process registration response"));
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
}

#define SIGNIN_DEFAULT "https://signin.id.ue1.app.chime.aws/"
static void chime_register_device(ChimeConnection *self)
{
	const gchar *server = purple_account_get_string(self->prpl_conn->account, "server", NULL);
	JsonNode *node = chime_device_register_req(self->prpl_conn->account);

	if (!server || !server[0])
		server = SIGNIN_DEFAULT;

	SoupURI *uri = soup_uri_new_printf(server, "/sessions");
	soup_uri_set_query_from_fields(uri, "Token", self->token, NULL);

	purple_connection_update_progress(self->prpl_conn, _("Connecting..."), 1, CONNECT_STEPS);
	chime_queue_http_request(self, node, uri, register_cb, NULL);
}

static void
chime_connection_constructed(GObject *object)
{
	ChimeConnection *self = CHIME_CONNECTION(object);

	if (self->token)
		chime_register_device(self);
	else
		chime_initial_login(self);

	G_OBJECT_CLASS(chime_connection_parent_class)->constructed(object);
}

static void
chime_connection_class_init(ChimeConnectionClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->finalize = chime_connection_finalize;
	object_class->dispose = chime_connection_dispose;
	object_class->get_property = chime_connection_get_property;
	object_class->set_property = chime_connection_set_property;
	object_class->constructed = chime_connection_constructed;

	props[PROP_PURPLE_CONNECTION] =
		g_param_spec_pointer("purple-connection",
				     "purple connection",
				     "purple connection",
				     G_PARAM_READWRITE |
				     G_PARAM_CONSTRUCT_ONLY |
				     G_PARAM_STATIC_STRINGS);

	props[PROP_TOKEN] =
		g_param_spec_string("token",
				    "token",
				    "token",
				    NULL,
				    G_PARAM_READWRITE |
				    G_PARAM_CONSTRUCT |
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
chime_connection_new(PurpleConnection *connection,
                     const gchar      *token)
{
	return g_object_new (CHIME_TYPE_CONNECTION,
	                     "purple-connection", connection,
	                     "token", token,
	                     NULL);
}
