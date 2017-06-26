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

/* Called to signal presence to Pidgin, with a node obtained
 * either from Juggernaut or explicit request (at startup). */
static void set_contact_presence(struct chime_connection *cxn, JsonNode *node)
{
	const gchar *id;
	gint64 availability, revision;

	if (!cxn->contacts_by_id)
		return;

	if (!parse_string(node, "ProfileId", &id) ||
	    !parse_int(node, "Revision", &revision) ||
	    !parse_int(node, "Availability", &availability))
		return;

	struct chime_contact *contact = g_hash_table_lookup(cxn->contacts_by_id, id);
	if (!contact)
		return;

	if (revision < contact->avail_revision)
		return;

	contact->avail_revision = revision;
	contact->availability = availability;

	if (contact->buddy && availability > 0 && availability < CHIME_MAX_STATUS) {
		printf("Set %s avail %s\n", contact->buddy->name, chime_statuses[availability]);
		if (contact->buddy)
			purple_prpl_got_user_status(cxn->prpl_conn->account, contact->buddy->name, chime_statuses[availability], NULL);
	}
}

/* Callback for Juggernaut notifications about status */
static void contact_presence_cb(gpointer _cxn, JsonNode *node)
{
	struct chime_connection *cxn = _cxn;
	const gchar *str;

	if (!parse_string(node, "klass", &str) ||
	    strcmp(str, "Presence"))
		return;

	JsonObject *obj = json_node_get_object(node);
	node = json_object_get_member(obj, "record");
	if (!node)
		return;

	set_contact_presence(cxn, node);
}

static void presence_cb(struct chime_connection *cxn, SoupMessage *msg,
			 JsonNode *node, gpointer _unused)
{
	if (!SOUP_STATUS_IS_SUCCESSFUL(msg->status_code))
		return;

	JsonObject *obj = json_node_get_object(node);
	node = json_object_get_member(obj, "Presences");
	JsonArray *arr = json_node_get_array(node);
	int i, len = json_array_get_length(arr);
	for (i = 0; i < len; i++)
		set_contact_presence(cxn, json_array_get_element(arr, i));
}

static gboolean fetch_presences(gpointer _cxn)
{
	struct chime_connection *cxn = _cxn;
	int i, len = g_slist_length(cxn->contacts_needed);
	if (!len)
		return FALSE;

	gchar **ids = g_new0(gchar *, len + 1);
	i = 0;
	while (cxn->contacts_needed) {
		struct chime_contact *contact = g_hash_table_lookup(cxn->contacts_by_id,
								    cxn->contacts_needed->data);
		cxn->contacts_needed = g_slist_remove(cxn->contacts_needed, cxn->contacts_needed->data);
		if (!contact || contact->avail_revision)
			continue;

		ids[i++] = contact->profile_id;
	}
	/* We don't actually need any */
	if (i) {
		ids[i++] = NULL;
		gchar *query = g_strjoinv(",", ids);

		SoupURI *uri = soup_uri_new_printf(cxn->presence_url, "/presence");
		soup_uri_set_query_from_fields(uri, "profile-ids", query, NULL);
		g_free(query);

		chime_queue_http_request(cxn, NULL, uri, presence_cb, NULL);
	}
	g_free(ids);
	return FALSE;
}

/* Add or update a contact, from contacts list or conversation */
struct chime_contact *chime_contact_new(struct chime_connection *cxn, JsonNode *node, gboolean conv)
{
	const gchar *email, *full_name, *presence_channel, *display_name, *profile_id;
	const gchar *profile_channel = NULL;
	struct chime_contact *contact;

	/*                        ↓↓ WTF guys? ↓↓                 */
	if (!parse_string(node, conv?"Email":"email", &email) ||
	    !parse_string(node, conv?"FullName":"full_name", &full_name) ||
	    !parse_string(node, conv?"PresenceChannel":"presence_channel", &presence_channel) ||
	    (!conv && !parse_string(node, "profile_channel", &profile_channel)) ||
	    !parse_string(node, conv?"DisplayName":"display_name", &display_name) ||
	    !parse_string(node, conv?"ProfileId":"id", &profile_id))
		return NULL;

