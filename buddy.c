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

#include <string.h>

#include <glib/gi18n.h>
#include <glib/glist.h>

#include <prpl.h>
#include <blist.h>

#include "chime.h"

#include <libsoup/soup.h>

struct buddy_data {
	gchar *presence_channel;
	gchar *profile_channel;
	gchar *id;
	SoupMessage *add_msg;
};

static void set_buddy_presence(struct chime_connection *cxn, JsonNode *node)
{
	const gchar *id;

	if (!cxn->buddies)
		return;
	if (!parse_string(node, "ProfileId", &id))
		return;
	PurpleBuddy *buddy = g_hash_table_lookup(cxn->buddies, id);
	if (!buddy)
		return;
	JsonObject *obj = json_node_get_object(node);
	node = json_object_get_member(obj, "Availability");
	gchar *sts = g_strdup_printf("%d", (int)json_node_get_int(node));
	printf("Set %s avail %s\n", buddy->name, sts);
	purple_prpl_got_user_status(cxn->prpl_conn->account, buddy->name, sts, NULL);
	g_free(sts);
}

static void buddy_presence_cb(gpointer _cxn, JsonNode *node)
{
	struct chime_connection *cxn = _cxn;
	const gchar *str;

	jugg_dump_incoming("Buddy presence", node);

	if (!parse_string(node, "klass", &str) ||
	    strcmp(str, "Presence"))
		return;

	JsonObject *obj = json_node_get_object(node);
	node = json_object_get_member(obj, "record");
	if (!node)
		return;

	set_buddy_presence(cxn, node);
}


struct buddy_gather {
	struct chime_connection *cxn;
	GSList *ids;
};
static void one_buddy_cb(JsonArray *arr, guint idx, JsonNode *elem, gpointer _bg)
{
	struct buddy_gather *bg = _bg;
	struct chime_connection *cxn = bg->cxn;
	struct buddy_data *bd;
	const gchar *email, *full_name, *presence_channel, *profile_channel, *display_name, *id;

	if (!parse_string(elem, "email", &email) ||
	    !parse_string(elem, "full_name", &full_name) ||
	    !parse_string(elem, "presence_channel", &presence_channel) ||
	    !parse_string(elem, "profile_channel", &profile_channel) ||
	    !parse_string(elem, "display_name", &display_name) ||
	    !parse_string(elem, "id", &id))
		return;

	PurpleBuddy *buddy = purple_find_buddy(cxn->prpl_conn->account, email);
	if (!buddy) {
		PurpleGroup *group = purple_find_group(_("Chime Contacts"));
		if (!group) {
			group = purple_group_new(_("Chime Contacts"));
			purple_blist_add_group(group, NULL);
		}

		buddy = purple_buddy_new(cxn->prpl_conn->account, email, display_name);
		purple_blist_add_buddy(buddy, NULL, group, NULL);

		printf("New buddy %s\n", display_name);
	}
	if (!buddy->proto_data) {
		buddy->proto_data = g_new0(struct buddy_data, 1);
	}
	bd = buddy->proto_data;
	if (!bd->id) {
		bd->id = g_strdup(id);
		g_hash_table_insert(cxn->buddies, bd->id, buddy);
	}
	if (!bd->profile_channel) {
		bd->profile_channel = g_strdup(profile_channel);
		chime_jugg_subscribe(cxn, profile_channel, jugg_dump_incoming, "Buddy Profile");
	}
	if (!bd->presence_channel) {
		bd->presence_channel = g_strdup(presence_channel);
		chime_jugg_subscribe(cxn, presence_channel, buddy_presence_cb, cxn);
		/* We'll need to request presence */
		bg->ids = g_slist_prepend(bg->ids, bd->id);
	}
}

void chime_purple_buddy_free(PurpleBuddy *buddy)
{
	struct chime_connection *cxn = purple_connection_get_protocol_data(buddy->account->gc);
	struct buddy_data *bd = buddy->proto_data;

	printf("buddy_free %s\n", purple_buddy_get_name(buddy));

	if (!bd)
		return;

	if (bd->add_msg) {
		SoupMessage *msg = bd->add_msg;
		bd->add_msg = NULL; /* Stop the callback from doing anything */
		soup_session_cancel_message(cxn->soup_sess, msg, 1);
	}
	if (bd->id) {
		g_hash_table_remove(cxn->buddies, bd->id);
		g_free(bd->id);
		bd->id = NULL;
	}
	if (bd->profile_channel) {
		chime_jugg_unsubscribe(cxn, bd->profile_channel, jugg_dump_incoming, "Buddy Profile");
		g_free(bd->profile_channel);
		bd->profile_channel = NULL;
	}
	if (bd->presence_channel) {
		chime_jugg_unsubscribe(cxn, bd->presence_channel, buddy_presence_cb, cxn);
		g_free(bd->presence_channel);
		bd->presence_channel = NULL;
	}
	g_free(bd);
	buddy->proto_data = NULL;
}

/* Fetching presence for new buddies */
static void one_presence_cb(JsonArray *arr, guint idx, JsonNode *elem, gpointer _cxn)
{
	struct chime_connection *cxn = _cxn;
	set_buddy_presence(cxn, elem);
}

static void presence_cb(struct chime_connection *cxn, SoupMessage *msg,
			 JsonNode *node, gpointer _unused)
{
	if (!SOUP_STATUS_IS_SUCCESSFUL(msg->status_code))
		return;

	JsonObject *obj = json_node_get_object(node);
	node = json_object_get_member(obj, "Presences");
	JsonArray *arr = json_node_get_array(node);
	json_array_foreach_element(arr, one_presence_cb, cxn);
}

