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
#include <debug.h>

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

static gboolean chime_purple_plugin_load(PurplePlugin *plugin)
{
	printf("Chime plugin load\n");
	setvbuf(stdout, NULL, _IONBF, 0);
	purple_notify_message(plugin, PURPLE_NOTIFY_MSG_ERROR, "Foo",
			      "Chime plugin starting...", NULL, NULL, NULL);
	return TRUE;
}

static gboolean chime_purple_plugin_unload(PurplePlugin *plugin)
{
	printf("Plugin unload\n");
	return TRUE;
}

static void chime_purple_plugin_destroy(PurplePlugin *plugin)
{
	printf("Pkugin destroy\n");
}

static const char *chime_purple_list_icon(PurpleAccount *a, PurpleBuddy *b)
{
        return "chime";
}

static void on_set_idle_ready(GObject *source, GAsyncResult *result, gpointer user_data)
{
	GError *error = NULL;

	if (!chime_connection_set_device_status_finish(CHIME_CONNECTION(source), result, &error)) {
		g_warning("Could not set the device status: %s", error->message);
		g_error_free(error);
		return;
	}
}

static void chime_purple_set_idle(PurpleConnection *conn, int idle_time)
{
	ChimeConnection *cxn = purple_connection_get_protocol_data(conn);
	printf("set idle %d\n", idle_time);

	chime_connection_set_device_status_async(cxn, idle_time ? "Idle" : "Active",
						 NULL, on_set_idle_ready,
						 NULL);
}

static void on_chime_connected(ChimeConnection *cxn, PurpleConnection *conn)
{
	purple_connection_set_state(conn, PURPLE_CONNECTED);
	chime_purple_set_idle(conn, 0);
	purple_debug(PURPLE_DEBUG_INFO, "chime", "Chime connected\n");
}

static void on_chime_disconnected(ChimeConnection *connection, GError *error, PurpleConnection *conn)
{
	if (error)
		purple_connection_error_reason(conn, PURPLE_CONNECTION_ERROR_NETWORK_ERROR, error->message);

	purple_debug(PURPLE_DEBUG_INFO, "chime", "Chime disconnected: %s\n",
		     error ? error->message : "<no error>");
}

static void on_session_token_changed(ChimeConnection *connection, GParamSpec *pspec, PurpleConnection *conn)
{
	purple_debug(PURPLE_DEBUG_INFO, "chime", "Session token changed\n");
	purple_account_set_string(conn->account, "token", chime_connection_get_session_token(connection));
}

static void chime_purple_login(PurpleAccount *account)
{
	PurpleConnection *conn = purple_account_get_connection(account);
	ChimeConnection *cxn;

	const gchar *devtoken = purple_account_get_string(account, "devtoken", NULL);

	if (!devtoken || !devtoken[0]) {
		gchar *uuid = purple_uuid_random();
		purple_account_set_string(account, "devtoken", uuid);
		g_free(uuid);
		devtoken = purple_account_get_string(account, "devtoken", NULL);
	}

	const gchar *server = purple_account_get_string(account, "server", NULL);
	const gchar *token = purple_account_get_string(account, "token", NULL);

	purple_connection_update_progress(conn, _("Connecting..."), 1, CONNECT_STEPS);

	cxn = chime_connection_new(conn, server, devtoken, token);
	purple_connection_set_protocol_data(conn, cxn);

	g_signal_connect(cxn, "notify::session-token",
			 G_CALLBACK(on_session_token_changed), conn);
	g_signal_connect(cxn, "connected",
			 G_CALLBACK(on_chime_connected), conn);
	g_signal_connect(cxn, "disconnected",
			 G_CALLBACK(on_chime_disconnected), conn);


}

static void chime_purple_close(PurpleConnection *conn)
{
	ChimeConnection *cxn = purple_connection_get_protocol_data(conn);
	g_clear_object(&cxn);

	printf("Chime close\n");
}


const gchar *chime_statuses[CHIME_MAX_STATUS] = {
	"zero", "offline", "Automatic", "three", "Busy", "Mobile"
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
	type = purple_status_type_new(PURPLE_STATUS_AWAY, chime_statuses[3],
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

static void on_set_status_ready(GObject *source, GAsyncResult *result, gpointer user_data)
{
	GError *error = NULL;

	if (!chime_connection_set_presence_finish(CHIME_CONNECTION(source), result, &error)) {
		g_warning("Could not set the status: %s", error->message);
		g_error_free(error);
		return;
	}
}

static void chime_purple_set_status(PurpleAccount *account, PurpleStatus *status)
{
	ChimeConnection *cxn = purple_connection_get_protocol_data(account->gc);
	printf("set status %s\n", purple_status_get_id(status));

	chime_connection_set_presence_async(cxn, purple_status_get_id(status), NULL,
					    NULL, on_set_status_ready,
					    NULL);
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
	.send_typing = chime_send_typing,
	.set_idle = chime_purple_set_idle,
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

void chime_update_last_msg(ChimeConnection *cxn, gboolean is_room,
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
	chime_connection_queue_http_request(cxn, json_builder_get_root(jb), uri, "POST", NULL, NULL);
	g_object_unref(jb);
}

/* WARE! msg_id is allocated, msg_time is const */
gboolean chime_read_last_msg(ChimeConnection *cxn, gboolean is_room,
			     const gchar *id, const gchar **msg_time,
			     gchar **msg_id)
{
	gchar *key = g_strdup_printf("last-%s-%s", is_room ? "room" : "conversation", id);
	const gchar *val = purple_account_get_string(cxn->prpl_conn->account, key, NULL);
	g_free(key);

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

	return TRUE;
}
