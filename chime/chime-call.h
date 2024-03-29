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

#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>

G_BEGIN_DECLS

#define CHIME_TYPE_CALL (chime_call_get_type ())
G_DECLARE_FINAL_TYPE (ChimeCall, chime_call, CHIME, CALL, ChimeObject)

typedef enum {
	CHIME_PARTICIPATION_PRESENT,
	CHIME_PARTICIPATION_CHECKED_IN,
	CHIME_PARTICIPATION_HUNG_UP,
	CHIME_PARTICIPATION_DROPPED,
	CHIME_PARTICIPATION_RUNNING_LATE,
	CHIME_PARTICIPATION_INVITED,
	CHIME_PARTICIPATION_DECLINED,
	CHIME_PARTICIPATION_INACTIVE,
} ChimeCallParticipationStatus;

#define CHIME_TYPE_CALL_PARTICIPATION_STATUS (chime_call_participation_status_get_type ())
GType chime_call_participation_status_get_type (void) G_GNUC_CONST;

typedef enum {
	CHIME_SHARED_SCREEN_NONE,
	CHIME_SHARED_SCREEN_VIEWING,
	CHIME_SHARED_SCREEN_PRESENTING,
} ChimeCallSharedScreenStatus;

#define CHIME_TYPE_CALL_SHARED_SCREEN_STATUS (chime_call_shared_screen_status_get_type ())
GType chime_call_shared_screen_status_get_type (void) G_GNUC_CONST;

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

/* Is this an audio-less call, where we are just "checked in"? */
void chime_call_set_silent(ChimeCall *call, gboolean silent);
gboolean chime_call_get_silent(ChimeCall *call);

/* Audio call, but we want to be quiet now. */
void chime_call_set_local_mute(ChimeCall *call, gboolean muted);

typedef struct {
	gchar *participant_id;
	gchar *participant_type;
	gchar *full_name;
	gchar *email;
	ChimeCallParticipationStatus status;
	ChimeCallSharedScreenStatus shared_screen;
	gboolean admin;
	gboolean speaker;
	gboolean pots;
	gboolean video_present;
	int volume;
	int signal_strength;
	char *passcode;
} ChimeCallParticipant;

GList *chime_call_get_participants(ChimeCall *self);

struct _ChimeCallAudio;
typedef struct _ChimeCallAudio ChimeCallAudio;

struct _ChimeCallScreen;
typedef struct _ChimeCallScreen ChimeCallScreen;
typedef struct _ChimeWebcamScreen ChimeWebcamScreen;


void chime_call_emit_participants(ChimeCall *call);

typedef enum {
	CHIME_AUDIO_STATE_CONNECTING = 0,
	CHIME_AUDIO_STATE_FAILED,
	CHIME_AUDIO_STATE_HANGUP,
	CHIME_AUDIO_STATE_AUDIOLESS,
	CHIME_AUDIO_STATE_AUDIO,
	CHIME_AUDIO_STATE_AUDIO_MUTED,
} ChimeAudioState;

typedef enum {
	CHIME_SCREEN_STATE_CONNECTING = 0,
	CHIME_SCREEN_STATE_FAILED,
	CHIME_SCREEN_STATE_HANGUP,
	CHIME_SCREEN_STATE_CONNECTED,
	CHIME_SCREEN_STATE_VIEWING,
	CHIME_SCREEN_STATE_SENDING,
} ChimeScreenState;

void chime_call_install_gst_app_callbacks(ChimeCall *call, GstAppSrc *appsrc, GstAppSink *appsink);
void chime_call_view_screen(ChimeConnection *cxn, ChimeCall *call, GstAppSrc *appsrc);
void chime_call_send_screen(ChimeConnection *cxn, ChimeCall *call, GstAppSink *appsink);

G_END_DECLS

#endif /* __CHIME_CALL_H__ */
