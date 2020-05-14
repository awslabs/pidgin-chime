/*
 * Pidgin/libpurple Chime client plugin
 *
 * Copyright © 2017 Amazon.com, Inc. or its affiliates.
 *
 * Authors: David Woodhouse <dwmw2@infradead.org>
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


#include "chime-connection-private.h"
#include "chime-call.h"
#include "chime-call-audio.h"
#include "chime-call-screen.h"

#include <glib/gi18n.h>

#define BOOL_PROPS(x)							\
	x(ongoing, ONGOING, "ongoing?", "ongoing", "ongoing", TRUE)	\
	x(is_recording, IS_RECORDING, "is_recording", "is-recording", "is recording", TRUE)

#define STRING_PROPS(x)							\
	x(channel, CHANNEL, "channel", "channel", "channel", TRUE)	\
	x(roster_channel, ROSTER_CHANNEL, "roster_channel", "roster-channel", "roster channel", TRUE) \
	x(host, HOST, "host", "host", "host", TRUE)			\
	x(media_host, MEDIA_HOST, "media_host", "media-host", "media host", TRUE) \
	x(mobile_bithub_url, MOBILE_BITHUB_URL, "mobile_bithub_url", "mobile-bithub-url", "mobile bithub url", TRUE) \
	x(desktop_bithub_url, DESKTOP_BITHUB_URL, "desktop_bithub_url", "desktop-bithub-url", "desktop bithub url", TRUE) \
	x(control_url, CONTROL_URL, "control_url", "control-url", "control url", TRUE) \
	x(stun_server_url, STUN_SERVER_URL, "stun_server_url", "stun-server-url", "stun server url", TRUE) \
	x(audio_ws_url, AUDIO_WS_URL, "audio_ws_url", "audio-ws-url", "audio ws url", TRUE)

#define CHIME_PROP_OBJ_VAR call

#include "chime-props.h"

enum
{
	PROP_0,

	CHIME_PROPS_ENUM

	LAST_PROP,
};

static GParamSpec *props[LAST_PROP];

enum {
	ENDED,
	AUDIO_STATE,
	SCREEN_STATE,
	PARTICIPANTS_CHANGED,
	NEW_PRESENTER,
	LAST_SIGNAL,
};

static guint signals[LAST_SIGNAL];

struct _ChimeCall {
	ChimeObject parent_instance;

	CHIME_PROPS_VARS

	GHashTable *participants;
	ChimeCallParticipant *presenter;

	ChimeCallAudio *audio;
	ChimeCallScreen *screen;
	guint opens;
};

G_DEFINE_TYPE(ChimeCall, chime_call, CHIME_TYPE_OBJECT)

#define CHIME_PARTICIPATION_VALUES					  \
	{ CHIME_PARTICIPATION_PRESENT,		"present",	N_("present") }, \
	{ CHIME_PARTICIPATION_CHECKED_IN,	"checked_in",	N_("checkedin") }, \
	{ CHIME_PARTICIPATION_HUNG_UP,		"hung_up",	N_("hungup") }, \
	{ CHIME_PARTICIPATION_DROPPED,		"dropped",	N_("dropped") }, \
	{ CHIME_PARTICIPATION_RUNNING_LATE,	"running_late",	N_("runninglate") }, \
	{ CHIME_PARTICIPATION_INVITED,		"invited",	N_("invited") }, \
	{ CHIME_PARTICIPATION_DECLINED,		"declined",	N_("declined") }, \
	{ CHIME_PARTICIPATION_INACTIVE,		"inactive",	N_("inactive") },

CHIME_DEFINE_ENUM_TYPE(ChimeCallParticipationStatus, chime_call_participation_status,
		       CHIME_PARTICIPATION_VALUES)

#define CHIME_SHARED_SCREEN_STATUS_VALUES				\
	{ CHIME_SHARED_SCREEN_NONE,	"none",		"" },		\
	{ CHIME_SHARED_SCREEN_VIEWING,	"viewing",	N_("viewing") },\
	{ CHIME_SHARED_SCREEN_PRESENTING,"presenting",	N_("presenting") },

CHIME_DEFINE_ENUM_TYPE(ChimeCallSharedScreenStatus, chime_call_shared_screen_status,
		       CHIME_SHARED_SCREEN_STATUS_VALUES)

static void unsub_call(gpointer key, gpointer val, gpointer data);
static void free_participant(void *p);

static void
chime_call_dispose(GObject *object)
{
	ChimeCall *self = CHIME_CALL(object);

	chime_debug("Call disposed: %p\n", self);

	if (self->opens)
		unsub_call(NULL, self, NULL);

	g_signal_emit(self, signals[ENDED], 0, NULL);

	g_clear_pointer(&self->participants, g_hash_table_destroy);

	G_OBJECT_CLASS(chime_call_parent_class)->dispose(object);
}

static void
chime_call_finalize(GObject *object)
{
	ChimeCall *self = CHIME_CALL(object);

	CHIME_PROPS_FREE

	G_OBJECT_CLASS(chime_call_parent_class)->finalize(object);
}

static void chime_call_get_property(GObject *object, guint prop_id,
				    GValue *value, GParamSpec *pspec)
{
	ChimeCall *self = CHIME_CALL(object);

	switch (prop_id) {

	CHIME_PROPS_GET

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void chime_call_set_property(GObject *object, guint prop_id,
				    const GValue *value, GParamSpec *pspec)
{
	ChimeCall *self = CHIME_CALL(object);

	switch (prop_id) {

	CHIME_PROPS_SET

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void chime_call_class_init(ChimeCallClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->finalize = chime_call_finalize;
	object_class->dispose = chime_call_dispose;
	object_class->get_property = chime_call_get_property;
	object_class->set_property = chime_call_set_property;

	CHIME_PROPS_REG

	g_object_class_install_properties(object_class, LAST_PROP, props);

	signals[ENDED] =
		g_signal_new ("ended",
			      G_OBJECT_CLASS_TYPE (object_class), G_SIGNAL_RUN_FIRST,
			      0, NULL, NULL, NULL, G_TYPE_NONE, 0);

	signals[AUDIO_STATE] =
		g_signal_new ("audio-state",
			      G_OBJECT_CLASS_TYPE (object_class), G_SIGNAL_RUN_FIRST,
			      0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_STRING);

	signals[SCREEN_STATE] =
		g_signal_new ("screen-state",
			      G_OBJECT_CLASS_TYPE (object_class), G_SIGNAL_RUN_FIRST,
			      0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_STRING);

	signals[PARTICIPANTS_CHANGED] =
		g_signal_new ("participants-changed",
			      G_OBJECT_CLASS_TYPE (object_class), G_SIGNAL_RUN_FIRST,
			      0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_HASH_TABLE);

	signals[NEW_PRESENTER] =
		g_signal_new ("new_presenter",
			      G_OBJECT_CLASS_TYPE (object_class), G_SIGNAL_RUN_FIRST,
			      0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_POINTER);
}

static void chime_call_init(ChimeCall *self)
{
	self->participants = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, free_participant);
}


/* Internal only */
ChimeConnection *chime_call_get_connection(ChimeCall *self)
{
	g_return_val_if_fail(CHIME_IS_CALL(self), NULL);

	return chime_object_get_connection(CHIME_OBJECT(self));
}

