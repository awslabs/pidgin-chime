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

#ifndef __CHIME_MEETING_H__
#define __CHIME_MEETING_H__

#include <glib-object.h>

#include <json-glib/json-glib.h>

#include "chime-connection.h"
#include "chime-contact.h"
#include "chime-object.h"
#include "chime-call.h"

G_BEGIN_DECLS

#define CHIME_TYPE_MEETING (chime_meeting_get_type ())
G_DECLARE_FINAL_TYPE (ChimeMeeting, chime_meeting, CHIME, MEETING, ChimeObject)

typedef enum {
	CHIME_MEETING_TYPE_ADHOC,
	CHIME_MEETING_TYPE_GOOGLE_CALENDAR,
	CHIME_MEETING_TYPE_CONFERENCE_BRIDGE,
	CHIME_MEETING_TYPE_WEBINAR,
} ChimeMeetingType;

#define CHIME_TYPE_MEETING_TYPE (chime_meeting_type_get_type ())
GType chime_meeting_type_get_type (void) G_GNUC_CONST;

const gchar *chime_meeting_get_id(ChimeMeeting *self);
const gchar *chime_meeting_get_name(ChimeMeeting *self);
ChimeContact *chime_meeting_get_organiser(ChimeMeeting *self);
const gchar *chime_meeting_get_passcode(ChimeMeeting *self);
const gchar *chime_meeting_get_id_for_display(ChimeMeeting *self);
const gchar *chime_meeting_get_screen_share_url(ChimeMeeting *self);
const gchar *chime_meeting_get_start_at(ChimeMeeting *self);
const gchar *chime_meeting_get_channel(ChimeMeeting *self);
const gchar *chime_meeting_get_roster_channel(ChimeMeeting *self);
ChimeRoom *chime_meeting_get_chat_room(ChimeMeeting *self);
ChimeCall *chime_meeting_get_call(ChimeMeeting *self);

gboolean chime_meeting_match_pin(ChimeMeeting *self, const gchar *pin);

ChimeMeeting *chime_connection_meeting_by_name(ChimeConnection *cxn,
					 const gchar *name);
ChimeMeeting *chime_connection_meeting_by_id(ChimeConnection *cxn,
				       const gchar *id);

/* Designed to match the NEW_MEETING signal handler */
typedef void (*ChimeMeetingCB) (ChimeConnection *, ChimeMeeting *, gpointer);
void chime_connection_foreach_meeting(ChimeConnection *cxn, ChimeMeetingCB cb,
				   gpointer cbdata);

typedef struct {
	ChimeContact *contact;
	gboolean admin;
	gboolean speaker;
	char *passcode;
} ChimeMeetingParticipant;

GList *chime_meeting_get_participants(ChimeMeeting *self);

void chime_connection_close_meeting(ChimeConnection *cxn, ChimeMeeting *meeting);

typedef struct {
	const gchar *country;
	const gchar *display_string;
	const gchar *number;
	const gchar *toll;
	const gchar *toll_free;
	const gchar *iso;
	const gchar *city;
	const gchar *city_code;
} ChimeDialin;

typedef struct {
	const gchar *delegate_scheduling_email;
	const gchar *display_vanity_url_prefix;
	const gchar *vanity_url;
	const gchar *vanity_name;
	const gchar *toll_dialin;
	const gchar *meeting_id_for_display;
	const gchar *bridge_screenshare_url;
	const gchar *display_vanity_url;
	const gchar *bridge_passcode;
	const gchar *international_dialin_info_url;
	const gchar *scheduling_address;
	const gchar *toll_free_dialin;
	const gchar *meeting_join_url;
	GSList *international_dialin_info;

	JsonNode *_node;
} ChimeScheduledMeeting;

void chime_scheduled_meeting_free(ChimeScheduledMeeting *mtg);

void chime_connection_meeting_schedule_info_async(ChimeConnection *cxn,
						  gboolean onetime,
						  GCancellable *cancellable,
						  GAsyncReadyCallback callback,
						  gpointer user_data);

ChimeScheduledMeeting *chime_connection_meeting_schedule_info_finish(ChimeConnection *self,
								     GAsyncResult *result,
								     GError **error);


void chime_connection_lookup_meeting_by_pin_async(ChimeConnection *cxn,
						  const gchar *pin,
						  GCancellable *cancellable,
						  GAsyncReadyCallback callback,
						  gpointer user_data);

ChimeMeeting *chime_connection_lookup_meeting_by_pin_finish(ChimeConnection *self,
							    GAsyncResult *result,
							    GError **error);

void chime_connection_join_meeting_async(ChimeConnection *cxn,
					 ChimeMeeting *meeting,
					 gboolean muted,
					 GCancellable *cancellable,
					 GAsyncReadyCallback callback,
					 gpointer user_data);

ChimeMeeting *chime_connection_join_meeting_finish(ChimeConnection *self,
						   GAsyncResult *result,
						   GError **error);

void chime_connection_create_meeting_async(ChimeConnection *cxn,
					   GSList *contacts,
					   gboolean bridge_locked,
					   gboolean create_bridge_passcode,
					   gboolean p2p,
					   GCancellable *cancellable,
					   GAsyncReadyCallback callback,
					   gpointer user_data);
ChimeMeeting *chime_connection_create_meeting_finish(ChimeConnection *self,
						     GAsyncResult *result,
						     GError **error);


void chime_connection_end_meeting_async(ChimeConnection *cxn,
					ChimeMeeting *meeting,
					GCancellable *cancellable,
					GAsyncReadyCallback callback,
					gpointer user_data);

gboolean chime_connection_end_meeting_finish(ChimeConnection *self,
					     GAsyncResult *result,
					     GError **error);
G_END_DECLS

#endif /* __CHIME_MEETING_H__ */
