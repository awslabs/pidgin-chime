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

/* Consimes buddies list */


static void on_contact_availability(ChimeContact *contact, GParamSpec *ignored, PurpleConnection *conn)
{
	ChimeAvailability availability = chime_contact_get_availability(contact);

	if (availability)
		purple_prpl_got_user_status(conn->account, chime_contact_get_email(contact), chime_statuses[availability], NULL);
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
							    chime_statuses[availability], NULL);
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
	printf("buddy_free %s\n", purple_buddy_get_name(buddy));
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
	ChimeConnection *cxn = purple_connection_get_protocol_data(conn);
	ChimeContact *contact = chime_connection_contact_by_email(cxn,
								  purple_buddy_get_name(buddy));
	if (contact) {
		ChimeAvailability availability = chime_contact_get_availability(contact);

		purple_blist_server_alias_buddy(buddy, chime_contact_get_display_name(contact));
		if (availability)
			purple_prpl_got_user_status(conn->account, purple_buddy_get_name(buddy),
						    chime_statuses[availability], NULL);
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
	ChimeConnection *cxn = purple_connection_get_protocol_data(conn);
	ChimeContact *contact = chime_connection_contact_by_email(cxn,
								  buddy->name);
	g_signal_handlers_disconnect_matched(contact, G_SIGNAL_MATCH_DATA,
					     0, 0, NULL, NULL, conn);

	chime_connection_remove_contact_async(cxn, buddy->name,
					      NULL, on_buddy_removed, conn);
}

