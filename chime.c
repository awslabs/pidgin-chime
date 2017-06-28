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

#include <glib/gi18n.h>
#include <glib/gstrfuncs.h>
#include <glib/gquark.h>

#include <json-glib/json-glib.h>

#include <libsoup/soup.h>

#include "chime.h"

static void soup_msg_cb(SoupSession *soup_sess, SoupMessage *msg, gpointer _cmsg);

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

gboolean parse_int(JsonNode *node, const gchar *member, gint64 *val)
{
	node = json_object_get_member(json_node_get_object(node), member);
	if (!node)
		return FALSE;
	*val = json_node_get_int(node);
	return TRUE;
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

gboolean parse_time(JsonNode *parent, const gchar *name, const gchar **time_str, GTimeVal *tv)
{
	const gchar *msg_time;

	if (!parse_string(parent, name, &msg_time) ||
	    !g_time_val_from_iso8601(msg_time, tv))
		return FALSE;

	if (time_str)
		*time_str = msg_time;

	return TRUE;
}
/* If we get an auth failure on a standard request, we automatically attempt
 * to renew the authentication token and resubmit the request. */
static void renew_cb(struct chime_connection *cxn, SoupMessage *msg,
		     JsonNode *node, gpointer _unused)
{
	GList *l;
	const gchar *sess_tok;
	gchar *cookie_hdr;

	if (!node || !parse_string(node, "SessionToken", &sess_tok)) {
		purple_connection_error_reason(cxn->prpl_conn, PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
					       _("Failed to renew session token"));
		/* No need to cancel the outstanding requests; the session
		   will be torn down anyway. */
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

	uri = soup_uri_new_printf(cxn->profile_url, "/tokens");
	soup_uri_set_query_from_fields(uri, "Token", cxn->session_token, NULL);
	__chime_queue_http_request(cxn, node, uri, renew_cb, NULL, FALSE);

	g_object_unref(builder);
}

/* First callback for SoupMessage completion — do the common
 * parsing of the JSON response (if any) and hand it on to the
 * real callback function. Also handles auth token renewal. */
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
		GError *error = NULL;

		parser = json_parser_new();
		if (!json_parser_load_from_data(parser, msg->response_body->data, msg->response_body->length, &error)) {
			g_warning("Error loading data: %s", error->message);
			g_error_free(error);
		} else {
			node = json_parser_get_root(parser);
		}
	}

	if (cmsg->cb)
		cmsg->cb(cmsg->cxn, msg, node, cmsg->cb_data);
	g_clear_object(&parser);
	g_free(cmsg);
}


