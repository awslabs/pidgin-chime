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

#ifndef __CHIME_CALL_H__
#define __CHIME_CALL_H__

#include <glib-object.h>

#include <json-glib/json-glib.h>

#include "chime-connection.h"
#include "chime-contact.h"
#include "chime-object.h"

G_BEGIN_DECLS

#define CHIME_TYPE_CALL (chime_call_get_type ())
G_DECLARE_FINAL_TYPE (ChimeCall, chime_call, CHIME, CALL, ChimeObject)

typedef enum {
	CHIME_PARTICIPATION_PRESENT,
	CHIME_PARTICIPATION_CHECKED_IN,
	CHIME_PARTICIPATION_INVITED,
	CHIME_PARTICIPATION_HUNG_UP,
	CHIME_PARTICIPATION_DROPPED,
	CHIME_PARTICIPATION_RUNNING_LATE,
	CHIME_PARTICIPATION_DECLINED,
	CHIME_PARTICIPATION_INACTIVE,
} ChimeCallParticipationStatus;

#define CHIME_TYPE_CALL_PARTICIPATION_STATUS (chime_call_participation_status_get_type ())
GType chime_call_participation_status_get_type (void) G_GNUC_CONST;

gboolean chime_call_get_ongoing(ChimeCall *self);
const gchar *chime_call_get_uuid(ChimeCall *self);
const gchar *chime_call_get_channel(ChimeCall *self);
const gchar *chime_call_get_roster_channel(ChimeCall *self);
const gchar *chime_call_get_alert_body(ChimeCall *self);
const gchar *chime_call_get_host(ChimeCall *self);
const gchar *chime_call_get_media_host(ChimeCall *self);
const gchar *chime_call_get_audio_ws_url(ChimeCall *self);
const gchar *chime_call_get_control_url(ChimeCall *self);
const gchar *chime_call_get_desktop_bithub_url(ChimeCall *self);
const gchar *chime_call_get_mobile_bithub_url(ChimeCall *self);
const gchar *chime_call_get_stun_server_url(ChimeCall *self);

typedef struct {
	ChimeContact *contact;
	ChimeCallParticipationStatus status;
	gboolean admin;
	gboolean speaker;
	char *passcode;
} ChimeCallParticipant;

GList *chime_call_get_participants(ChimeCall *self);

void chime_connection_close_call(ChimeConnection *cxn, ChimeCall *call);


void chime_connection_join_call_async(ChimeConnection *cxn,
				      ChimeCall *call,
				      GCancellable *cancellable,
				      GAsyncReadyCallback callback,
				      gpointer user_data);

ChimeCall *chime_connection_join_call_finish(ChimeConnection *self,
					     GAsyncResult *result,
					     GError **error);

G_END_DECLS

#endif /* __CHIME_CALL_H__ */
