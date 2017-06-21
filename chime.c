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

#define PURPLE_PLUGINS

#include <prpl.h>
#include <version.h>
#include <accountopt.h>
#include <status.h>
#include <roomlist.h>

#include <glib/gi18n.h>
#include <glib/gstrfuncs.h>
#include <glib/gquark.h>

#include <json-glib/json-glib.h>

#include <libsoup/soup.h>

#include "chime.h"

G_DEFINE_QUARK(PidginChimeError, pidgin_chime_error);

SoupURI *soup_uri_new_printf(const gchar *base, const gchar *format, ...)
{
	SoupURI *uri;
	va_list args;
	gchar *constructed;
	gchar *append;

	va_start(args, format);
	append = g_strdup_vprintf(format, args);
	va_end(args);
	constructed = g_strdup_printf("%s%s%s", base,
				      g_str_has_suffix(base, "/") ? "" : "/",
				      append[0] == '/' ? append + 1 : append);
	uri = soup_uri_new(constructed);
	g_free(constructed);
	g_free(append);
	return uri;
}
/* Helper function to get a string from a JSON child node */
gboolean parse_string(JsonNode *parent, const gchar *name, const gchar **res)
{
	JsonObject *obj;
	JsonNode *node;
	const gchar *str;

	if (!parent)
		return FALSE;

	obj = json_node_get_object(parent);
	if (!obj)
		return FALSE;

	node = json_object_get_member(obj, name);
	if (!node)
		return FALSE;

	str = json_node_get_string(node);
	if (!str)
		return FALSE;

	*res = str;
	printf("Got %s = %s\n", name, str);
	return TRUE;
}

static JsonNode *process_soup_response(SoupMessage *msg, GError **error)
{
	const gchar *content_type;
	JsonParser *parser;
	JsonNode *node;

	if (msg->status_code != 200 && msg->status_code != 201) {
		g_set_error(error, CHIME_ERROR,
			    CHIME_ERROR_REQUEST_FAILED,
			    _("Request failed(%d): %s"),
			    msg->status_code, msg->reason_phrase);
		return NULL;
	}

	content_type = soup_message_headers_get_content_type(msg->response_headers, NULL);
	if (!content_type || strcmp(content_type, "application/json")) {
		g_set_error(error, CHIME_ERROR,
			    CHIME_ERROR_BAD_RESPONSE,
			    _("Server sent wrong content-type '%s'"),
			    content_type);
		return NULL;
	}
	parser = json_parser_new();
	if (!json_parser_load_from_data(parser, msg->response_body->data, msg->response_body->length,
					error)) {
		g_object_unref(parser);
		return NULL;
	}

	node = json_node_ref(json_parser_get_root(parser));
	g_object_unref(parser);
	return node;
}

static void chime_renew_token(struct chime_connection *cxn);

static void soup_msg_cb(SoupSession *soup_sess, SoupMessage *msg, gpointer _cmsg)
{
	struct chime_msg *cmsg = _cmsg;
	struct chime_connection *cxn = cmsg->cxn;
	JsonParser *parser = NULL;
	JsonNode *node = NULL;

	if (cmsg->auto_renew && msg->status_code == 401) {
		g_object_ref(msg);
		if (cxn->msg_queue) {
			/* Already renewing; just add to the list */
			cxn->msg_queue = g_list_append(cxn->msg_queue, cmsg);
			return;
		}

		cxn->msg_queue = g_list_append(cxn->msg_queue, cmsg);
		chime_renew_token(cxn);
		return;
	}

	const gchar *content_type = soup_message_headers_get_content_type(msg->response_headers, NULL);
	if (content_type && !strcmp(content_type, "application/json")) {
		parser = json_parser_new();
		if (json_parser_load_from_data(parser, msg->response_body->data, msg->response_body->length, NULL))
			node = json_parser_get_root(parser);
	}

	cmsg->cb(cmsg->cxn, msg, node, cmsg->cb_data);
	if (parser)
		g_object_unref(parser);
	g_free(cmsg);
}