gboolean chime_call_get_ongoing(ChimeCall *self)
{
	g_return_val_if_fail(CHIME_IS_CALL(self), FALSE);

	return self->ongoing;
}

const gchar *chime_call_get_uuid(ChimeCall *self)
{
	g_return_val_if_fail(CHIME_IS_CALL(self), NULL);

	return chime_object_get_id(CHIME_OBJECT(self));
}

const gchar *chime_call_get_channel(ChimeCall *self)
{
	g_return_val_if_fail(CHIME_IS_CALL(self), NULL);

	return self->channel;
}

const gchar *chime_call_get_roster_channel(ChimeCall *self)
{
	g_return_val_if_fail(CHIME_IS_CALL(self), NULL);

	return self->roster_channel;
}

const gchar *chime_call_get_alert_body(ChimeCall *self)
{
	g_return_val_if_fail(CHIME_IS_CALL(self), NULL);

	return chime_object_get_name(CHIME_OBJECT(self));
}

const gchar *chime_call_get_host(ChimeCall *self)
{
	g_return_val_if_fail(CHIME_IS_CALL(self), NULL);

	return self->host;
}

const gchar *chime_call_get_media_host(ChimeCall *self)
{
	g_return_val_if_fail(CHIME_IS_CALL(self), NULL);

	return self->media_host;
}

