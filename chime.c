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
#include "chime-connection-private.h"

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
	printf("Plugin destroy\n");
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
	ChimeConnection *cxn = PURPLE_CHIME_CXN(conn);
	printf("set idle %d\n", idle_time);

	chime_connection_set_device_status_async(cxn, idle_time ? "Idle" : "Active",
						 NULL, on_set_idle_ready,
						 NULL);
}

static void on_chime_connected(ChimeConnection *cxn, const gchar *display_name, PurpleConnection *conn)
{
	purple_debug(PURPLE_DEBUG_INFO, "chime", "Chime connected as %s\n", display_name);
	purple_connection_set_display_name(conn, display_name);
	purple_connection_set_state(conn, PURPLE_CONNECTED);
	chime_purple_set_idle(conn, 0);

	/* We don't want this before we are connected and have them all */
	g_signal_connect(cxn, "new-contact",
			 G_CALLBACK(on_chime_new_contact), conn);

	/* Remove any contacts that don't exist */
	GSList *l = purple_find_buddies(conn->account, NULL);
	while (l) {
		PurpleBuddy *buddy = l->data;
		ChimeContact *contact = chime_connection_contact_by_email(cxn,
									  purple_buddy_get_name(buddy));
		if (!contact)
			purple_blist_remove_buddy(buddy);

		l = g_slist_remove(l, buddy);
	}

	/* And add any that do, and monitor status for all */
	chime_connection_foreach_contact(cxn, (ChimeContactCB)on_chime_new_contact, conn);

	chime_connection_foreach_room(cxn, (ChimeRoomCB)on_chime_new_room, conn);
}

static void on_chime_disconnected(ChimeConnection *cxn, GError *error, PurpleConnection *conn)
{
	if (error)
		purple_connection_error_reason(conn, PURPLE_CONNECTION_ERROR_NETWORK_ERROR, error->message);

	g_signal_handlers_disconnect_matched(cxn, G_SIGNAL_MATCH_FUNC|G_SIGNAL_MATCH_DATA,
					     0, 0, NULL, on_chime_new_contact, conn);
	purple_debug(PURPLE_DEBUG_INFO, "chime", "Chime disconnected: %s\n",
		     error ? error->message : "<no error>");
}

static void on_chime_progress(ChimeConnection *cxn, int percent, const gchar *msg, PurpleConnection *conn)
{
	printf("CHIME PROGRESS %p %d %s\n", conn, percent, msg);
	purple_connection_update_progress(conn, msg, percent, 100);
}


static int purple_level_from_chime(ChimeLogLevel lvl)
{
	return lvl + 1; /* Because it is. We only maintain the *fiction* that we do
			   a proper lookup/translation, and call this out into a
			   separate function to make it 100% clear. */
}

static void on_chime_log_message(ChimeConnection *cxn, ChimeLogLevel lvl, const gchar *str,
				 PurpleConnection *conn)
{
	purple_debug(purple_level_from_chime(lvl), "chime", "%s", str);
}

static void on_session_token_changed(ChimeConnection *connection, GParamSpec *pspec, PurpleConnection *conn)
{
	purple_debug(PURPLE_DEBUG_INFO, "chime", "Session token changed\n");
	purple_account_set_string(conn->account, "token", chime_connection_get_session_token(connection));
}

/* Hm, doesn't GLib have something that'll do this for us? */
static void get_machine_id(unsigned char *id, int len)
{
	int i = 0;

	memset(id, 0, len);

	gchar *machine_id;
	if (g_file_get_contents("/etc/machine-id", &machine_id, NULL, NULL)) {
		while (i < len * 2 && g_ascii_isxdigit(machine_id[i]) &&
		       g_ascii_isxdigit(machine_id[i+1])) {
			id[i / 2] = (g_ascii_xdigit_value(machine_id[i]) << 4) + g_ascii_xdigit_value(machine_id[i+1]);
			i += 2;
		}
		return;
	}
#ifdef _WIN32
	/* XXX: On Windows, try HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Cryptography\MachineGuid */
#endif
	/* XXX: We could actually try to cobble one together from things like
	 * the FSID of the root file system (see how OpenConnect does that). */
	g_warning("No /etc/machine-id; faking");
	for (i = 0; i < len; i++)
		id[i] = g_random_int_range(0, 256);
}