SoupMessage *chime_queue_http_request(struct chime_connection *cxn, JsonNode *node,
				      SoupURI *uri, ChimeSoupMessageCallback callback,
				      gpointer cb_data, gboolean auto_renew)
{
	struct chime_msg *cmsg = g_new0(struct chime_msg, 1);

	cmsg->cxn = cxn;
	cmsg->cb = callback;
	cmsg->cb_data = cb_data;
	cmsg->auto_renew = auto_renew;
	cmsg->msg = soup_message_new_from_uri(node?"POST":"GET", uri);
	soup_uri_free(uri);

	if (cxn->session_token) {
		gchar *cookie = g_strdup_printf("_aws_wt_session=%s", cxn->session_token);
		soup_message_headers_append(cmsg->msg->request_headers, "Cookie", cookie);
		g_free(cookie);
	}
	soup_message_headers_append(cmsg->msg->request_headers, "Accept", "*/*");
	if (node) {
		gchar *body;
		gsize body_size;
		JsonGenerator *gen = json_generator_new();
		json_generator_set_root(gen, node);
		body = json_generator_to_data(gen, &body_size);
		soup_message_set_request(cmsg->msg, "application/json",
					 SOUP_MEMORY_TAKE,
					 body, body_size);
		g_object_unref(gen);
		json_node_unref(node);
	}

	soup_session_queue_message(cxn->soup_sess, cmsg->msg, soup_msg_cb, cmsg);

	return cmsg->msg;
}

static void renew_cb(struct chime_connection *cxn, SoupMessage *msg,
		     JsonNode *node, gpointer _unused)
{
	GError *error = NULL;
	GList *l;
	const gchar *sess_tok;
	JsonNode *tok_node;
	gchar *cookie_hdr;

	if (!node || !parse_string(node, "SessionToken", &sess_tok)) {
		purple_connection_error_reason(cxn->prpl_conn, PURPLE_CONNECTION_ERROR_NETWORK_ERROR, _("Failed to renew session token"));
		return;
	}

	purple_account_set_string(cxn->prpl_conn->account, "token", sess_tok);
	g_free(cxn->session_token);
	cxn->session_token = g_strdup(sess_tok);

	cookie_hdr = g_strdup_printf("_aws_wt_session=%s", cxn->session_token);

	while ( (l = g_list_first(cxn->msg_queue)) ) {
		struct chime_msg *cmsg = l->data;

		soup_message_headers_replace(cmsg->msg->request_headers, "Cookie", cookie_hdr);
		soup_session_queue_message(cxn->soup_sess, cmsg->msg, soup_msg_cb, cmsg);
		cxn->msg_queue = g_list_remove(cxn->msg_queue, cmsg);
	}

	g_free(cookie_hdr);
}

static void chime_renew_token(struct chime_connection *cxn)
{
	SoupURI *uri;
	JsonBuilder *builder;
	JsonNode *node;

	builder = json_builder_new();
	builder = json_builder_begin_object(builder);
	builder = json_builder_set_member_name(builder, "Token");
	builder = json_builder_add_string_value(builder, cxn->session_token);
	builder = json_builder_end_object(builder);
	node = json_builder_get_root(builder);
	g_object_unref(builder);

	uri = soup_uri_new_printf(cxn->profile_url, "/tokens");
	soup_uri_set_query_from_fields(uri, "Token", cxn->session_token, NULL);
	chime_queue_http_request(cxn, node, uri, renew_cb, NULL, FALSE);
}


static gboolean chime_purple_plugin_load(PurplePlugin *plugin)
{
	printf("Chime plugin load\n");
	purple_notify_message(plugin, PURPLE_NOTIFY_MSG_INFO, "Foo",
			      "Chime plugin starting...", NULL, NULL, NULL);
	return TRUE;
}

