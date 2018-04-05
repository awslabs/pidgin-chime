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
#include <request.h>

#include "chime.h"

#include <libsoup/soup.h>

static void on_contact_availability(ChimeContact *contact, GParamSpec *ignored, PurpleConnection *conn)
{
	ChimeAvailability availability = chime_contact_get_availability(contact);

	if (availability)
		purple_prpl_got_user_status(conn->account, chime_contact_get_email(contact),
					    chime_availability_name(availability), NULL);
}

static void on_contact_display_name(ChimeContact *contact, GParamSpec *ignored, PurpleConnection *conn)
{
	GSList *buddies = purple_find_buddies(conn->account, chime_contact_get_email(contact));
	while (buddies) {
		PurpleBuddy *buddy = buddies->data;
		purple_blist_server_alias_buddy(buddy, chime_contact_get_display_name(contact));
		buddies = g_slist_remove(buddies, buddy);
	}
}

static void on_contact_disposed(ChimeContact *contact, PurpleConnection *conn)
{
	PurpleGroup *group = purple_find_group(_("xx Ignore transient Chime contacts xx"));
	if (group) {
		PurpleBuddy *buddy = purple_find_buddy_in_group(conn->account,
								chime_contact_get_email(contact),
								group);
		if (buddy)
			purple_blist_remove_buddy(buddy);
	}
}


static void on_buddystatus_changed(ChimeContact *contact, GParamSpec *ignored, PurpleConnection *conn)
{
	gboolean is_buddy;
	const gchar *email;
	ChimeAvailability availability;

	email = chime_contact_get_email(contact);
	availability = chime_contact_get_availability(contact);
	is_buddy = chime_contact_get_contacts_list(contact);

	if (!is_buddy) {
		/* Don't remove from blist until we're fully connected because
		 * some contacts may appear first from conversations and only
		 * later from the contacts list. We don't want to delete them
		 * here only to add them back to the default "Chime Contacts"
		 * group later. */
		if (PURPLE_CONNECTION_IS_CONNECTED(conn)) {
			GSList *buddies = purple_find_buddies(conn->account, email);
			while (buddies) {
				if (PURPLE_BLIST_NODE_SHOULD_SAVE(buddies->data))
					purple_blist_remove_buddy(buddies->data);
				buddies = g_slist_remove(buddies, buddies->data);
			}
		}
	} else {
		const gchar *display_name = chime_contact_get_display_name(contact);

		gboolean found = FALSE;
		GSList *buddies = purple_find_buddies(conn->account, email);
		while (buddies) {
			PurpleBuddy *buddy = buddies->data;
			if (PURPLE_BLIST_NODE_SHOULD_SAVE(buddy))
				found = TRUE;
			purple_blist_server_alias_buddy(buddy, display_name);
			buddies = g_slist_remove(buddies, buddy);
		}
		/* If this is a known contact on the server and we didn't find it
		   in Pidgin except as a transient one, add it now. */
		if (!found) {
			PurpleGroup *group = purple_find_group(_("Chime Contacts"));
			if (!group) {
				group = purple_group_new(_("Chime Contacts"));
				purple_blist_add_group(group, NULL);
			}
			PurpleBuddy *buddy = purple_buddy_new(conn->account, email, NULL);
			purple_blist_server_alias_buddy(buddy, display_name);
			purple_blist_add_buddy(buddy, NULL, group, NULL);
		}

		if (availability)
			purple_prpl_got_user_status(conn->account, email,
						    chime_availability_name(availability), NULL);
	}
}

