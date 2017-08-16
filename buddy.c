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

/* Consimes buddies list */


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

static void on_buddystatus_changed(ChimeContact *contact, GParamSpec *ignored, PurpleConnection *conn)
{
	gboolean is_buddy;
	const gchar *email;
	ChimeAvailability availability;

	email = chime_contact_get_email(contact);
	availability = chime_contact_get_availability(contact);
	is_buddy = chime_contact_get_contacts_list(contact);

	if (!is_buddy) {
		g_signal_handlers_disconnect_matched(contact, G_SIGNAL_MATCH_FUNC|G_SIGNAL_MATCH_DATA,
						     0, 0, NULL, on_contact_availability, conn);
		g_signal_handlers_disconnect_matched(contact, G_SIGNAL_MATCH_FUNC|G_SIGNAL_MATCH_DATA,
						     0, 0, NULL, on_contact_display_name, conn);
		/* Don't remove from blist until we're fully connected because
		 * some contacts may appear first from conversations and only
		 * later from the contacts list. We don't want to delete them
		 * here only to add them back to the default "Chime Contacts"
		 * group later. */
		if (PURPLE_CONNECTION_IS_CONNECTED(conn)) {
			GSList *buddies = purple_find_buddies(conn->account, email);
			while (buddies) {
				purple_blist_remove_buddy(buddies->data);
				buddies = g_slist_remove(buddies, buddies->data);
			}
		}
	} else {
		const gchar *display_name = chime_contact_get_display_name(contact);

		/* Is there a better way to do this? In the absence of somewhere to
		 * easily store the handler ID returned from g_signal_connect()?
		 * We don't want to connect the same handler multiple times. */
		g_signal_handlers_disconnect_matched(contact, G_SIGNAL_MATCH_FUNC|G_SIGNAL_MATCH_DATA,
						     0, 0, NULL, on_contact_availability, conn);
		g_signal_handlers_disconnect_matched(contact, G_SIGNAL_MATCH_FUNC|G_SIGNAL_MATCH_DATA,
						     0, 0, NULL, on_contact_display_name, conn);
		g_signal_connect(contact, "notify::availability",
				 G_CALLBACK(on_contact_availability), conn);
		g_signal_connect(contact, "notify::display-name",
				 G_CALLBACK(on_contact_display_name), conn);

		GSList *buddies = purple_find_buddies(conn->account, email);
		if (buddies) {
			while (buddies) {
				PurpleBuddy *buddy = buddies->data;
				purple_blist_server_alias_buddy(buddy, display_name);
				buddies = g_slist_remove(buddies, buddy);
			}
			if (availability)
				purple_prpl_got_user_status(conn->account, email,
							    chime_availability_name(availability), NULL);
		} else {
			PurpleGroup *group = purple_find_group(_("Chime Contacts"));
			if (!group) {
				group = purple_group_new(_("Chime Contacts"));
				purple_blist_add_group(group, NULL);
			}
			PurpleBuddy *buddy = purple_buddy_new(conn->account, email, NULL);
			purple_blist_server_alias_buddy(buddy, display_name);
			purple_blist_add_buddy(buddy, NULL, group, NULL);
		}
	}
}

void on_chime_new_contact(ChimeConnection *cxn, ChimeContact *contact, PurpleConnection *conn)
{
	g_signal_connect(contact, "notify::dead",
			 G_CALLBACK(on_buddystatus_changed), conn);

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
		ChimeAvailability availability = chime_contact_get_availability(contact);

		purple_blist_server_alias_buddy(buddy, chime_contact_get_display_name(contact));
		if (availability)
			purple_prpl_got_user_status(conn->account, purple_buddy_get_name(buddy),
						    chime_availability_name(availability), NULL);
		if (chime_contact_get_contacts_list(contact))
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
		if (b != buddy) {
			g_slist_free(buddies);
			return;
		}
		buddies = g_slist_remove(buddies, b);
	}
	ChimeConnection *cxn = PURPLE_CHIME_CXN(conn);
	ChimeContact *contact = chime_connection_contact_by_email(cxn,
								  buddy->name);
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