	contact = g_hash_table_lookup(cxn->contacts_by_id, profile_id);
	if (!contact) {
		contact = g_new0(struct chime_contact, 1);
		contact->profile_id = g_strdup(profile_id);
		contact->presence_channel = g_strdup(presence_channel);
		chime_jugg_subscribe(cxn, presence_channel, contact_presence_cb, cxn);
		if (!conv) {
			contact->presence_channel = g_strdup(profile_channel);
			chime_jugg_subscribe(cxn, profile_channel, jugg_dump_incoming, (char *)"Buddy Profile");
		}
		g_hash_table_insert(cxn->contacts_by_id, contact->profile_id, contact);

		/* We'll need to request presence */
		if (!cxn->contacts_needed)
			g_idle_add(fetch_presences, cxn);
		cxn->contacts_needed = g_slist_prepend(cxn->contacts_needed, contact->profile_id);
	}
	if (g_strcmp0(contact->email, email)) {
		/* This should never change for a given ProfileId but let's at least
		   not blow up completely... */
		if (contact->email)
			g_hash_table_steal(cxn->contacts_by_email, contact->email);
		g_free(contact->email);
		contact->email = g_strdup(email);
		g_hash_table_insert(cxn->contacts_by_email, contact->email, contact);
	}
	if (g_strcmp0(contact->full_name, full_name)) {
		g_free(contact->full_name);
		contact->full_name = g_strdup(full_name);
		/* XXX: Who's going to emit purple_conv_chat_rename_user() for chats, and
		   rename buddies? Probably want to make ChimeContact a proper GObject and
		   then it can just be signals, so leave it unresolved for now... */
	}
	if (g_strcmp0(contact->display_name, display_name)) {
		g_free(contact->display_name);
		contact->display_name = g_strdup(display_name);
	}
	return contact;
}

/* Temporary struct for iterating over a JsonArray of presences */
static void one_buddy_cb(JsonArray *arr, guint idx, JsonNode *elem, gpointer _cxn)
{
	struct chime_connection *cxn = _cxn;
	struct chime_contact *contact;

	contact = chime_contact_new(cxn, elem, FALSE);
	if (!contact)
		return;

	PurpleBuddy *buddy = purple_find_buddy(cxn->prpl_conn->account, contact->email);
	if (!buddy) {
		PurpleGroup *group = purple_find_group(_("Chime Contacts"));
		if (!group) {
			group = purple_group_new(_("Chime Contacts"));
			purple_blist_add_group(group, NULL);
		}

		buddy = purple_buddy_new(cxn->prpl_conn->account, contact->email, contact->display_name);
		purple_blist_add_buddy(buddy, NULL, group, NULL);
	}

	/* ASSERT: there isn't already a *different* contact->buddy */
	contact->buddy = buddy;
	buddy->proto_data = contact;
}

void chime_purple_buddy_free(PurpleBuddy *buddy)
{
	struct chime_connection *cxn = purple_connection_get_protocol_data(buddy->account->gc);
	struct chime_contact *contact = buddy->proto_data;

	printf("buddy_free %s\n", purple_buddy_get_name(buddy));

	if (!contact)
		return;

	if (contact->profile_id) {
		g_hash_table_remove(cxn->contacts_by_email, contact->email);
		g_hash_table_remove(cxn->contacts_by_id, contact->profile_id);
		g_free(contact->profile_id);
		contact->profile_id = NULL;
	}
	if (contact->profile_channel) {
		chime_jugg_unsubscribe(cxn, contact->profile_channel, jugg_dump_incoming, (char *)"Buddy Profile");
		g_free(contact->profile_channel);
		contact->profile_channel = NULL;
	}
	if (contact->presence_channel) {
		chime_jugg_unsubscribe(cxn, contact->presence_channel, contact_presence_cb, cxn);
		g_free(contact->presence_channel);
		contact->presence_channel = NULL;
	}
	g_free(contact);
	buddy->proto_data = NULL;
}


static void buddies_cb(struct chime_connection *cxn, SoupMessage *msg, JsonNode *node, gpointer _unused)
{
	if (node) {
		JsonArray *arr= json_node_get_array(node);

		json_array_foreach_element(arr, one_buddy_cb, cxn);
	}

	/* Delete any that don't exist on the server (any more) */
	GSList *l = purple_find_buddies(cxn->prpl_conn->account, NULL);
	while (l) {
		PurpleBuddy *buddy = l->data;
		struct chime_contact *contact = buddy->proto_data;
		if (!contact || !contact->profile_id)
			purple_blist_remove_buddy(buddy);

		l = g_slist_remove(l, buddy);
	}
}