void on_chime_new_contact(ChimeConnection *cxn, ChimeContact *contact, PurpleConnection *conn)
{
	g_signal_handlers_disconnect_matched(contact, G_SIGNAL_MATCH_FUNC|G_SIGNAL_MATCH_DATA,
					     0, 0, NULL, on_buddystatus_changed, conn);
	g_signal_handlers_disconnect_matched(contact, G_SIGNAL_MATCH_FUNC|G_SIGNAL_MATCH_DATA,
					     0, 0, NULL, on_contact_availability, conn);
	g_signal_handlers_disconnect_matched(contact, G_SIGNAL_MATCH_FUNC|G_SIGNAL_MATCH_DATA,
					     0, 0, NULL, on_contact_display_name, conn);
	g_signal_handlers_disconnect_matched(contact, G_SIGNAL_MATCH_FUNC|G_SIGNAL_MATCH_DATA,
					     0, 0, NULL, on_contact_disposed, conn);

	g_signal_connect(contact, "notify::dead",
			 G_CALLBACK(on_buddystatus_changed), conn);
	g_signal_connect(contact, "notify::availability",
			 G_CALLBACK(on_contact_availability), conn);
	g_signal_connect(contact, "notify::display-name",
			 G_CALLBACK(on_contact_display_name), conn);
	g_signal_connect(contact, "disposed",
			 G_CALLBACK(on_contact_disposed), conn);

	/* Refresh status for transient buddies on reconnect */
	if (purple_find_buddy(conn->account, chime_contact_get_email(contact)))
		on_contact_availability(contact, NULL, conn);

	/* When invoked for all contacts on the CONNECTED signal, we don't immediately
	   get the above signal invoked because they're not actually *new* contacts.
	   So run it manually. */
	if (chime_contact_get_contacts_list(contact))
		on_buddystatus_changed(contact, NULL, conn);
}

void chime_purple_buddy_free(PurpleBuddy *buddy)
{
	/* We don't need to unref the underlying contact as we'll do that when the
	 * connection is destroyed anyway. */
}

static void on_buddy_invited(GObject *source, GAsyncResult *result, gpointer user_data)
{
	GError *error = NULL;

	if (!chime_connection_invite_contact_finish(CHIME_CONNECTION(source), result, &error)) {
		g_warning("Could not invite buddy: %s", error->message);
		g_error_free(error);
		return;
	}
}

void chime_purple_add_buddy(PurpleConnection *conn, PurpleBuddy *buddy, PurpleGroup *group)
{
	ChimeConnection *cxn = PURPLE_CHIME_CXN(conn);

	ChimeContact *contact = chime_connection_contact_by_email(cxn,
								  purple_buddy_get_name(buddy));

	if (contact) {
		purple_blist_server_alias_buddy(buddy, chime_contact_get_display_name(contact));
		on_contact_availability(contact, NULL, conn);

		if (chime_contact_get_contacts_list(contact))
			return;

		/* Ensure we are subscribed to signals */
		on_chime_new_contact(cxn, contact, conn);
	}

	if (!PURPLE_BLIST_NODE_SHOULD_SAVE(buddy)) {
		/* XX: if (!contact) we should probably look it up with an
		 * autocomplete search */
		return;
	}

	chime_connection_invite_contact_async(cxn, purple_buddy_get_name(buddy),
					      NULL, on_buddy_invited, conn);
}

static void on_buddy_removed(GObject *source, GAsyncResult *result, gpointer user_data)
{
	GError *error = NULL;

	if (!chime_connection_remove_contact_finish(CHIME_CONNECTION(source), result, &error)) {
		g_warning("Could not remove buddy: %s", error->message);
		g_error_free(error);
		return;
	}
}

void chime_purple_remove_buddy(PurpleConnection *conn, PurpleBuddy *buddy, PurpleGroup *group)
{
	/* If this buddy still exists elsewhere, don't kill it! */
	GSList *buddies = purple_find_buddies(conn->account, buddy->name);
	while (buddies) {
		PurpleBuddy *b = buddies->data;
		if (b != buddy && PURPLE_BLIST_NODE_SHOULD_SAVE(b)) {
			g_slist_free(buddies);
			return;
		}
		buddies = g_slist_remove(buddies, b);
	}
	ChimeConnection *cxn = PURPLE_CHIME_CXN(conn);
	ChimeContact *contact = chime_connection_contact_by_email(cxn,
								  buddy->name);
	if (!chime_contact_get_contacts_list(contact))
		return;

	g_signal_handlers_disconnect_matched(contact, G_SIGNAL_MATCH_DATA,
					     0, 0, NULL, NULL, conn);

	chime_connection_remove_contact_async(cxn, buddy->name,
					      NULL, on_buddy_removed, conn);
}
static void search_add_buddy(PurpleConnection *conn, GList *row, gpointer _unused)
{
	purple_blist_request_add_buddy(purple_connection_get_account(conn),
				       g_list_nth_data(row, 1), NULL, NULL);
}