static gboolean chime_purple_plugin_unload(PurplePlugin *plugin)
{
	return TRUE;
}

void chime_purple_plugin_destroy(PurplePlugin *plugin)
{
}
const char *chime_purple_list_icon(PurpleAccount *a, PurpleBuddy *b)
{
        return "chime";
}

static JsonNode *chime_device_register_req(PurpleAccount *account)
{
	JsonBuilder *jb;
	JsonNode *jn;

	jb = json_builder_new();
	jb = json_builder_begin_object(jb);
	jb = json_builder_set_member_name(jb, "Device");
	jb = json_builder_begin_object(jb);
	jb = json_builder_set_member_name(jb, "Platform");
	jb = json_builder_add_string_value(jb, "android");
	jb = json_builder_set_member_name(jb, "DeviceToken");
	jb = json_builder_add_string_value(jb, "not-a-real-device-not-even-android");
	jb = json_builder_set_member_name(jb, "UaChannelToken");
	jb = json_builder_add_string_value(jb, "blah42");
	jb = json_builder_set_member_name(jb, "Capabilities");
	jb = json_builder_add_int_value(jb, 1234);
	jb = json_builder_end_object(jb);
	jb = json_builder_end_object(jb);
	jn = json_builder_get_root(jb);
	g_object_unref(jb);

	return jn;
}

static gboolean parse_regnode(struct chime_connection *cxn, JsonNode *regnode)
{
	JsonObject *obj = json_node_get_object(cxn->reg_node);
	JsonNode *node, *sess_node = json_object_get_member(obj, "Session");
	const gchar *str, *sess_tok;

	if (!sess_node)
		return FALSE;
	if (!parse_string(sess_node, "SessionToken", &sess_tok))
		return FALSE;
	purple_account_set_string(cxn->prpl_conn->account, "token", sess_tok);
	cxn->session_token = g_strdup(sess_tok);

	if (!parse_string(sess_node, "SessionId", &cxn->session_id))
		return FALSE;

	obj = json_node_get_object(sess_node);

	node = json_object_get_member(obj, "Profile");
	if (!parse_string(node, "profile_channel", &cxn->profile_channel) ||
	    !parse_string(node, "presence_channel", &cxn->presence_channel))
		return FALSE;

	node = json_object_get_member(obj, "Device");
	if (!parse_string(node, "DeviceId", &cxn->device_id) ||
	    !parse_string(node, "Channel", &cxn->device_channel))
		return FALSE;

	node = json_object_get_member(obj, "ServiceConfig");
	if (!node)
		return FALSE;
	obj = json_node_get_object(node);

	node = json_object_get_member(obj, "Presence");
	if (!parse_string(node, "RestUrl", &cxn->presence_url))
		return FALSE;

	node = json_object_get_member(obj, "Push");
	if (!parse_string(node, "ReachabilityUrl", &cxn->reachability_url) ||
	    !parse_string(node, "WebsocketUrl", &cxn->websocket_url))
		return FALSE;

	node = json_object_get_member(obj, "Profile");
	if (!parse_string(node, "RestUrl", &cxn->profile_url))
		return FALSE;

	node = json_object_get_member(obj, "Contacts");
	if (!parse_string(node, "RestUrl", &cxn->contacts_url))
		return FALSE;

	node = json_object_get_member(obj, "Messaging");
	if (!parse_string(node, "RestUrl", &cxn->messaging_url))
		return FALSE;

	node = json_object_get_member(obj, "Presence");
	if (!parse_string(node, "RestUrl", &cxn->presence_url))
		return FALSE;

	node = json_object_get_member(obj, "Conference");
	if (!parse_string(node, "RestUrl", &cxn->conference_url))
		return FALSE;

	return TRUE;
}