const gchar *chime_call_get_mobile_bithub_url(ChimeCall *self)
{
	g_return_val_if_fail(CHIME_IS_CALL(self), NULL);

	return self->mobile_bithub_url;
}

const gchar *chime_call_get_desktop_bithub_url(ChimeCall *self)
{
	g_return_val_if_fail(CHIME_IS_CALL(self), NULL);

	return self->desktop_bithub_url;
}

const gchar *chime_call_get_control_url(ChimeCall *self)
{
	g_return_val_if_fail(CHIME_IS_CALL(self), NULL);

	return self->control_url;
}

const gchar *chime_call_get_stun_server_url(ChimeCall *self)
{
	g_return_val_if_fail(CHIME_IS_CALL(self), NULL);

	return self->stun_server_url;
}

const gchar *chime_call_get_audio_ws_url(ChimeCall *self)
{
	g_return_val_if_fail(CHIME_IS_CALL(self), NULL);

	return self->audio_ws_url;
}

static gboolean parse_call_participation_status(JsonNode *node, const gchar *member, ChimeCallParticipationStatus *type)
{
	const gchar *str;

	if (!parse_string(node, member, &str))
		return FALSE;

	gpointer klass = g_type_class_ref(CHIME_TYPE_CALL_PARTICIPATION_STATUS);
	GEnumValue *val = g_enum_get_value_by_name(klass, str);
	g_type_class_unref(klass);

	if (!val)
		return FALSE;
	*type = val->value;
	return TRUE;
}

static gboolean parse_call_shared_screen_status(JsonNode *node, const gchar *member,
						ChimeCallSharedScreenStatus *type)
{
	const gchar *str;

	if (!parse_string(node, member, &str))
		return FALSE;

	gpointer klass = g_type_class_ref(CHIME_TYPE_CALL_SHARED_SCREEN_STATUS);
	GEnumValue *val = g_enum_get_value_by_name(klass, str);
	g_type_class_unref(klass);

	if (!val)
		return FALSE;
	*type = val->value;
	return TRUE;
}

static void free_participant(void *_p)
{
	ChimeCallParticipant *p = _p;

	free(p->participant_id);
	free(p->participant_type);
	free(p->full_name);
	free(p->email);
	free(p);
}

static gboolean parse_participant(ChimeConnection *cxn, ChimeCall *call, JsonNode *p,
				  ChimeCallParticipant **presenter)
{
	const gchar *participant_id, *full_name, *participant_type;
	gboolean pots, speaker;
	ChimeCallParticipationStatus status;

	if (!parse_string(p, "participant_id", &participant_id) ||
	    !parse_string(p, "full_name", &full_name) ||
	    !parse_string(p, "participant_type", &participant_type) ||
	    !parse_call_participation_status(p, "status", &status) ||
	    !parse_boolean(p, "pots?", &pots) ||
	    !parse_boolean(p, "speaker?", &speaker))
		return FALSE;

	const gchar *email = NULL;
	parse_string(p, "email", &email);

	ChimeCallSharedScreenStatus screen = CHIME_SHARED_SCREEN_NONE;
	parse_call_shared_screen_status(p, "shared_screen_indicator", &screen);

	ChimeCallParticipant *cp = g_hash_table_lookup(call->participants, (void *)participant_id);
	if (!cp) {
		cp = g_new0(ChimeCallParticipant, 1);
		cp->volume = -128;
		cp->participant_id = g_strdup(participant_id);
		cp->participant_type = g_strdup(participant_type);
		cp->full_name = g_strdup(full_name);
		if (email)
			cp->email = g_strdup(email);
		g_hash_table_insert(call->participants, (void *)cp->participant_id, cp);
	}
	cp->pots = pots;
	cp->speaker = speaker;
	cp->status = status;
	cp->shared_screen = screen;

	if (screen == CHIME_SHARED_SCREEN_PRESENTING)
		*presenter = cp;

	if (!strcmp(participant_id, chime_connection_get_profile_id(cxn))) {
		JsonObject *obj = json_node_get_object(p);
		JsonNode *muter = json_object_get_member(obj, "muter");
		if (muter && json_node_get_node_type(muter) != JSON_NODE_NULL) {
			if (call->audio)
				chime_call_audio_local_mute(call->audio, TRUE);
		}
	}

	return TRUE;
}