void fetch_buddies(struct chime_connection *cxn)
{
	SoupURI *uri = soup_uri_new_printf(cxn->contacts_url, "/contacts");
	chime_queue_http_request(cxn, NULL, uri, buddies_cb, NULL);
}


static void add_buddy_cb(struct chime_connection *cxn, SoupMessage *msg, JsonNode *node, gpointer _buddy)
{
	PurpleBuddy *buddy = _buddy;

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
#if 0
	const gchar *id;
	if (parse_string(node, "id", &id)) {
		printf("new buddy id %s\n", id);
		contact->profile_id = g_strdup(id);
		g_hash_table_insert(cxn->contacts_by_id, contact->profile_id, contact);
		g_hash_table_insert(cxn->contacts_by_name, contact->email, contact);
	}
	/* XXX: Don't know how to correctly get the presence/profile
	   channels without refetching all contacts, but they *are*
	   predictable... */
	if (!contact->profile_channel) {
		contact->profile_channel = g_strdup_printf("profile!%s", id);
		chime_jugg_subscribe(cxn, contact->profile_channel, jugg_dump_incoming, (char *)"Buddy Profile");
	}
	if (!contact->presence_channel) {
		contact->presence_channel = g_strdup_printf("profile_presence!%s", id);
		chime_jugg_subscribe(cxn, contact->presence_channel, contact_presence_cb, cxn);
	}
#endif
	/* XXX: Can we get only the one? */
	fetch_buddies(cxn);
}

void chime_purple_add_buddy(PurpleConnection *conn, PurpleBuddy *buddy, PurpleGroup *group)
{
	struct chime_connection *cxn = purple_connection_get_protocol_data(conn);

	struct chime_contact *contact = g_hash_table_lookup(cxn->contacts_by_email,
							    purple_buddy_get_name(buddy));
	if (contact) {
		/* We already knew about this person (from a conversation) */
		buddy->proto_data = contact;
		return;
	}

	SoupURI *uri = soup_uri_new_printf(cxn->contacts_url, "/invites");
	JsonBuilder *builder = json_builder_new();

	builder = json_builder_begin_object(builder);
	builder = json_builder_set_member_name(builder, "profile");
	builder = json_builder_begin_object(builder);
	builder = json_builder_set_member_name(builder, "email");
	builder = json_builder_add_string_value(builder, purple_buddy_get_name(buddy));
	builder = json_builder_end_object(builder);
	builder = json_builder_end_object(builder);

	/* For cancellation if the buddy is deleted before the request completes */
	chime_queue_http_request(cxn, json_builder_get_root(builder), uri, add_buddy_cb, buddy);

	g_object_unref(builder);
}

void chime_purple_remove_buddy(PurpleConnection *conn, PurpleBuddy *buddy, PurpleGroup *group)
{
	struct chime_connection *cxn = purple_connection_get_protocol_data(conn);
	purple_notify_error(conn, NULL, _("Cannot remove buddies yet"), NULL);
	/* Put it back :) */
	fetch_buddies(cxn);
}

static void destroy_contact(gpointer _contact)
{
	struct chime_contact *contact = _contact;
	g_free(contact->profile_id);
	g_free(contact->presence_channel);
	g_free(contact->profile_channel);
	g_free(contact->email);
	g_free(contact->full_name);
	g_free(contact->display_name);
	if (contact->buddy)
		contact->buddy->proto_data = NULL;
	/* XXX: conversations list */
	g_free(contact);
}

void chime_init_buddies(struct chime_connection *cxn)
{
	cxn->contacts_by_id = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, destroy_contact);
	cxn->contacts_by_email = g_hash_table_new(g_str_hash, g_str_equal);

	fetch_buddies(cxn);
}

void chime_destroy_buddies(struct chime_connection *cxn)
{
	g_hash_table_destroy(cxn->contacts_by_email);
	g_hash_table_destroy(cxn->contacts_by_id);
	cxn->contacts_by_email = cxn->contacts_by_id = NULL;
}