static void register_cb(struct chime_connection *cxn, SoupMessage *msg,
			JsonNode *node, gpointer _unused)
{
	if (!node) {
		purple_connection_error_reason(cxn->prpl_conn, PURPLE_CONNECTION_ERROR_NETWORK_ERROR, _("Device registration failed"));
		return;
	}

	cxn->reg_node = json_node_ref(node);
	if (!parse_regnode(cxn, cxn->reg_node)) {
		purple_connection_error_reason(cxn->prpl_conn, PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
					       _("Failed to process registration response"));
		return;
	}

	chime_init_juggernaut(cxn);

	chime_jugg_subscribe(cxn, cxn->profile_channel, jugg_dump_incoming, "Profile");
	chime_jugg_subscribe(cxn, cxn->presence_channel, jugg_dump_incoming, "Presence");
	chime_jugg_subscribe(cxn, cxn->device_channel, jugg_dump_incoming, "Device");

	chime_init_buddies(cxn);
}

#define SIGNIN_DEFAULT "https://signin.id.ue1.app.chime.aws/"
void chime_purple_login(PurpleAccount *account)
{
	PurpleConnection *conn = purple_account_get_connection(account);
	struct chime_connection *cxn;
	const gchar *token = purple_account_get_string(account, "token",
						       NULL);
	JsonNode *node;
	SoupURI *uri;

	if (!token) {
		purple_connection_error(conn,
					_("No authentication token"));
		return;
	}

	cxn = g_new0(struct chime_connection, 1);
	purple_connection_set_protocol_data(conn, cxn);
	cxn->prpl_conn = conn;
	cxn->soup_sess = soup_session_new();
	printf("conn %p cxn %p\n", conn, cxn);
	if (getenv("CHIME_DEBUG") && atoi(getenv("CHIME_DEBUG")) > 0) {
		SoupLogger *l = soup_logger_new(SOUP_LOGGER_LOG_BODY, -1);
		soup_session_add_feature(cxn->soup_sess, SOUP_SESSION_FEATURE(l));
		g_object_unref(l);
		g_object_set(cxn->soup_sess, "ssl-strict", FALSE, NULL);
	}
	node = chime_device_register_req(account);
	uri = soup_uri_new_printf(purple_account_get_string(account, "server", SIGNIN_DEFAULT),
				  "/sessions");
	soup_uri_set_query_from_fields(uri, "Token", token, NULL);

	purple_connection_update_progress(conn, _("Connecting..."), 1, CONNECT_STEPS);
	chime_queue_http_request(cxn, node, uri, register_cb, NULL, TRUE);
}

void chime_purple_close(PurpleConnection *conn)
{
	struct chime_connection *cxn = purple_connection_get_protocol_data(conn);
	GList *l;

	if (cxn) {
		if (cxn->soup_sess) {
			soup_session_abort(cxn->soup_sess);
			g_object_unref(cxn->soup_sess);
			cxn->soup_sess = NULL;
		}
		chime_destroy_juggernaut(cxn);
		chime_destroy_buddies(cxn);

		if (cxn->reg_node) {
			json_node_unref(cxn->reg_node);
			cxn->reg_node = NULL;
		}
		while ( (l = g_list_first(cxn->msg_queue)) ) {
			struct chime_msg *cmsg = l->data;

			g_object_unref(cmsg->msg);
			g_free(cmsg);
			cxn->msg_queue = g_list_remove(cxn->msg_queue, cmsg);
		}
		cxn->msg_queue = NULL;
		if (cxn->session_token) {
			g_free(cxn->session_token);
			cxn->session_token = NULL;
		}
		purple_connection_set_protocol_data(cxn->prpl_conn, NULL);
		g_free(cxn);
	}
	printf("Chime close\n");
}

GList *chime_purple_chat_info(PurpleConnection *conn)
{
	struct proto_chat_entry *pce = g_new0(struct proto_chat_entry, 1);

	pce->label = _("Name:");
	pce->identifier = "Name";
	pce->required = TRUE;

	return g_list_append(NULL, pce);
}