static gboolean call_roster_cb(ChimeConnection *cxn, gpointer _call, JsonNode *data_node)
{
	ChimeCall *call = CHIME_CALL(_call);
	JsonObject *obj = json_node_get_object(data_node);
	JsonNode *record = json_object_get_member(obj, "record");
	if (!record)
		return FALSE;

	obj = json_node_get_object(record);
	JsonNode *participants_node = json_object_get_member(obj, "participants");
	if (!obj)
		return FALSE;
	JsonArray *participants_arr = json_node_get_array(participants_node);
	int i, len = json_array_get_length(participants_arr);

	gboolean ret = TRUE;

	ChimeCallParticipant *presenter = NULL;
	for (i = 0; i < len; i++) {
		JsonNode *p = json_array_get_element(participants_arr, i);
		if (!parse_participant(cxn, call, p, &presenter))
			ret = FALSE;
	}
	if (call->presenter != presenter) {
		call->presenter = presenter;
		g_signal_emit(call, signals[NEW_PRESENTER], 0, presenter);
	}

	g_signal_emit(call, signals[PARTICIPANTS_CHANGED], 0, call->participants);

	return ret;
}

gboolean chime_call_participant_audio_stats(ChimeCall *call, const gchar *participant_id,
					    int vol, int signal_strength)
{
	g_return_val_if_fail(CHIME_IS_CALL(call), FALSE);
	g_return_val_if_fail(participant_id != NULL, FALSE);

	ChimeCallParticipant *p = g_hash_table_lookup(call->participants, participant_id);
	if (!p)
		return FALSE;

	if (vol != p->volume || signal_strength != p->signal_strength) {
		p->volume = vol;
		p->signal_strength = signal_strength;
		return TRUE;
	}

	return FALSE;
}

void chime_call_emit_participants(ChimeCall *call)
{
	g_signal_emit(call, signals[PARTICIPANTS_CHANGED], 0, call->participants);
}

static gboolean call_jugg_cb(ChimeConnection *cxn, gpointer _unused, JsonNode *data_node)
{
	JsonObject *obj = json_node_get_object(data_node);
	JsonNode *record = json_object_get_member(obj, "record");
	if (!record)
		return FALSE;

	ChimeCall *call = chime_connection_parse_call(cxn, record, NULL);
	if (call)
		g_object_unref(call);

	return !!call;
}


ChimeCall *chime_connection_parse_call(ChimeConnection *cxn, JsonNode *node,
				       GError **error)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE(cxn);
	const gchar *uuid, *alert_body;
	CHIME_PROPS_PARSE_VARS

	if (!parse_string(node, "uuid", &uuid) ||
	    !parse_string(node, "alert_body", &alert_body) ||
	    CHIME_PROPS_PARSE) {
		g_set_error(error, CHIME_ERROR,
			    CHIME_ERROR_BAD_RESPONSE,
			    _("Failed to parse Call node"));
		return NULL;
	}

	ChimeCall *call = g_hash_table_lookup(priv->calls.by_id, uuid);
	if (!call) {
		call = g_object_new(CHIME_TYPE_CALL,
				       "id", uuid,
				       "name", alert_body,
				       CHIME_PROPS_NEWOBJ
				       NULL);

		g_object_ref(call);
		chime_object_collection_hash_object(&priv->calls, CHIME_OBJECT(call), FALSE);

		return call;
	}

	if (alert_body && g_strcmp0(alert_body, chime_call_get_alert_body(call))) {
		chime_object_rename(CHIME_OBJECT(call), alert_body);
		g_object_notify(G_OBJECT(call), "name");
	}

	CHIME_PROPS_UPDATE

	return g_object_ref(call);
}

