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
#include "chime-connection-private.h"

#include <libsoup/soup.h>

/* Consimes buddies list */


static void on_contact_availability(ChimeContact *contact, GParamSpec *ignored, PurpleConnection *conn)
{
	const gchar *email;
	ChimeAvailability availability;

	g_object_get(contact, "availability", &availability, "email", &email, NULL);

	if (availability)
		purple_prpl_got_user_status(conn->account, email, chime_statuses[availability], NULL);
}

static void on_contact_display_name(ChimeContact *contact, GParamSpec *ignored, PurpleConnection *conn)
{
	const gchar *email;
	const gchar *display_name;

	g_object_get(contact, "email", &email, "display-name", &display_name, NULL);

	GSList *buddies = purple_find_buddies(conn->account, email);
	while (buddies) {
		PurpleBuddy *buddy = buddies->data;
		purple_blist_server_alias_buddy(buddy, display_name);
		buddies = g_slist_remove(buddies, buddy);
	}

}

static void on_buddystatus_changed(ChimeContact *contact, GParamSpec *ignored, PurpleConnection *conn)
{
	gboolean is_buddy;
	const gchar *email;
	const gchar *display_name;
	ChimeAvailability availability;

	g_object_get(contact, "contacts-list", &is_buddy, "availability", &availability,
		     "email", &email, "display-name", &display_name, NULL);

	GSList *buddies = purple_find_buddies(conn->account, email);
	if (!is_buddy) {
		g_signal_handlers_disconnect_matched(contact, G_SIGNAL_MATCH_FUNC|G_SIGNAL_MATCH_DATA,
						     0, 0, NULL, on_contact_availability, conn);
		g_signal_handlers_disconnect_matched(contact, G_SIGNAL_MATCH_FUNC|G_SIGNAL_MATCH_DATA,
						     0, 0, NULL, on_contact_display_name, conn);
		while (buddies) {
			purple_blist_remove_buddy(buddies->data);
			buddies = g_slist_remove(buddies, buddies->data);
		}
	} else {
		g_signal_connect(contact, "notify::availability",
				 G_CALLBACK(on_contact_availability), conn);
		g_signal_connect(contact, "notify::display-name",
				 G_CALLBACK(on_contact_display_name), conn);
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
	gboolean is_buddy;
	printf("Got NEW_CONTACT For %p\n", contact);
	g_signal_connect(contact, "notify::contacts-list",
			 G_CALLBACK(on_buddystatus_changed), conn);

	g_object_get(contact, "contacts-list", &is_buddy, NULL);
	if (is_buddy)
		on_buddystatus_changed(contact, NULL, conn);
	printf("Done\n");
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
		ChimeAvailability availability;
		const gchar *display_name;
		gboolean is_buddy;

		g_object_get(contact, "contacts-list", &is_buddy, "availability", &availability,
			     "display-name", &display_name, NULL);

		purple_blist_server_alias_buddy(buddy, display_name);
		if (availability)
			purple_prpl_got_user_status(conn->account, purple_buddy_get_name(buddy),
						    chime_statuses[availability], NULL);
		if (is_buddy)
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
	chime_connection_remove_contact_async(cxn, purple_buddy_get_name(buddy),
					      NULL, on_buddy_removed, conn);
}

