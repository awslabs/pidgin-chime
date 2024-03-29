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

#ifndef __CHIME_CONNECTION_H__
#define __CHIME_CONNECTION_H__

#include <glib-object.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define CHIME_TYPE_CONNECTION (chime_connection_get_type ())
G_DECLARE_FINAL_TYPE (ChimeConnection, chime_connection, CHIME, CONNECTION, GObject)

struct _ChimeConnection {
	GObject parent_instance;
};

#define CHIME_ERROR (chime_error_quark())
GQuark chime_error_quark (void);

enum {
	CHIME_ERROR_REQUEST_FAILED = 1,
	CHIME_ERROR_BAD_RESPONSE,
	CHIME_ERROR_AUTH_FAILED,
	CHIME_ERROR_NETWORK,
};

/* Shamelessly matching (by name) the Pidgin loglevels. */
typedef enum {
	CHIME_LOGLVL_MISC,
	CHIME_LOGLVL_INFO,
	CHIME_LOGLVL_WARNING,
	CHIME_LOGLVL_ERROR,
	CHIME_LOGLVL_FATAL
} ChimeLogLevel;

typedef void (*ChimeSoupMessageCallback)(ChimeConnection *cxn,
					 SoupMessage *msg,
					 JsonNode *node,
					 gpointer cb_data);

ChimeConnection *chime_connection_new                        (const gchar *email,
							      const gchar *server,
							      const gchar *device_token,
							      const gchar *session_token);

void             chime_connection_set_presence_async         (ChimeConnection    *self,
                                                              const gchar        *availability,
                                                              const gchar        *visibility,
                                                              GCancellable       *cancellable,
                                                              GAsyncReadyCallback callback,
                                                              gpointer            user_data);

gboolean         chime_connection_set_presence_finish        (ChimeConnection  *self,
                                                              GAsyncResult     *result,
                                                              GError          **error);

void             chime_connection_set_device_status_async    (ChimeConnection    *self,
                                                              const gchar        *status,
                                                              GCancellable       *cancellable,
                                                              GAsyncReadyCallback callback,
                                                              gpointer            user_data);

gboolean         chime_connection_set_device_status_finish   (ChimeConnection  *self,
                                                              GAsyncResult     *result,
                                                              GError          **error);

const gchar     *chime_connection_get_session_token          (ChimeConnection  *self);

void             chime_connection_set_session_token          (ChimeConnection  *self,
                                                              const gchar      *sess_tok);


void chime_connection_signin (ChimeConnection *self);
void chime_connection_authenticate (ChimeConnection *self,
				    const gchar *username,
				    const gchar *password);
void chime_connection_log_out_async (ChimeConnection    *self,
				     GCancellable       *cancellable,
				     GAsyncReadyCallback callback,
				     gpointer            user_data);
gboolean chime_connection_log_out_finish (ChimeConnection  *self,
					  GAsyncResult     *result,
					  GError          **error);

const gchar *chime_connection_get_profile_id(ChimeConnection *self);
const gchar *chime_connection_get_display_name(ChimeConnection *self);
const gchar *chime_connection_get_email(ChimeConnection *self);
void chime_connection_connect(ChimeConnection *cxn);
void chime_connection_disconnect(ChimeConnection *cxn);

/* XXX: Expose something other than a JsonNode for messages? */
gboolean parse_int(JsonNode *node, const gchar *member, gint64 *val);
gboolean parse_string(JsonNode *parent, const gchar *name, const gchar **res);
gboolean parse_boolean(JsonNode *node, const gchar *member, gboolean *val);
gboolean iso8601_to_ms(const gchar *str, gint64 *ms);

G_END_DECLS

#endif /* __CHIME_CONNECTION_H__ */