static void chime_purple_login(PurpleAccount *account)
{
	PurpleConnection *conn = purple_account_get_connection(account);

	const gchar *devtoken = purple_account_get_string(account, "devtoken", NULL);
	/* Generate a stable device-id based on on the host identity and account name.
	 * This helps prevent an explosion of separate "devices" being tracked on
	 * the Chime service side, as we delete and recreate accounts. */
	if (!devtoken || !devtoken[0]) {
		unsigned char machine_id[16];
		get_machine_id(machine_id, sizeof(machine_id));

		GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA1);
		g_checksum_update(sum, machine_id, sizeof(machine_id));

		const char *user = purple_account_get_username(account);
		g_checksum_update(sum, (void *)user, strlen(user));
		purple_account_set_string(account, "devtoken", g_checksum_get_string(sum));
		g_checksum_free(sum);
		devtoken = purple_account_get_string(account, "devtoken", NULL);
	}

	const gchar *server = purple_account_get_string(account, "server", NULL);
	const gchar *token = purple_account_get_string(account, "token", NULL);

	purple_connection_update_progress(conn, _("Connecting..."), 0, 100);

	struct purple_chime *pc = g_new0(struct purple_chime, 1);
	purple_connection_set_protocol_data(conn, pc);
	purple_chime_init_conversations(pc);
	pc->cxn = chime_connection_new(conn, server, devtoken, token);

	g_signal_connect(pc->cxn, "notify::session-token",
			 G_CALLBACK(on_session_token_changed), conn);
	g_signal_connect(pc->cxn, "connected",
			 G_CALLBACK(on_chime_connected), conn);
	g_signal_connect(pc->cxn, "disconnected",
			 G_CALLBACK(on_chime_disconnected), conn);
	g_signal_connect(pc->cxn, "progress",
			 G_CALLBACK(on_chime_progress), conn);
	g_signal_connect(pc->cxn, "new-conversation",
			 G_CALLBACK(on_chime_new_conversation), conn);
	/* We don't use 'conn' for this one as we don't want it disconnected
	   on close, and it doesn't use it anyway. */
	g_signal_connect(pc->cxn, "log-message",
			 G_CALLBACK(on_chime_log_message), NULL);


}

static void disconnect_contact(ChimeConnection *cxn, ChimeContact *contact,
			       PurpleConnection *conn)
{
	g_signal_handlers_disconnect_matched(contact, G_SIGNAL_MATCH_DATA,
					     0, 0, NULL, NULL, conn);
}

static void chime_purple_close(PurpleConnection *conn)
{
	struct purple_chime *pc = purple_connection_get_protocol_data(conn);

	purple_chime_destroy_conversations(pc);

	chime_connection_foreach_contact(pc->cxn, (ChimeContactCB)disconnect_contact, conn);

	g_signal_handlers_disconnect_matched(pc->cxn, G_SIGNAL_MATCH_DATA,
					     0, 0, NULL, NULL, conn);
	g_clear_object(&pc->cxn);
	g_free(pc);
	purple_connection_set_protocol_data(conn, NULL);

	purple_debug(PURPLE_DEBUG_INFO, "chime", "Chime close");
}


static const PurpleStatusPrimitive purple_statuses[] = {
	[CHIME_AVAILABILITY_OFFLINE] = PURPLE_STATUS_OFFLINE,
	[CHIME_AVAILABILITY_AVAILABLE] = PURPLE_STATUS_AVAILABLE,
	[CHIME_AVAILABILITY_AWAY] = PURPLE_STATUS_AWAY,
	[CHIME_AVAILABILITY_BUSY] = PURPLE_STATUS_UNAVAILABLE,
	[CHIME_AVAILABILITY_MOBILE] = PURPLE_STATUS_MOBILE,
	[CHIME_AVAILABILITY_PRIVATE] = PURPLE_STATUS_INVISIBLE,
};

static GList *chime_purple_status_types(PurpleAccount *account)
{
	PurpleStatusType *type;
	ChimeAvailability av;
	GList *types = NULL;

	gpointer klass = g_type_class_ref(CHIME_TYPE_AVAILABILITY);

	for (av = CHIME_AVAILABILITY_OFFLINE; av <= CHIME_AVAILABILITY_PRIVATE; av++) {
		GEnumValue *val = g_enum_get_value(klass, av);

		type = purple_status_type_new(purple_statuses[av], val->value_name,
					      _(val->value_nick),
					      (av == CHIME_AVAILABILITY_AVAILABLE ||
					       av == CHIME_AVAILABILITY_BUSY));
		types = g_list_append(types, type);
	}
	g_type_class_unref(klass);

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
	ChimeConnection *cxn = PURPLE_CHIME_CXN(account->gc);
	const gchar *status_str  = purple_status_is_available(status) ? "Automatic" : "Busy";

	printf("set status %s for %s\n", status_str, purple_status_get_id(status));

	chime_connection_set_presence_async(cxn, status_str, NULL, NULL,
					    on_set_status_ready, NULL);
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
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

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

	SoupURI *uri = soup_uri_new_printf(priv->messaging_url,
					   "/%ss/%s", is_room ? "room" : "conversation",
					   id);
	JsonNode *node = json_builder_get_root(jb);
	chime_connection_queue_http_request(cxn, node, uri, "POST", NULL, NULL);
	json_node_unref(node);
	g_object_unref(jb);
}

/* WARE! msg_id is allocated, msg_time is const */
gboolean chime_read_last_msg(PurpleConnection *conn, gboolean is_room,
			     const gchar *id, const gchar **msg_time,
			     gchar **msg_id)
{
	gchar *key = g_strdup_printf("last-%s-%s", is_room ? "room" : "conversation", id);
	const gchar *val = purple_account_get_string(conn->account, key, NULL);
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