static void search_im(PurpleConnection *conn, GList *row, gpointer _unused)
{
	PurpleConversation *conv = purple_conversation_new(PURPLE_CONV_TYPE_IM, purple_connection_get_account(conn),
							   g_list_nth_data(row, 1));
	purple_conversation_present(conv);
}

struct search_data {
	PurpleConnection *conn;
	void *ui_handle;
	GSList *contacts;
	guint refresh_id;
};

static PurpleNotifySearchResults *generate_search_results(GSList *contacts)
{
	PurpleNotifySearchResults *results = purple_notify_searchresults_new();
	PurpleNotifySearchColumn *column;

	column = purple_notify_searchresults_column_new(_("Name"));
	purple_notify_searchresults_column_add(results, column);
	column = purple_notify_searchresults_column_new(_("Email"));
	purple_notify_searchresults_column_add(results, column);
	column = purple_notify_searchresults_column_new(_("Availability"));
	purple_notify_searchresults_column_add(results, column);

	purple_notify_searchresults_button_add(results, PURPLE_NOTIFY_BUTTON_ADD,
					       search_add_buddy);
	purple_notify_searchresults_button_add(results, PURPLE_NOTIFY_BUTTON_IM,
					       search_im);

	gpointer klass = g_type_class_ref(CHIME_TYPE_AVAILABILITY);

	while (contacts) {
		ChimeContact *contact = contacts->data;
		GList *row = NULL;
		row = g_list_append(row, g_strdup(chime_contact_get_display_name(contact)));
		row = g_list_append(row, g_strdup(chime_contact_get_email(contact)));
		GEnumValue *val = g_enum_get_value(klass, chime_contact_get_availability(contact));
		row = g_list_append(row, g_strdup(_(val->value_nick)));
		purple_notify_searchresults_row_add(results, row);
		contacts = contacts->next;
	}

	g_type_class_unref(klass);
	return results;
}

static void search_closed_cb(gpointer _sd)
{
	struct search_data *sd = _sd;

	if (sd->refresh_id)
		g_source_remove(sd->refresh_id);
	while (sd->contacts) {
		ChimeContact *contact = sd->contacts->data;
		g_signal_handlers_disconnect_matched(contact, G_SIGNAL_MATCH_DATA,
						     0, 0, NULL, NULL, sd);
		g_object_unref(contact);
		sd->contacts = g_slist_remove(sd->contacts, contact);
	}
	g_free(sd);
}

static gboolean renew_search_results(gpointer _sd)
{
	struct search_data *sd = _sd;

	PurpleNotifySearchResults *results = generate_search_results(sd->contacts);
	purple_notify_searchresults_new_rows(sd->conn, results, sd->ui_handle);

	sd->refresh_id = 0;
	return FALSE;
}

static void on_search_availability(ChimeContact *contact, GParamSpec *ignored, struct search_data *sd)
{
	/* Gather them up to avoid repeatedly redrawing as they come in the first time */
	if (!sd->refresh_id)
		sd->refresh_id = g_idle_add(renew_search_results, sd);
}