GHashTable *chime_purple_chat_info_defaults(PurpleConnection *conn, const char *name)
{
	printf("chat_info_defaults %s\n", name);
	return NULL;
}

void chime_purple_join_chat(PurpleConnection *conn, GHashTable *data)
{
	const gchar *roomid = g_hash_table_lookup(data, "RoomId");
	printf("join_chat %p %s\n", data, roomid);
	//	PurpleConversation *conv = serv_got_joined_chat(conn, 0x1234, g_hash_table_lookup(data, "name"));
}

GList *chime_purple_status_types(PurpleAccount *account)
{
	PurpleStatusType *type;
	GList *types = NULL;

	type = purple_status_type_new(PURPLE_STATUS_OFFLINE, "1",
				      _("Offline"), TRUE);
	types = g_list_append(types, type);
	type = purple_status_type_new(PURPLE_STATUS_MOBILE, "2",
				      _("Mobile"), TRUE);
	types = g_list_append(types, type);
	type = purple_status_type_new(PURPLE_STATUS_UNAVAILABLE, "4",
				      _("Busy"), TRUE);
	types = g_list_append(types, type);
	type = purple_status_type_new(PURPLE_STATUS_AVAILABLE, "5",
				      _("Available"), TRUE);
	types = g_list_append(types, type);

	return types;
}

static int chime_purple_send_im(PurpleConnection *gc,
				const char *who,
				const char *what,
				PurpleMessageFlags flags)
{

	printf("send %s to %s\n", what, who);
	return 1;
}
static void room_cb(JsonArray *array, guint index_,
                     JsonNode *node, gpointer _roomlist)
{
	PurpleRoomlist *roomlist = _roomlist;
	PurpleRoomlistRoom *room;
	const gchar *name, *id, *visibility, *privacy;

	if (!parse_string(node, "Name", &name) ||
	    !parse_string(node, "RoomId", &id))
		return;

	room = purple_roomlist_room_new(PURPLE_ROOMLIST_ROOMTYPE_ROOM, name, NULL);
	purple_roomlist_room_add_field(roomlist, room, id);
	purple_roomlist_room_add_field(roomlist, room,
				       GUINT_TO_POINTER(!parse_string(node, "Visibility", &visibility) ||
							!strcmp(visibility, "visible")));
	purple_roomlist_room_add_field(roomlist, room,
				       GUINT_TO_POINTER(!parse_string(node, "Privacy", &privacy) ||
							!strcmp(privacy, "private")));
	purple_roomlist_room_add(roomlist, room);
}

struct roomlist_data {
	struct chime_connection *cxn;
	SoupMessage *msg;
};

static void roomlist_cb(struct chime_connection *cxn, SoupMessage *msg,
			JsonNode *node, gpointer _roomlist)
{
	PurpleRoomlist *roomlist = _roomlist;
	GError *error = NULL;

	if (node) {
		JsonNode *rooms_node;
		JsonObject *obj;
		JsonArray *rooms_arr;

		obj = json_node_get_object(node);
		rooms_node = json_object_get_member(obj, "Rooms");
		if (rooms_node) {
			rooms_arr = json_node_get_array(rooms_node);
			json_array_foreach_element(rooms_arr, room_cb, roomlist);
		}
	}
	purple_roomlist_set_in_progress(roomlist, FALSE);
	g_free(roomlist->proto_data);
	roomlist->proto_data = NULL;
	purple_roomlist_unref(roomlist);
}