void chime_init_calls(ChimeConnection *cxn)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	chime_object_collection_init(cxn, &priv->calls);
}

void chime_destroy_calls(ChimeConnection *cxn)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	if (priv->calls.by_id)
		g_hash_table_foreach(priv->calls.by_id, unsub_call, NULL);

	chime_object_collection_destroy(&priv->calls);
}

static void unsub_call(gpointer key, gpointer val, gpointer data)
{
	ChimeCall *call = CHIME_CALL (val);

	ChimeConnection *cxn = chime_object_get_connection(CHIME_OBJECT(call));
	chime_jugg_unsubscribe(cxn, call->channel, "Call", call_jugg_cb, NULL);
	chime_jugg_unsubscribe(cxn, call->roster_channel, "Roster", call_roster_cb, call);

	if (call->audio) {
		chime_call_audio_close(call->audio, TRUE);
		call->audio = NULL;
	}
	if (call->screen) {
		chime_call_screen_close(call->screen);
		call->screen = NULL;
	}
}

void chime_connection_close_call(ChimeConnection *cxn, ChimeCall *call)
{
	g_return_if_fail(CHIME_IS_CONNECTION(cxn));
	g_return_if_fail(CHIME_IS_CALL(call));
	g_return_if_fail(call->opens);

	if (!--call->opens)
		unsub_call(NULL, call, NULL);
}



void chime_connection_open_call(ChimeConnection *cxn, ChimeCall *call, gboolean muted)
{
	g_return_if_fail(CHIME_IS_CONNECTION(cxn));
	g_return_if_fail(CHIME_IS_CALL(call));

	if (!call->opens++) {
		call->presenter = NULL;
		chime_jugg_subscribe(cxn, call->channel, "Call", call_jugg_cb, NULL);
		chime_jugg_subscribe(cxn, call->roster_channel, "Roster", call_roster_cb, call);
		call->audio = chime_call_audio_open(cxn, call, muted);
	}
}

void chime_call_set_silent(ChimeCall *call, gboolean silent)
{
	if (call->audio)
		chime_call_audio_reopen(call->audio, silent);
}

gboolean chime_call_get_silent(ChimeCall *call)
{
	if (call->audio)
		return chime_call_audio_get_silent(call->audio);
	else
		return TRUE;
}

void chime_call_set_local_mute(ChimeCall *call, gboolean muted)
{
	if (call->audio)
		chime_call_audio_local_mute(call->audio, muted);
}

void chime_call_audio_set_state(ChimeCallAudio *audio, ChimeAudioState state, const gchar *message)
{
	chime_debug("Audio state %d (was %d), msg %s\n", state, audio->state, message);

	if (audio->state == state)
		return;

	audio->state = state;
	g_signal_emit(audio->call, signals[AUDIO_STATE], 0, state, message);
}

void chime_call_screen_set_state(ChimeCallScreen *screen, ChimeScreenState state, const gchar *message)
{
	chime_debug("Screen state %d (was %d), msg %s\n", state, screen->state, message);

	if (screen->state == state)
		return;

	screen->state = state;
	g_signal_emit(screen->call, signals[SCREEN_STATE], 0, state, message);
}

void chime_call_install_gst_app_callbacks(ChimeCall *call, GstAppSrc *appsrc, GstAppSink *appsink)
{
	if (call->audio)
		chime_call_audio_install_gst_app_callbacks(call->audio, appsrc, appsink);
}

void chime_call_view_screen(ChimeConnection *cxn, ChimeCall *call, GstAppSrc *appsrc)
{
	call->screen = chime_call_screen_open(cxn, call, call->screen);

	chime_call_screen_install_appsrc(call->screen, appsrc);
}

void chime_call_send_screen(ChimeConnection *cxn, ChimeCall *call, GstAppSink *appsink)
{
	call->screen = chime_call_screen_open(cxn, call, call->screen);

	chime_call_screen_install_appsink(call->screen, appsink);
}