SoupMessage *__chime_queue_http_request(struct chime_connection *cxn, JsonNode *node,
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
	soup_message_headers_append(cmsg->msg->request_headers, "User-Agent", "Pidgin-Chime " PACKAGE_VERSION);
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

static gboolean chime_purple_plugin_load(PurplePlugin *plugin)
{
	printf("Chime plugin load\n");
	setvbuf(stdout, NULL, _IONBF, 0);
	purple_notify_message(plugin, PURPLE_NOTIFY_MSG_INFO, "Foo",
			      "Chime plugin starting...", NULL, NULL, NULL);
	return TRUE;
}

static gboolean chime_purple_plugin_unload(PurplePlugin *plugin)
{
	return TRUE;
}

static void chime_purple_plugin_destroy(PurplePlugin *plugin)
{
}

static const char *chime_purple_list_icon(PurpleAccount *a, PurpleBuddy *b)
{
        return "chime";
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

static gboolean parse_regnode(struct chime_connection *cxn, JsonNode *regnode)
{
	JsonObject *obj = json_node_get_object(cxn->reg_node);
	JsonNode *node, *sess_node = json_object_get_member(obj, "Session");
	const gchar *sess_tok;
	const gchar *display_name;

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
	    !parse_string(node, "presence_channel", &cxn->presence_channel) ||
	    !parse_string(node, "id", &cxn->profile_id) ||
	    !parse_string(node, "display_name", &display_name))
		return FALSE;

	purple_connection_set_display_name(cxn->prpl_conn, display_name);

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

	chime_jugg_subscribe(cxn, cxn->profile_channel, NULL, jugg_dump_incoming, (gpointer)"Profile");
	chime_jugg_subscribe(cxn, cxn->presence_channel, NULL, jugg_dump_incoming, (gpointer)"Presence");
	chime_jugg_subscribe(cxn, cxn->device_channel, NULL, jugg_dump_incoming, (gpointer)"Device");

	chime_init_buddies(cxn);
	chime_init_rooms(cxn);
	chime_init_conversations(cxn);
	chime_init_chats(cxn);
}

#define SIGNIN_DEFAULT "https://signin.id.ue1.app.chime.aws/"
void chime_register_device(struct chime_connection *cxn, const gchar *token)
{
	const gchar *server = purple_account_get_string(cxn->prpl_conn->account, "server", NULL);
	JsonNode *node = chime_device_register_req(cxn->prpl_conn->account);

	if (!server || !server[0])
		server = SIGNIN_DEFAULT;

	SoupURI *uri = soup_uri_new_printf(server, "/sessions");
	soup_uri_set_query_from_fields(uri, "Token", token, NULL);

	purple_connection_update_progress(cxn->prpl_conn, _("Connecting..."), 1, CONNECT_STEPS);
	chime_queue_http_request(cxn, node, uri, register_cb, NULL);
}

static void chime_purple_login(PurpleAccount *account)
{
	PurpleConnection *conn = purple_account_get_connection(account);
	struct chime_connection *cxn;

	cxn = g_new0(struct chime_connection, 1);
	purple_connection_set_protocol_data(conn, cxn);
	cxn->prpl_conn = conn;
	cxn->soup_sess = soup_session_new();

	if (getenv("CHIME_DEBUG") && atoi(getenv("CHIME_DEBUG")) > 0) {
		SoupLogger *l = soup_logger_new(SOUP_LOGGER_LOG_BODY, -1);
		soup_session_add_feature(cxn->soup_sess, SOUP_SESSION_FEATURE(l));
		g_object_unref(l);
		g_object_set(cxn->soup_sess, "ssl-strict", FALSE, NULL);
	}

	const gchar *token = purple_account_get_string(account, "token",
						       NULL);
	if (token)
		chime_register_device(cxn, token);
	else
		chime_initial_login(cxn);
}

static void chime_purple_close(PurpleConnection *conn)
{
	struct chime_connection *cxn = purple_connection_get_protocol_data(conn);
	GList *l;

	if (cxn) {
		if (cxn->soup_sess) {
			soup_session_abort(cxn->soup_sess);
		}
		g_clear_object(&cxn->soup_sess);

		chime_destroy_juggernaut(cxn);
		chime_destroy_buddies(cxn);
		chime_destroy_rooms(cxn);
		chime_destroy_conversations(cxn);
		chime_destroy_chats(cxn);

		g_clear_pointer(&cxn->reg_node, json_node_unref);

		while ( (l = g_list_first(cxn->msg_queue)) ) {
			struct chime_msg *cmsg = l->data;

			g_object_unref(cmsg->msg);
			g_free(cmsg);
			cxn->msg_queue = g_list_remove(cxn->msg_queue, cmsg);
		}
		cxn->msg_queue = NULL;
		g_clear_pointer(&cxn->session_token, g_free);
		purple_connection_set_protocol_data(cxn->prpl_conn, NULL);
		g_free(cxn);
	}
	printf("Chime close\n");
}


const gchar *chime_statuses[CHIME_MAX_STATUS] = {
	"zero", "offline", "Available", "three", "Busy", "Mobile"
};

static GList *chime_purple_status_types(PurpleAccount *account)
{
	PurpleStatusType *type;
	GList *types = NULL;

	type = purple_status_type_new(PURPLE_STATUS_OFFLINE, chime_statuses[1],
				      _("Offline"), TRUE);
	types = g_list_append(types, type);
	type = purple_status_type_new(PURPLE_STATUS_AVAILABLE, chime_statuses[2],
				      _("Available"), TRUE);
	types = g_list_append(types, type);
	type = purple_status_type_new(PURPLE_STATUS_AVAILABLE, chime_statuses[3],
				      _("Status 3"), FALSE);
	types = g_list_append(types, type);
	type = purple_status_type_new(PURPLE_STATUS_UNAVAILABLE, chime_statuses[4],
				      _("Busy"), TRUE);
	types = g_list_append(types, type);
	type = purple_status_type_new(PURPLE_STATUS_MOBILE, chime_statuses[5],
				      _("Mobile"), FALSE);
	types = g_list_append(types, type);

	return types;
}
static void sts_cb(struct chime_connection *cxn, SoupMessage *msg,
		   JsonNode *node, gpointer _roomlist)
{
}
static void chime_purple_set_status(PurpleAccount *account, PurpleStatus *status)
{
	struct chime_connection *cxn = purple_connection_get_protocol_data(account->gc);
	printf("set status %s\n", purple_status_get_id(status));

	JsonBuilder *builder = json_builder_new();
	builder = json_builder_begin_object(builder);
	builder = json_builder_set_member_name(builder, "ManualAvailability");
	builder = json_builder_add_string_value(builder, purple_status_get_id(status));
	builder = json_builder_end_object(builder);
	JsonNode *node = json_builder_get_root(builder);

	SoupURI *uri = soup_uri_new_printf(cxn->presence_url, "/presencesettings");
	chime_queue_http_request(cxn, node, uri, sts_cb, NULL);

	g_object_unref(builder);

}

static PurplePluginProtocolInfo chime_prpl_info = {
	.options = OPT_PROTO_NO_PASSWORD,
	.list_icon = chime_purple_list_icon,
	.login = chime_purple_login,
	.close = chime_purple_close,
	.status_types = chime_purple_status_types,
	.set_status = chime_purple_set_status,
	.send_im = chime_purple_send_im,
	.chat_info = chime_purple_chat_info,
	.join_chat = chime_purple_join_chat,
	.chat_leave = chime_purple_chat_leave,
	.chat_send = chime_purple_chat_send,
	.roomlist_get_list = chime_purple_roomlist_get_list,
	.chat_info_defaults = chime_purple_chat_info_defaults,
	.add_buddy = chime_purple_add_buddy,
	.buddy_free = chime_purple_buddy_free,
	.remove_buddy = chime_purple_remove_buddy,
	.keepalive = chime_purple_keepalive,
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
	.id = (char *)"prpl-chime",
	.name = (char *)"Amazon Chime",
	.version = (char *)PACKAGE_VERSION,
	.summary = (char *)"Amazon Chime Protocol Plugin",
	.description = (char *)"A plugin for Chime",
	.author = (char *)"David Woodhouse <dwmw2@infradead.org>",
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

void chime_update_last_msg(struct chime_connection *cxn, gboolean is_room,
			   const gchar *id, const gchar *msg_time,
			   const gchar *msg_id)
{
	gchar *key = g_strdup_printf("last-%s-%s", is_room ? "room" : "conversation", id);
	gchar *val = g_strdup_printf("%s|%s", msg_id, msg_time);

	purple_account_set_string(cxn->prpl_conn->account, key, val);
	g_free(key);
	g_free(val);

	JsonBuilder *jb = json_builder_new();
	jb = json_builder_begin_object(jb);
	jb = json_builder_set_member_name(jb, "LastReadMessageId");
	jb = json_builder_add_string_value(jb, msg_id);
	jb = json_builder_end_object(jb);

	SoupURI *uri = soup_uri_new_printf(cxn->messaging_url,
					   "/%ss/%s", is_room ? "room" : "conversation",
					   id);
	chime_queue_http_request(cxn, json_builder_get_root(jb), uri, NULL, NULL);
	g_object_unref(jb);
}

/* WARE! msg_id is allocated, msg_time is const */
gboolean chime_read_last_msg(struct chime_connection *cxn, gboolean is_room,
			     const gchar *id, const gchar **msg_time,
			     gchar **msg_id)
{
	gchar *key = g_strdup_printf("last-%s-%s", is_room ? "room" : "conversation", id);
	const gchar *val = purple_account_get_string(cxn->prpl_conn->account, key, NULL);
	if (!val || !val[0])
		return FALSE;

	*msg_time = strrchr(val, '|');
	if (!*msg_time) {
		/* Only a date, no msgid */
		*msg_time = val;
		if (msg_id)
			*msg_id = NULL;
		return TRUE;
	}

	if (msg_id)
		*msg_id = g_strndup(val, *msg_time - val);
	(*msg_time)++; /* Past the | */

	g_free(key);
	return TRUE;
}
