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

#include <glib/gi18n.h>
#include <glib/glist.h>

#include <prpl.h>
#include <request.h>

#include "chime.h"
#include "chime-meeting.h"

static void schedule_meeting_cb(GObject *source, GAsyncResult *result, gpointer _conn)
{
	PurpleConnection *conn = _conn;
	GError *error = NULL;

	ChimeScheduledMeeting *mtg = chime_connection_meeting_schedule_info_finish(CHIME_CONNECTION(source),
										   result, &error);
	if (!mtg) {
		purple_notify_error(conn, NULL,
				    _("Unable to schedule meeting"),
				    error->message);
		return;
	}

	gchar *secondary = g_strdup_printf(_("Remember to include Chime in the invites:\n%s"), mtg->delegate_scheduling_email);
	GString *invite_str = g_string_new("");

	g_string_append_printf(invite_str, _("---------- %s ----------<br><br>"),
		_("Amazon Chime Meeting Information"));
	g_string_append_printf(invite_str, _("You have been invited to an online meeting, powered by Amazon Chime.<br><br>"));
	g_string_append_printf(invite_str, _("1. Click to join the meeting: %s<br>Meeting ID: %s<br><br>"),
			       mtg->meeting_join_url, mtg->meeting_id_for_display);
	if (mtg->bridge_passcode) {
		gchar *pin = (gchar *)mtg->bridge_passcode;
		if (strlen(pin) == 10)
			pin = g_strdup_printf("%.4s %.2s %.4s", pin, pin + 4, pin + 6);
		else if (strlen(pin) == 13)
			pin = g_strdup_printf("%.4s %.2s %.4s %.3s", pin, pin + 4, pin + 6, pin + 10);

		g_string_append_printf(invite_str, _("2. You can use your computer's microphone and speakers; however, a headset is recommended. Or, call in using your phone:<br><br>"));
		GSList *l = mtg->international_dialin_info;
		if (!l) {
			if (mtg->toll_free_dialin)
				g_string_append_printf(invite_str, _("Toll Free: %s<br>"),
						       mtg->toll_free_dialin);
			if (mtg->toll_dialin)
				g_string_append_printf(invite_str, _("Toll: %s<br>"),
						       mtg->toll_dialin);
		} else for (; l; l = l->next) {
			ChimeDialin *d = l->data;

			g_string_append_printf(invite_str, _("%s: %s<br>"),
					       d->display_string, d->number);
		}
		g_string_append_printf(invite_str, _("<br>Meeting PIN: %s<br><br>"), pin);
		g_string_append_printf(invite_str, _("One-click Mobile Dial-in: %s,,%s#<br><br>"),
				       mtg->toll_free_dialin ? : mtg->toll_dialin, mtg->bridge_passcode);
		g_string_append_printf(invite_str, _("International: %s<br><br>"),
				       mtg->international_dialin_info_url);

		if (pin != mtg->bridge_passcode)
			g_free(pin);

	}
	g_string_append_printf(invite_str, "---------- %s ---------",
			       _("End of Amazon Chime Meeting Information"));

	purple_notify_formatted(conn, _("Amazon Chime Meeting Information"),
				_("Meeting invite template"),
		secondary, invite_str->str, NULL, NULL);

	g_free(secondary);
	g_string_free(invite_str, TRUE);

	chime_scheduled_meeting_free(mtg);
}

static void do_schedule_meeting(PurplePluginAction *action, gboolean onetime)
{
	PurpleConnection *conn = (PurpleConnection *) action->context;
	ChimeConnection *cxn = PURPLE_CHIME_CXN(conn);

	chime_connection_meeting_schedule_info_async(cxn, onetime, NULL,
						     schedule_meeting_cb, conn);
}

void chime_purple_schedule_onetime(PurplePluginAction *action)
{
	do_schedule_meeting(action, TRUE);
}
void chime_purple_schedule_personal(PurplePluginAction *action)
{
	do_schedule_meeting(action, FALSE);
}

static void pin_join_done(GObject *source, GAsyncResult *result, gpointer _conn)
{
	PurpleConnection *conn = _conn;
	GError *error = NULL;
	ChimeMeeting *mtg = chime_connection_lookup_meeting_by_pin_finish(CHIME_CONNECTION(source), result, &error);

	if (!mtg) {
		purple_notify_error(conn, NULL,
				    _("Unable to join meeting"),
				    error->message);
		return;
	}
	/* Actually we'll handle it in the NEW_MEETING signal handler */
}

static void pin_join_begin(PurpleConnection *conn, const char *query)
{
	ChimeConnection *cxn = PURPLE_CHIME_CXN(conn);

	chime_connection_lookup_meeting_by_pin_async(cxn, query, NULL,
						     pin_join_done, conn);
}

void chime_purple_pin_join(PurplePluginAction *action)
{
	PurpleConnection *conn = (PurpleConnection *) action->context;

	purple_request_input(conn, _("Chime PIN join meeting"),
			     _("Enter the meeting PIN"), NULL, NULL,
			     FALSE, FALSE, NULL,
			     _("Search"), PURPLE_CALLBACK(pin_join_begin),
			     _("Cancel"), NULL,
			     NULL, NULL, NULL, conn);
}

void on_chime_new_meeting(ChimeConnection *cxn, ChimeMeeting *mtg, PurpleConnection *conn)
{
	gchar *secondary = g_strdup_printf(_("Meeting PIN: %s"),
					     chime_meeting_get_passcode(mtg));

	gchar *text = g_strdup_printf(_("Web join URL: %s"),
				      chime_meeting_get_passcode(mtg));

	purple_notify_formatted(conn, _("Amazon Chime Meeting"),
				chime_meeting_get_name(mtg), secondary,
				text, NULL, NULL);
	g_free(text);
	g_free(secondary);
	g_object_unref(mtg);
}
