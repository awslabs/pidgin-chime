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
const gchar *chime_meeting_get_start_at(ChimeMeeting *self);
const gchar *chime_meeting_get_channel(ChimeMeeting *self);
const gchar *chime_meeting_get_roster_channel(ChimeMeeting *self);

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

gboolean chime_connection_open_meeting(ChimeConnection *cxn, ChimeMeeting *meeting);
void chime_connection_close_meeting(ChimeConnection *cxn, ChimeMeeting *meeting);


void chime_connection_meeting_schedule_info_async(ChimeConnection *cxn,
						  gboolean onetime,
						  GCancellable *cancellable,
						  GAsyncReadyCallback callback,
						  gpointer user_data);

/* XXXX: Parse it and return a ChimeScheduleInfo instead */
JsonNode *chime_connection_meeting_schedule_info_finish(ChimeConnection *self,
							GAsyncResult *result,
							GError **error);


G_END_DECLS

#endif /* __CHIME_MEETING_H__ */