static void buddies_cb(struct chime_connection *cxn, SoupMessage *msg, JsonNode *node, gpointer _unused)
{
	struct buddy_gather bg;
	bg.cxn = cxn;
	bg.ids = NULL;

	if (node) {
		JsonArray *arr= json_node_get_array(node);

		json_array_foreach_element(arr, one_buddy_cb, &bg);
	}

	/* Delete any that don't exist on the server (any more) */
	GSList *l = purple_find_buddies(cxn->prpl_conn->account, NULL);
	while (l) {
		PurpleBuddy *buddy = l->data;
		struct buddy_data *bd = buddy->proto_data;
		if (!bd || !bd->id) {
			printf("kill %s %p %s\n", buddy->name, bd, bd?bd->id:"<no bd>");
			purple_blist_remove_buddy(buddy);
		}
		l = g_slist_remove(l, buddy);
	}
	/* New contacts; fetch presence */
	if (bg.ids) {
		int len = g_slist_length(bg.ids);
		gchar **strs = g_new0(gchar *, len + 1);
		while (bg.ids) {
			strs[--len] = bg.ids->data;
			bg.ids = g_slist_remove(bg.ids, bg.ids->data);
		}
		gchar *query = g_strjoinv(",", strs);
		g_free(strs);

		SoupURI *uri = soup_uri_new_printf(cxn->presence_url, "/presence");
		soup_uri_set_query_from_fields(uri, "profile-ids", query, NULL);
		g_free(query);

		chime_queue_http_request(cxn, NULL, uri, presence_cb, NULL, TRUE);

	}
}

static void add_buddy_cb(struct chime_connection *cxn, SoupMessage *msg, JsonNode *node, gpointer _buddy)
{
	PurpleBuddy *buddy = _buddy;
	struct buddy_data *bd = buddy->proto_data;

	if (!bd || bd->add_msg != msg)
		return;

	bd->add_msg = NULL;

	if (!SOUP_STATUS_IS_SUCCESSFUL(msg->status_code)) {
		const gchar *reason = msg->reason_phrase;
		gchar *err_str;

		parse_string(node, "error", &reason);
		err_str = g_strdup_printf(_("Failed to add/invite contact: %s"), reason);
		purple_notify_error(cxn->prpl_conn, NULL, err_str, NULL);
		g_free(err_str);
		purple_blist_remove_buddy(buddy);
		return;
	}
	const gchar *id;
	if (parse_string(node, "id", &id)) {
		printf("new buddy id %s\n", id);
		bd->id = g_strdup(id);
		g_hash_table_insert(cxn->buddies, buddy->name, bd->id);
	}
	/* XXX: Don't know how to correctly get the presence/profile
	   channels without refetching all contacts, but they *are*
	   predictable... */
	if (!bd->profile_channel) {
		bd->profile_channel = g_strdup_printf("profile_presence!%s", id);
		chime_jugg_subscribe(cxn, bd->profile_channel, jugg_dump_incoming, "Buddy Profile");
	}
	if (!bd->presence_channel) {
		bd->presence_channel = g_strdup_printf("profile!%s", id);
		chime_jugg_subscribe(cxn, bd->presence_channel, buddy_presence_cb, cxn);
	}

	//	fetch_buddies(cxn);
}

void fetch_buddies(struct chime_connection *cxn)
{
	SoupURI *uri = soup_uri_new_printf(cxn->contacts_url, "/contacts");
	chime_queue_http_request(cxn, NULL, uri, buddies_cb, NULL, TRUE);
}

void chime_purple_add_buddy(PurpleConnection *conn, PurpleBuddy *buddy, PurpleGroup *group)
{
	struct chime_connection *cxn = purple_connection_get_protocol_data(conn);
	SoupURI *uri = soup_uri_new_printf(cxn->contacts_url, "/invites");
	SoupMessage *msg;
	JsonBuilder *builder = json_builder_new();
	struct buddy_data *bd = buddy->proto_data;

	builder = json_builder_begin_object(builder);
	builder = json_builder_set_member_name(builder, "profile");
	builder = json_builder_begin_object(builder);
	builder = json_builder_set_member_name(builder, "email");
	builder = json_builder_add_string_value(builder, purple_buddy_get_name(buddy));
	builder = json_builder_end_object(builder);
	builder = json_builder_end_object(builder);

	if (!bd)
		bd = buddy->proto_data = g_new0(struct buddy_data, 1);

	/* For cancellation if the buddy is deleted before the request completes */
	bd->add_msg = chime_queue_http_request(cxn, json_builder_get_root(builder), uri, add_buddy_cb, buddy, TRUE);

	g_object_unref(builder);
}

void chime_purple_remove_buddy(PurpleConnection *conn, PurpleBuddy *buddy, PurpleGroup *group)
{
	struct chime_connection *cxn = purple_connection_get_protocol_data(conn);
	purple_notify_error(conn, NULL, _("Cannot remove buddies yet"), NULL);
	/* Put it back :) */
	fetch_buddies(cxn);
}

void chime_init_buddies(struct chime_connection *cxn)
{
	cxn->buddies = g_hash_table_new(g_str_hash, g_str_equal);

	fetch_buddies(cxn);
}

void chime_destroy_buddies(struct chime_connection *cxn)
{
	g_hash_table_destroy(cxn->buddies);
	cxn->buddies = NULL;
}