static void search_done(GObject *source, GAsyncResult *result, gpointer _conn)
{
	PurpleConnection *conn = _conn;
	GError *error = NULL;
	GSList *contacts = chime_connection_autocomplete_contact_finish(CHIME_CONNECTION(source), result, &error);

	if (error) {
		g_warning("Autocomplete failed: %s\n", error->message);
		g_error_free(error);
		return;
	}

	PurpleNotifySearchResults *results = generate_search_results(contacts);

	struct search_data *sd = g_new0(struct search_data, 1);
	sd->contacts = contacts;
	sd->conn = conn;
	sd->ui_handle = purple_notify_searchresults(conn, _("Chime autocomplete"), _("Search results"),
						    NULL, results, search_closed_cb, sd);
	if (!sd->ui_handle) {
		purple_notify_error(conn, NULL,
				    _("Unable to display search results."),
				    NULL);
		search_closed_cb(sd);
		return;
	}
	/* Strictly speaking we don't own these now but we know we're single-threaded */
	while (contacts) {
		ChimeContact *contact = contacts->data;
		g_signal_connect(contact, "notify::availability",
				 G_CALLBACK(on_search_availability), sd);
		contacts = contacts->next;
	}
}

static void user_search_begin(PurpleConnection *conn, const char *query)
{
	ChimeConnection *cxn = PURPLE_CHIME_CXN(conn);

	chime_connection_autocomplete_contact_async(cxn, query, NULL, search_done, conn);
}

void chime_purple_user_search(PurplePluginAction *action)
{
	PurpleConnection *conn = (PurpleConnection *) action->context;
	purple_request_input(conn, _("Chime user lookup"),
			     _("Enter the user information to lookup"), NULL, NULL,
			     FALSE, FALSE, NULL,
			     _("Search"), PURPLE_CALLBACK(user_search_begin),
			     _("Cancel"), NULL,
			     NULL, NULL, NULL, conn);
}

struct conv_match_data {
	gpointer _conv;
	gboolean found;
};

static void match_conv_cb(ChimeConnection *cxn, ChimeConversation *conv, gpointer _d)
{
	struct conv_match_data *d = _d;

	if (d->_conv == conv)
		d->found = TRUE;
}

static void open_group_conv (PurpleBuddy *buddy, gpointer _conv)
{
	/* XXX: See https://developer.pidgin.im/ticket/12597 — we cannot
	 * ref the object or allocate anything, because there's no sane
	 * way to free/unref them (and I don't want to play the nasty
	 * tricks that SIPE does. Instead, we just search the existing
	 * conversations and see if this is in them. */
	ChimeConnection *cxn = PURPLE_CHIME_CXN(buddy->account->gc);
	struct conv_match_data d = {
		._conv = _conv,
		.found = FALSE,
	};
	chime_connection_foreach_conversation(cxn, match_conv_cb, &d);

	if (d.found)
		do_join_chat(buddy->account->gc, cxn, _conv, NULL, NULL);
}

struct conv_find_data {
	GList *menu;
	gpointer im_conv;
	const gchar *id;
};

static void group_conv_cb(ChimeConnection *cxn, ChimeConversation *conv, gpointer _d)
{
	struct conv_find_data *d = _d;

	if (conv == d->im_conv || !chime_conversation_has_member(conv, d->id))
		return;

	d->menu = g_list_append(d->menu, purple_menu_action_new(chime_conversation_get_name(conv),
								PURPLE_CALLBACK(open_group_conv), conv, NULL));
}

GList *chime_purple_buddy_menu(PurpleBuddy *buddy)
{
	struct purple_chime *pc = purple_connection_get_protocol_data(buddy->account->gc);
	ChimeConnection *cxn = CHIME_CONNECTION(pc->cxn);

	ChimeContact *contact = chime_connection_contact_by_email(cxn, buddy->name);
	if (!contact)
		return NULL;

	struct conv_find_data d = {
		.menu = NULL,
		.im_conv = NULL,
		.id = chime_contact_get_profile_id(contact),
	};
	struct chime_msgs *msgs = g_hash_table_lookup(pc->ims_by_email, buddy->name);
	if (msgs)
		d.im_conv = msgs->obj;

	chime_connection_foreach_conversation(cxn, group_conv_cb, &d);

	return g_list_append(NULL,  purple_menu_action_new(_("Group chats"), NULL, NULL, d.menu));
}
