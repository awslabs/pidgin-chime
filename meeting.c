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
#include <debug.h>

#include "chime.h"
#include "chime-meeting.h"

static gchar *format_pin(const gchar *pin)
{
	if (strlen(pin) == 10)
		return g_strdup_printf("%.4s %.2s %.4s", pin, pin + 4, pin + 6);
	else if (strlen(pin) == 13)
		return g_strdup_printf("%.4s %.2s %.4s %.3s", pin, pin + 4, pin + 6, pin + 10);
	else
		return g_strdup(pin);
}

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

	gchar *secondary = g_strdup_printf(_("Remember to invite:\n%s, %s"),
					   "meet@chime.aws", mtg->delegate_scheduling_email);
	GString *invite_str = g_string_new("");

	g_string_append_printf(invite_str, _("---------- %s ----------<br><br>"),
		_("Amazon Chime Meeting Information"));
	g_string_append_printf(invite_str, _("You have been invited to an online meeting, powered by Amazon Chime.<br><br>"));
	g_string_append_printf(invite_str, _("1. Click to join the meeting: %s<br>Meeting ID: %s<br><br>"),
			       mtg->meeting_join_url, mtg->meeting_id_for_display);
	if (mtg->bridge_passcode) {
		gchar *pin = format_pin(mtg->bridge_passcode);

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

static void join_mtg_done(GObject *source, GAsyncResult *result, gpointer _conn)
{
	ChimeConnection *cxn = CHIME_CONNECTION(source);
	PurpleConnection *conn = _conn;
	GError *error = NULL;
	ChimeMeeting *mtg = chime_connection_join_meeting_finish(CHIME_CONNECTION(source), result, &error);

	if (!mtg) {
		purple_notify_error(conn, NULL,
				    _("Unable to join meeting"),
				    error->message);
		return;
	}
	ChimeRoom *room = chime_meeting_get_chat_room(mtg);
	if (room)
		do_join_chat(conn, cxn, CHIME_OBJECT(room), NULL, mtg);
}

struct pin_join_data {
	gchar *query;
	PurpleConnection *conn;
};

static void pin_join_done(GObject *source, GAsyncResult *result, gpointer _pjd)
{
	struct pin_join_data *pjd = _pjd;
	struct purple_chime *pc = purple_connection_get_protocol_data(pjd->conn);
	ChimeConnection *cxn = CHIME_CONNECTION(source);
	GError *error = NULL;
	ChimeMeeting *mtg = chime_connection_lookup_meeting_by_pin_finish(cxn, result, &error);

	if (!mtg) {
		purple_notify_error(pjd->conn, NULL,
				    _("Unable to lookup meeting"),
				    error->message);
	} else {
		chime_connection_join_meeting_async(cxn, mtg, NULL, join_mtg_done, pjd->conn);
	}

	pc->pin_joins = g_slist_remove(pc->pin_joins, pjd->query);
	free(pjd->query);
	free(pjd);
}

static void pin_join_begin(PurpleConnection *conn, const char *query)
{
	struct purple_chime *pc = purple_connection_get_protocol_data(conn);
	ChimeConnection *cxn = PURPLE_CHIME_CXN(conn);
	struct pin_join_data *pjd = g_new0(struct pin_join_data, 1);

	pjd->conn = conn;
	pjd->query = g_strdup(query);
	pc->pin_joins = g_slist_prepend(pc->pin_joins, pjd->query);

	chime_connection_lookup_meeting_by_pin_async(cxn, query, NULL,
						     pin_join_done, pjd);
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

static void join_joinable(PurpleConnection *conn, GList *row, gpointer _unused)
{
	ChimeConnection *cxn = PURPLE_CHIME_CXN(conn);
	if (!row)
		return;

	/* XXX Do it by PIN */
	gchar *name = g_list_nth_data(row, 1);
	purple_debug(PURPLE_DEBUG_INFO, "chime", "Join meeting %s\n", name);
	ChimeMeeting *mtg = chime_connection_meeting_by_name(cxn, name);
	if (mtg)
		chime_connection_join_meeting_async(cxn, mtg, NULL, join_mtg_done, conn);
}

static void append_mtg(ChimeConnection *cxn, ChimeMeeting *mtg, gpointer _results)
{
	PurpleNotifySearchResults *results = _results;
	ChimeContact *organiser = chime_meeting_get_organiser(mtg);

	GList *row = NULL;
	row = g_list_append(row, format_pin(chime_meeting_get_passcode(mtg)));
	row = g_list_append(row, g_strdup(chime_meeting_get_name(mtg)));
	row = g_list_append(row, g_strdup_printf("%s <%s>", chime_contact_get_display_name(organiser),
						 chime_contact_get_email(organiser)));

	purple_notify_searchresults_row_add(results, row);
}


static PurpleNotifySearchResults *generate_joinable_results(PurpleConnection *conn)
{
	PurpleNotifySearchResults *results = purple_notify_searchresults_new();
	PurpleNotifySearchColumn *column;

	column = purple_notify_searchresults_column_new(_("Passcode"));
	purple_notify_searchresults_column_add(results, column);
	column = purple_notify_searchresults_column_new(_("Summary"));
	purple_notify_searchresults_column_add(results, column);
	column = purple_notify_searchresults_column_new(_("Organiser"));
	purple_notify_searchresults_column_add(results, column);

	purple_notify_searchresults_button_add(results, PURPLE_NOTIFY_BUTTON_JOIN, join_joinable);

	chime_connection_foreach_meeting(PURPLE_CHIME_CXN(conn), append_mtg, results);
	return results;
}

static gboolean update_joinable(gpointer _conn)
{
	PurpleConnection *conn = _conn;
	struct purple_chime *pc = purple_connection_get_protocol_data(conn);

	PurpleNotifySearchResults *results = generate_joinable_results(conn);
	purple_notify_searchresults_new_rows(conn, results, pc->joinable_handle);

	pc->joinable_refresh_id = 0;
	return FALSE;
}


static void on_joinable_changed(ChimeMeeting *mtg, GParamSpec *ignored, PurpleConnection *conn)
{
	struct purple_chime *pc = purple_connection_get_protocol_data(conn);

	if (!pc->joinable_refresh_id)
		pc->joinable_refresh_id = g_idle_add(update_joinable, conn);
}

static void unsub_mtg(ChimeConnection *cxn, ChimeMeeting *mtg, gpointer _conn)
{
	g_signal_handlers_disconnect_matched(mtg, G_SIGNAL_MATCH_FUNC|G_SIGNAL_MATCH_DATA,
					     0, 0, NULL, on_joinable_changed, _conn);
}

static void joinable_closed_cb(gpointer _conn)
{
	PurpleConnection *conn = _conn;
	struct purple_chime *pc = purple_connection_get_protocol_data(conn);

	if (!pc)
		return;

	if (pc->joinable_refresh_id) {
		g_source_remove(pc->joinable_refresh_id);
		pc->joinable_refresh_id = 0;
	}
	pc->joinable_handle = NULL;

	chime_connection_foreach_meeting(PURPLE_CHIME_CXN(conn), unsub_mtg, conn);
}

static void sub_mtg(ChimeConnection *cxn, ChimeMeeting *mtg, gpointer _conn)
{
	PurpleConnection *conn = _conn;

	g_signal_connect(mtg, "notify::passcode",
			 G_CALLBACK(on_joinable_changed), conn);
	g_signal_connect(mtg, "notify::name",
			 G_CALLBACK(on_joinable_changed), conn);
	g_signal_connect(mtg, "ended",
			 G_CALLBACK(on_joinable_changed), conn);
}

void on_chime_new_meeting(ChimeConnection *cxn, ChimeMeeting *mtg, PurpleConnection *conn)
{
	struct purple_chime *pc = purple_connection_get_protocol_data(conn);

	if (pc->joinable_handle) {
		if (mtg)
			sub_mtg(cxn, mtg, conn);

		if (!pc->joinable_refresh_id)
			pc->joinable_refresh_id = g_idle_add(update_joinable, conn);
		return;
	}

	/* Don't pop up the 'Joinable Meetings' dialog if this is was triggered by a PIN join.
	   We're about to join it directly anyway. */
	if (mtg) {
		GSList *l;
		for (l = pc->pin_joins; l; l = l->next) {
			if (chime_meeting_match_pin(mtg, l->data))
				return;
		}
	}

	PurpleNotifySearchResults *results = generate_joinable_results(conn);
	pc->joinable_handle = purple_notify_searchresults(conn, _("Joinable Chime Meetings"), _("Joinable Meetings:"),
							  conn->account->username, results, joinable_closed_cb, conn);
	if (!pc->joinable_handle) {
		purple_notify_error(conn, NULL,
				    _("Unable to display joinable meetings."),
				    NULL);
		joinable_closed_cb(conn);
	}

	chime_connection_foreach_meeting(PURPLE_CHIME_CXN(conn), sub_mtg, conn);
}

void chime_purple_show_joinable(PurplePluginAction *action)
{
	PurpleConnection *conn = (PurpleConnection *) action->context;

	on_chime_new_meeting(PURPLE_CHIME_CXN(conn), NULL, conn);
}

void purple_chime_init_meetings(PurpleConnection *conn)
{
}

void purple_chime_destroy_meetings(PurpleConnection *conn)
{
	struct purple_chime *pc = purple_connection_get_protocol_data(conn);

	if (pc->joinable_handle)
		joinable_closed_cb(conn);
}