static PurpleRoomlist *chime_purple_roomlist_get_list(PurpleConnection *conn)
{
	struct chime_connection *cxn = purple_connection_get_protocol_data(conn);
	struct roomlist_data *d = g_new0(struct roomlist_data, 1);
	SoupURI *uri;
	PurpleRoomlist *roomlist;
	GList *fields = NULL;

	roomlist = purple_roomlist_new(conn->account);
	fields = g_list_append(fields, purple_roomlist_field_new(PURPLE_ROOMLIST_FIELD_STRING, "", "RoomId", TRUE));
	fields = g_list_append(fields, purple_roomlist_field_new(PURPLE_ROOMLIST_FIELD_BOOL, _("Visible"), "Visibility", FALSE));
	fields = g_list_append(fields, purple_roomlist_field_new(PURPLE_ROOMLIST_FIELD_BOOL, _("Private"), "Privacy", FALSE));

	purple_roomlist_set_fields(roomlist, fields);
	purple_roomlist_set_in_progress(roomlist, TRUE);
	roomlist->proto_data = d;
	d->cxn = cxn;
	uri = soup_uri_new_printf(cxn->messaging_url, "/rooms");
	d->msg = chime_queue_http_request(cxn, NULL, uri, roomlist_cb, roomlist, TRUE);

	return roomlist;
}

void chime_purple_roomlist_cancel(PurpleRoomlist *roomlist)
{
	struct roomlist_data *d = roomlist->proto_data;

	soup_session_cancel_message(d->cxn->soup_sess, d->msg, 1);
}

static PurplePluginProtocolInfo chime_prpl_info = {
	.options = OPT_PROTO_NO_PASSWORD,
	.list_icon = chime_purple_list_icon,
	.login = chime_purple_login,
	.close = chime_purple_close,
	.status_types = chime_purple_status_types,
	.send_im = chime_purple_send_im,
	.chat_info = chime_purple_chat_info,
	.join_chat = chime_purple_join_chat,
	.roomlist_get_list = chime_purple_roomlist_get_list,
	.roomlist_cancel = chime_purple_roomlist_cancel,
	.chat_info_defaults = chime_purple_chat_info_defaults,
	.add_buddy = chime_purple_add_buddy,
	.buddy_free = chime_purple_buddy_free,
	.remove_buddy = chime_purple_remove_buddy,
};

static void chime_purple_show_about_plugin(PurplePluginAction *action)
{
	purple_notify_formatted(action->context,
				NULL, _("Foo"), NULL, _("Hello"),
				NULL, NULL);
}

static GList *chime_purple_plugin_actions(PurplePlugin *plugin,
					  gpointer context)
{
	PurplePluginAction *act;
	GList *acts = NULL;
	printf("chime acts\n");
	act = purple_plugin_action_new(_("About Chime plugin..."),
				       chime_purple_show_about_plugin);
	acts = g_list_append(acts, act);

	return acts;
}

static PurplePluginInfo chime_plugin_info = {
	.magic = PURPLE_PLUGIN_MAGIC,
	.major_version = PURPLE_MAJOR_VERSION,
	.minor_version = PURPLE_MINOR_VERSION,
	.type = PURPLE_PLUGIN_PROTOCOL,
	.priority = PURPLE_PRIORITY_DEFAULT,
	.id = "prpl-chime",
	.name = "Amazon Chime",
	.version = PACKAGE_VERSION,
	.summary = "Amazon Chime Protocol Plugin",
	.description = "A plugin for Chime",
	.author = "David Woodhouse <dwmw2@infradead.org>",
	.load = chime_purple_plugin_load,
	.unload = chime_purple_plugin_unload,
	.destroy = chime_purple_plugin_destroy,
	.extra_info = &chime_prpl_info,
	.actions = chime_purple_plugin_actions,
};

static void chime_purple_init_plugin(PurplePlugin *plugin)
{
	PurpleAccountOption *opt;
	GList *opts = NULL;

	opt = purple_account_option_string_new(_("Signin URL"),
					       "server", NULL);
	opts = g_list_append(opts, opt);

	opt = purple_account_option_string_new(_("Token"), "token", NULL);
	opts = g_list_append(opts, opt);

	chime_prpl_info.protocol_options = opts;
}

PURPLE_INIT_PLUGIN(chime, chime_purple_init_plugin, chime_plugin_info);
