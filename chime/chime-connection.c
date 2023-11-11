/*
 * Pidgin/libpurple Chime client plugin
 *
 * Copyright © 2017 Amazon.com, Inc. or its affiliates.
 *
 * Authors: David Woodhouse <dwmw2@infradead.org>
 *          Ignacio Casal Quinteiro <qignacio@amazon.com>
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

#include "chime-connection.h"
#include "chime-connection-private.h"

#include <glib/gi18n.h>

#define SIGNIN_DEFAULT "https://signin.id.ue1.app.chime.aws/"

enum
{
    PROP_0,
    PROP_SESSION_TOKEN,
    PROP_DEVICE_TOKEN,
    PROP_SERVER,
    PROP_ACCOUNT_EMAIL,
    LAST_PROP
};

static GParamSpec *props[LAST_PROP];

enum {
	AUTHENTICATE,
	CONNECTED,
	DISCONNECTED,
	NEW_CONTACT,
	NEW_ROOM,
	ROOM_MENTION,
	NEW_CONVERSATION,
	NEW_MEETING,
	LOG_MESSAGE,
	PROGRESS,
	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

G_DEFINE_QUARK(chime-error-quark, chime_error)
G_DEFINE_TYPE_WITH_PRIVATE(ChimeConnection, chime_connection, G_TYPE_OBJECT)

static void soup_msg_cb(SoupSession *soup_sess, SoupMessage *msg, gpointer _cmsg);

ChimeConnectionPrivate *
chime_connection_get_private(ChimeConnection *cxn)
{
	return chime_connection_get_instance_private(cxn);
}

static void
chime_connection_finalize(GObject *object)
{
	ChimeConnection *self = CHIME_CONNECTION(object);
	ChimeConnectionPrivate *priv = chime_connection_get_private(self);

	g_free(priv->session_token);
	g_free(priv->device_token);
	g_free(priv->server);
	g_free(priv->express_url);

	chime_connection_log(self, CHIME_LOGLVL_MISC, "Connection finalized: %p\n", self);

	G_OBJECT_CLASS(chime_connection_parent_class)->finalize(object);
}

static void
cmsg_free(struct chime_msg *cmsg)
{
	g_object_unref(cmsg->msg);
	g_free(cmsg);
}

void
chime_connection_disconnect(ChimeConnection    *self)
{
	ChimeConnectionPrivate *priv = chime_connection_get_private (self);

	chime_connection_log(self, CHIME_LOGLVL_MISC, "Disconnecting connection: %p\n", self);

	if (priv->soup_sess) {
		soup_session_abort(priv->soup_sess);
		g_clear_object(&priv->soup_sess);
	}

	chime_destroy_meetings(self);
	chime_destroy_calls(self);
	chime_destroy_rooms(self);
	chime_destroy_conversations(self);
	chime_destroy_contacts(self);
	chime_destroy_juggernaut(self);

	g_clear_pointer(&priv->reg_node, json_node_unref);

	if (priv->msgs_pending_auth) {
		g_queue_free_full(priv->msgs_pending_auth, (GDestroyNotify)cmsg_free);
		priv->msgs_pending_auth = NULL;
	}
	if (priv->msgs_queued) {
		g_queue_free(priv->msgs_queued);
		priv->msgs_queued = NULL;
	}

	if (priv->state != CHIME_STATE_DISCONNECTED)
		g_signal_emit(self, signals[DISCONNECTED], 0, NULL);
	priv->state = CHIME_STATE_DISCONNECTED;
}

static void
chime_connection_dispose(GObject *object)
{
	ChimeConnection *self = CHIME_CONNECTION(object);
	ChimeConnectionPrivate *priv = chime_connection_get_private (self);

	if (priv->state != CHIME_STATE_DISCONNECTED)
		chime_connection_disconnect(self);

	g_slist_free_full(priv->amazon_cas, g_object_unref);
	priv->amazon_cas = NULL;
	chime_connection_log(self, CHIME_LOGLVL_MISC, "Connection disposed: %p\n", self);

	G_OBJECT_CLASS(chime_connection_parent_class)->dispose(object);
}

static void
chime_connection_get_property(GObject    *object,
                              guint       prop_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
	ChimeConnection *self = CHIME_CONNECTION(object);
	ChimeConnectionPrivate *priv = chime_connection_get_private (self);

	switch (prop_id) {
	case PROP_SESSION_TOKEN:
		g_value_set_string(value, priv->session_token);
		break;
	case PROP_DEVICE_TOKEN:
		g_value_set_string(value, priv->device_token);
		break;
	case PROP_SERVER:
		g_value_set_string(value, priv->server);
		break;
	case PROP_ACCOUNT_EMAIL:
		g_value_set_string(value, priv->account_email);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void
chime_connection_set_property(GObject      *object,
                              guint         prop_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
	ChimeConnection *self = CHIME_CONNECTION(object);
	ChimeConnectionPrivate *priv = chime_connection_get_private(self);

	switch (prop_id) {
	case PROP_SESSION_TOKEN:
		priv->session_token = g_value_dup_string(value);
		break;
	case PROP_DEVICE_TOKEN:
		priv->device_token = g_value_dup_string(value);
		break;
	case PROP_SERVER:
		priv->server = g_value_dup_string(value);
		break;
	case PROP_ACCOUNT_EMAIL:
		priv->account_email = g_value_dup_string(value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void
chime_connection_class_init(ChimeConnectionClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->finalize = chime_connection_finalize;
	object_class->dispose = chime_connection_dispose;
	object_class->get_property = chime_connection_get_property;
	object_class->set_property = chime_connection_set_property;

	props[PROP_SESSION_TOKEN] =
		g_param_spec_string("session-token",
				    "session token",
				    "session token",
				    NULL,
				    G_PARAM_READWRITE |
				    G_PARAM_CONSTRUCT |
				    G_PARAM_STATIC_STRINGS);

	props[PROP_DEVICE_TOKEN] =
		g_param_spec_string("device-token",
				    "device token",
				    "device token",
				    NULL,
				    G_PARAM_READWRITE |
				    G_PARAM_CONSTRUCT_ONLY |
				    G_PARAM_STATIC_STRINGS);

	props[PROP_SERVER] =
		g_param_spec_string("server",
				    "server",
				    "server",
				    SIGNIN_DEFAULT,
				    G_PARAM_READWRITE |
				    G_PARAM_CONSTRUCT_ONLY |
				    G_PARAM_STATIC_STRINGS);

	props[PROP_ACCOUNT_EMAIL] =
		g_param_spec_string("account-email",
				    "account e-mail",
				    "account e-mail",
				    NULL,
				    G_PARAM_READWRITE |
				    G_PARAM_CONSTRUCT_ONLY |
				    G_PARAM_STATIC_STRINGS);

	g_object_class_install_properties(object_class, LAST_PROP, props);

	signals[AUTHENTICATE] =
		g_signal_new ("authenticate",
			      G_OBJECT_CLASS_TYPE (object_class), G_SIGNAL_RUN_FIRST,
			      0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);

	signals[CONNECTED] =
		g_signal_new ("connected",
			      G_OBJECT_CLASS_TYPE (object_class), G_SIGNAL_RUN_FIRST,
			      0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);

	signals[DISCONNECTED] =
		g_signal_new ("disconnected",
			      G_OBJECT_CLASS_TYPE (object_class), G_SIGNAL_RUN_FIRST,
			      0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_ERROR);

	signals[NEW_CONTACT] =
		g_signal_new ("new-contact",
			      G_OBJECT_CLASS_TYPE (object_class), G_SIGNAL_RUN_FIRST,
			      0, NULL, NULL, NULL, G_TYPE_NONE, 1, CHIME_TYPE_CONTACT);

	signals[NEW_ROOM] =
		g_signal_new ("new-room",
			      G_OBJECT_CLASS_TYPE (object_class), G_SIGNAL_RUN_FIRST,
			      0, NULL, NULL, NULL, G_TYPE_NONE, 1, CHIME_TYPE_ROOM);

	signals[ROOM_MENTION] =
		g_signal_new ("room-mention",
			      G_OBJECT_CLASS_TYPE (object_class), G_SIGNAL_RUN_FIRST,
			      0, NULL, NULL, NULL, G_TYPE_NONE, 2, CHIME_TYPE_ROOM, JSON_TYPE_NODE);

	signals[NEW_CONVERSATION] =
		g_signal_new ("new-conversation",
			      G_OBJECT_CLASS_TYPE (object_class), G_SIGNAL_RUN_FIRST,
			      0, NULL, NULL, NULL, G_TYPE_NONE, 1, CHIME_TYPE_CONVERSATION);

	signals[NEW_MEETING] =
		g_signal_new ("new-meeting",
			      G_OBJECT_CLASS_TYPE (object_class), G_SIGNAL_RUN_FIRST,
			      0, NULL, NULL, NULL, G_TYPE_NONE, 1, CHIME_TYPE_MEETING);

	signals[LOG_MESSAGE] =
		g_signal_new ("log-message",
			      G_OBJECT_CLASS_TYPE (object_class), G_SIGNAL_RUN_FIRST,
			      0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_STRING);

	signals[PROGRESS] =
		g_signal_new ("progress",
			      G_OBJECT_CLASS_TYPE (object_class), G_SIGNAL_RUN_FIRST,
			      0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_STRING);
}

void chime_connection_fail_error(ChimeConnection *cxn, GError *error)
{
	ChimeConnectionPrivate *priv = chime_connection_get_private (cxn);

	priv->state = CHIME_STATE_DISCONNECTED;
	g_signal_emit(cxn, signals[DISCONNECTED], 0, error);

	/* Finish the cleanup */
	chime_connection_disconnect(cxn);
}

void chime_connection_fail(ChimeConnection *cxn, gint code, const gchar *format, ...)
{
	GError *error;
	va_list args;

	va_start(args, format);
	error = g_error_new_valist(CHIME_ERROR, code, format, args);
	va_end(args);

	chime_connection_fail_error(cxn, error);
	g_error_free(error);
}

static void
req_started_cb(SoupSession *sess, SoupMessage *msg, SoupSocket *sock, gpointer _cxn)
{
	ChimeConnection *cxn = CHIME_CONNECTION(_cxn);
	ChimeConnectionPrivate *priv = chime_connection_get_private (cxn);

	if (!soup_socket_is_ssl(sock))
		return;

	GTlsCertificateFlags cert_errors;
	g_object_get(sock, "tls-errors", &cert_errors, NULL);
	if (!cert_errors)
		return;

	/* If the problem was *only* an unknown CA (i.e. the hostname did
	 * match OK, it wasn't expired, etc.) then check if it's trusted
	 * by the Amazon internal CA. */
	if (cert_errors == G_TLS_CERTIFICATE_UNKNOWN_CA) {
		/* The identity part shouldn't be needed but there's no
		 * real harm in being paranoid and checking it again. */
		SoupURI *uri = soup_message_get_uri(msg);
		GSocketConnectable *ident = g_network_address_new(soup_uri_get_host(uri),
								  soup_uri_get_port(uri));

		GTlsCertificate *cert;
		g_object_get(sock, "tls-certificate", &cert, NULL);

		GSList *l = priv->amazon_cas;
		while (l && cert_errors) {
			cert_errors = g_tls_certificate_verify(cert, ident, G_TLS_CERTIFICATE(l->data));
			l = l->next;
		}
		g_object_unref(ident);

		if (!cert_errors) {
			chime_debug("Allow Amazon CA for %s\n", soup_uri_get_host(uri));
			return;
		}
	}

	/* Don't like the server's cert. Fail the message. */
	soup_session_cancel_message(sess, msg, SOUP_STATUS_SSL_FAILED);
}

static void
chime_connection_init(ChimeConnection *self)
{
	ChimeConnectionPrivate *priv = chime_connection_get_private (self);
	priv->soup_sess = soup_session_new();
	priv->amazon_cas = chime_cert_list();

	if (getenv("CHIME_DEBUG") && atoi(getenv("CHIME_DEBUG")) > 0) {
		SoupLogger *l = soup_logger_new(SOUP_LOGGER_LOG_BODY, -1);
		soup_session_add_feature(priv->soup_sess, SOUP_SESSION_FEATURE(l));
		g_object_unref(l);
	}

	const gchar *https_aliases[2] = { "wss", NULL };
	g_object_set(priv->soup_sess, "https-aliases", https_aliases, NULL);

	/* Unset ssl-strict and manually check, so that we can allow
	 * the Amazon internal CAs. The media endpoints may use those. */
	g_object_set(priv->soup_sess, "ssl-strict", FALSE, NULL);
	g_signal_connect(G_OBJECT(priv->soup_sess), "request-started", G_CALLBACK(req_started_cb), self);

	priv->msgs_pending_auth = g_queue_new();
	priv->msgs_queued = g_queue_new();
	priv->state = CHIME_STATE_DISCONNECTED;
}

ChimeConnection *
chime_connection_new(const gchar *email, const gchar *server,
		     const gchar *device_token, const gchar *session_token)
{
	if (!server || !*server)
		server = SIGNIN_DEFAULT;

	return g_object_new (CHIME_TYPE_CONNECTION,
			     "account-email", email,
			     "server", server,
			     "device-token", device_token,
			     "session-token", session_token,
	                     NULL);
}

static gboolean parse_regnode(ChimeConnection *self, JsonNode *regnode)
{
	ChimeConnectionPrivate *priv = chime_connection_get_private (self);
	JsonObject *obj = json_node_get_object(priv->reg_node);
	JsonNode *node, *sess_node = json_object_get_member(obj, "Session");
	const gchar *sess_tok;
	char *express_url, *p;

	if (!sess_node)
		return FALSE;
	if (!parse_string(sess_node, "SessionToken", &sess_tok))
		return FALSE;

	chime_connection_set_session_token(self, sess_tok);

	if (!parse_string(sess_node, "SessionId", &priv->session_id))
		return FALSE;

	obj = json_node_get_object(sess_node);

	node = json_object_get_member(obj, "Profile");
	if (!parse_string(node, "profile_channel", &priv->profile_channel) ||
	    !parse_string(node, "presence_channel", &priv->presence_channel) ||
	    !parse_string(node, "id", &priv->profile_id) ||
	    !parse_string(node, "display_name", &priv->display_name) ||
	    !parse_string(node, "email", &priv->email))
		return FALSE;

	node = json_object_get_member(obj, "Device");
	if (!parse_string(node, "DeviceId", &priv->device_id) ||
	    !parse_string(node, "Channel", &priv->device_channel))
		return FALSE;

	node = json_object_get_member(obj, "ServiceConfig");
	if (!node)
		return FALSE;
	obj = json_node_get_object(node);

	node = json_object_get_member(obj, "Presence");
	if (!parse_string(node, "RestUrl", &priv->presence_url))
		return FALSE;

	node = json_object_get_member(obj, "Push");
	if (!parse_string(node, "ReachabilityUrl", &priv->reachability_url) ||
	    !parse_string(node, "WebsocketUrl", &priv->websocket_url))
		return FALSE;

	node = json_object_get_member(obj, "Profile");
	if (!parse_string(node, "RestUrl", &priv->profile_url))
		return FALSE;

	node = json_object_get_member(obj, "Contacts");
	if (!parse_string(node, "RestUrl", &priv->contacts_url))
		return FALSE;

	node = json_object_get_member(obj, "Messaging");
	if (!parse_string(node, "RestUrl", &priv->messaging_url))
		return FALSE;

	node = json_object_get_member(obj, "Presence");
	if (!parse_string(node, "RestUrl", &priv->presence_url))
		return FALSE;

	node = json_object_get_member(obj, "Conference");
	if (!parse_string(node, "RestUrl", &priv->conference_url))
		return FALSE;

	node = json_object_get_member(obj, "Feature");
	if (!parse_string(node, "RestUrl", &priv->feature_url))
		return FALSE;

	/* Ick. How are we supposed to discover this? */
	express_url = g_strdup(priv->feature_url);
        p = strstr(express_url, "feature");
	if (!p) {
		g_free(express_url);
		return FALSE;
	}
	memcpy(p, "express", 7);
	priv->express_url = express_url;

	return TRUE;
}

static JsonNode *
chime_device_register_req(const gchar *devtoken)
{
	JsonBuilder *jb;
	JsonNode *jn;

	jb = json_builder_new();
	jb = json_builder_begin_object(jb);
	jb = json_builder_set_member_name(jb, "Device");
	jb = json_builder_begin_object(jb);
	jb = json_builder_set_member_name(jb, "Platform");
	jb = json_builder_add_string_value(jb, "pidgin");
	jb = json_builder_set_member_name(jb, "DeviceToken");
	jb = json_builder_add_string_value(jb, devtoken);
	jb = json_builder_set_member_name(jb, "PlatformDeviceId");
	jb = json_builder_add_string_value(jb, devtoken);
	jb = json_builder_set_member_name(jb, "Capabilities");
	jb = json_builder_add_int_value(jb, CHIME_DEVICE_CAP_PUSH_DELIVERY_RECEIPTS |
					CHIME_DEVICE_CAP_PRESENCE_PUSH |
					CHIME_DEVICE_CAP_PRESENCE_SUBSCRIPTION);
	jb = json_builder_end_object(jb);
	jb = json_builder_end_object(jb);
	jn = json_builder_get_root(jb);
	g_object_unref(jb);

	return jn;
}

static void register_cb(ChimeConnection *self, SoupMessage *msg,
			JsonNode *node, gpointer user_data)
{
	ChimeConnectionPrivate *priv = chime_connection_get_private (self);

	if (!node) {
		chime_connection_fail(self, CHIME_ERROR_NETWORK,
				      _("Device registration failed"));
		return;
	}

	priv->reg_node = json_node_ref(node);
	if (!parse_regnode(self, priv->reg_node)) {
		chime_connection_fail(self, CHIME_ERROR_BAD_RESPONSE,
				      _("Failed to process registration response"));
		return;
	}

	chime_init_juggernaut(self);

	chime_jugg_subscribe(self, priv->profile_channel, NULL, NULL, NULL);
	chime_jugg_subscribe(self, priv->presence_channel, NULL, NULL, NULL);
	chime_jugg_subscribe(self, priv->device_channel, NULL, NULL, NULL);

	chime_init_contacts(self);
	chime_init_rooms(self);
	chime_init_conversations(self);
	chime_init_calls(self);
	chime_init_meetings(self);
}

void chime_connection_calculate_online(ChimeConnection *self)
{
	ChimeConnectionPrivate *priv = chime_connection_get_private (self);

	if (priv->contacts_online && priv->rooms_online &&
	    priv->convs_online && priv->jugg_online && priv->meetings_online) {
		g_signal_emit (self, signals[CONNECTED], 0, priv->display_name);
		priv->state = CHIME_STATE_CONNECTED;
	}
}

void
chime_connection_connect(ChimeConnection    *self)
{
	ChimeConnectionPrivate *priv = chime_connection_get_private (self);

	if (priv->state != CHIME_STATE_DISCONNECTED)
		return;

	priv->state = CHIME_STATE_CONNECTING;

	if (!priv->session_token || !*priv->session_token) {
		priv->state = CHIME_STATE_DISCONNECTED;
		chime_connection_fail(self, CHIME_ERROR_AUTH_FAILED,
				      _("Please authenticate in web browser"));
		g_signal_emit(self, signals[AUTHENTICATE], 0, priv->server);
		return;
	}

	JsonNode *node = chime_device_register_req(priv->device_token);

	SoupURI *uri = soup_uri_new_printf(priv->server, "/sessions");
	soup_uri_set_query_from_fields(uri, "Token", priv->session_token, NULL);

	chime_connection_queue_http_request(self, node, uri, "POST", register_cb, NULL);

	json_node_unref(node);
}

static void set_device_status_cb(ChimeConnection *self, SoupMessage *msg,
				 JsonNode *node, gpointer user_data)
{
	GTask *task = G_TASK(user_data);

	g_task_return_boolean(task, TRUE);
	g_object_unref(task);
}

void
chime_connection_set_device_status_async(ChimeConnection    *self,
					 const gchar        *status,
					 GCancellable       *cancellable,
					 GAsyncReadyCallback callback,
					 gpointer            user_data)
{
	g_return_if_fail(CHIME_IS_CONNECTION(self));
	ChimeConnectionPrivate *priv = chime_connection_get_private (self);

	GTask *task = g_task_new(self, cancellable, callback, user_data);
	JsonBuilder *builder = json_builder_new();
	builder = json_builder_begin_object(builder);
	builder = json_builder_set_member_name(builder, "Status");
	builder = json_builder_add_string_value(builder, status);
	builder = json_builder_end_object(builder);
	JsonNode *node = json_builder_get_root(builder);

	SoupURI *uri = soup_uri_new_printf(priv->presence_url, "/devicestatus");
	chime_connection_queue_http_request(self, node, uri, "PUT", set_device_status_cb, task);

	json_node_unref(node);
	g_object_unref(builder);
}

gboolean
chime_connection_set_device_status_finish(ChimeConnection  *self,
					  GAsyncResult     *result,
					  GError          **error)
{
	g_return_val_if_fail(CHIME_IS_CONNECTION(self), FALSE);
	g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

	return g_task_propagate_boolean(G_TASK(result), error);
}

static void set_presence_cb(ChimeConnection *self, SoupMessage *msg,
			    JsonNode *node, gpointer user_data)
{
	GTask *task = G_TASK(user_data);

	g_task_return_boolean(task, TRUE);
	g_object_unref(task);
}

void
chime_connection_set_presence_async(ChimeConnection    *self,
				    const gchar        *availability,
				    const gchar        *visibility,
				    GCancellable       *cancellable,
				    GAsyncReadyCallback callback,
				    gpointer            user_data)
{
	g_return_if_fail(CHIME_IS_CONNECTION(self));
	ChimeConnectionPrivate *priv = chime_connection_get_private (self);

	GTask *task = g_task_new(self, cancellable, callback, user_data);
	JsonBuilder *builder = json_builder_new();
	builder = json_builder_begin_object(builder);
	if (availability) {
		builder = json_builder_set_member_name(builder, "ManualAvailability");
		builder = json_builder_add_string_value(builder, availability);
	}
	if (visibility) {
		builder = json_builder_set_member_name(builder, "PresenceVisibility");
		builder = json_builder_add_string_value(builder, visibility);
	}
	builder = json_builder_end_object(builder);
	JsonNode *node = json_builder_get_root(builder);

	SoupURI *uri = soup_uri_new_printf(priv->presence_url, "/presencesettings");
	chime_connection_queue_http_request(self, node, uri, "POST", set_presence_cb, task);

	json_node_unref(node);
	g_object_unref(builder);
}

gboolean
chime_connection_set_presence_finish(ChimeConnection  *self,
				     GAsyncResult     *result,
				     GError          **error)
{
	g_return_val_if_fail(CHIME_IS_CONNECTION(self), FALSE);
	g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

	return g_task_propagate_boolean(G_TASK(result), error);
}

const gchar *
chime_connection_get_session_token(ChimeConnection *self)
{
	ChimeConnectionPrivate *priv = chime_connection_get_private (self);
	g_return_val_if_fail(CHIME_IS_CONNECTION(self), NULL);

	return priv->session_token;
}

void
chime_connection_set_session_token(ChimeConnection *self,
				   const gchar *sess_tok)
{
	ChimeConnectionPrivate *priv = chime_connection_get_private (self);
	g_return_if_fail(CHIME_IS_CONNECTION(self));

	if (g_strcmp0(priv->session_token, sess_tok)) {
		g_free(priv->session_token);
		priv->session_token = g_strdup(sess_tok);
		g_object_notify_by_pspec(G_OBJECT(self), props[PROP_SESSION_TOKEN]);
	}
}

/* If we get an auth failure on a standard request, we automatically attempt
 * to renew the authentication token and resubmit the request. */
static void renew_cb(ChimeConnection *self, SoupMessage *msg,
		     JsonNode *node, gpointer _unused)
{
	ChimeConnectionPrivate *priv = chime_connection_get_private (self);
	struct chime_msg *cmsg = NULL;
	const gchar *sess_tok;
	gchar *cookie_hdr;

	if (!node || !parse_string(node, "SessionToken", &sess_tok)) {
		chime_connection_fail(self, CHIME_ERROR_NETWORK,
				      _("Failed to renew session token"));
		chime_connection_set_session_token(self, NULL);
		return;
	}

	chime_connection_set_session_token(self, sess_tok);

	if (priv->state == CHIME_STATE_DISCONNECTED)
		return;

	cookie_hdr = g_strdup_printf("_aws_wt_session=%s", priv->session_token);

	while ( (cmsg = g_queue_pop_head(priv->msgs_pending_auth)) ) {
		soup_message_headers_replace(cmsg->msg->request_headers,
					     "Cookie", cookie_hdr);
		soup_message_headers_replace(cmsg->msg->request_headers,
					     "X-Chime-Auth-Token", cookie_hdr);
		chime_connection_log(self, CHIME_LOGLVL_MISC, "Requeued %p to %s\n", cmsg->msg,
				     soup_uri_get_path(soup_message_get_uri(cmsg->msg)));
		g_object_ref(self);
		soup_session_queue_message(priv->soup_sess, cmsg->msg,
					   soup_msg_cb, cmsg);
	}

	g_free(cookie_hdr);
}

static void chime_renew_token(ChimeConnection *self)
{
	ChimeConnectionPrivate *priv = chime_connection_get_private (self);
	SoupURI *uri;
	JsonBuilder *builder;
	JsonNode *node;

	builder = json_builder_new();
	builder = json_builder_begin_object(builder);
	builder = json_builder_set_member_name(builder, "Token");
	builder = json_builder_add_string_value(builder, priv->session_token);
	builder = json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	uri = soup_uri_new_printf(priv->profile_url, "/tokens");
	soup_uri_set_query_from_fields(uri, "Token", priv->session_token, NULL);
	chime_connection_queue_http_request(self, node, uri, "POST", renew_cb, NULL);

	json_node_unref(node);
	g_object_unref(builder);
}

/* First callback for SoupMessage completion — do the common
 * parsing of the JSON response (if any) and hand it on to the
 * real callback function. Also handles auth token renewal. */
static void soup_msg_cb(SoupSession *soup_sess, SoupMessage *msg, gpointer _cmsg)
{
	struct chime_msg *cmsg = _cmsg;
	ChimeConnection *cxn = cmsg->cxn;
	ChimeConnectionPrivate *priv = chime_connection_get_private (cxn);
	JsonParser *parser = NULL;
	JsonNode *node = NULL;

	if (priv->msgs_queued)
		g_queue_remove(priv->msgs_queued, cmsg);

	/* Special case for renew_cb itself, which mustn't recurse! */
	if (priv->state != CHIME_STATE_DISCONNECTED &&
	    cmsg->cb != renew_cb && cmsg->cb != register_cb &&
	    (msg->status_code == 401 /*||
	     (msg->status_code == 7 && !g_queue_is_empty(priv->msgs_pending_auth))*/)) {
		g_object_ref(msg);
		gboolean already_renewing = !g_queue_is_empty(priv->msgs_pending_auth);
		g_queue_push_tail(priv->msgs_pending_auth, cmsg);
		if (!already_renewing) {
#if 0 /* Not working; we can catch statue_code==7 above but it's also breaking
	 the websocket connection too. */
			while (!g_queue_is_empty(priv->msgs_queued)) {
				cmsg = g_queue_pop_head(priv->msgs_queued);
				soup_session_cancel_message(priv->soup_sess, cmsg->msg, 401);
				// They should requeue themselves
			}
#endif
			chime_renew_token(cxn);
		}
		g_object_unref(cxn);
		return;
	}

	const gchar *content_type = soup_message_headers_get_content_type(msg->response_headers, NULL);
	if (!g_strcmp0(content_type, "application/json") && msg->response_body->data) {
		GError *error = NULL;

		parser = json_parser_new();
		if (!json_parser_load_from_data(parser, msg->response_body->data, msg->response_body->length, &error)) {
			g_warning("Error loading data: %s", error->message);
			g_error_free(error);
		} else {
			node = json_parser_get_root(parser);
		}
	}

	if (cmsg->cb)
		cmsg->cb(cmsg->cxn, msg, node, cmsg->cb_data);
	g_clear_object(&parser);
	g_free(cmsg);
	g_object_unref(cxn);
}

SoupMessage *
chime_connection_queue_http_request(ChimeConnection *self, JsonNode *node,
				    SoupURI *uri, const gchar *method,
				    ChimeSoupMessageCallback callback,
				    gpointer cb_data)
{
	g_return_val_if_fail(CHIME_IS_CONNECTION(self), NULL);
	g_return_val_if_fail(SOUP_URI_IS_VALID(uri), NULL);

	ChimeConnectionPrivate *priv = chime_connection_get_private (self);
	struct chime_msg *cmsg = g_new0(struct chime_msg, 1);

	cmsg->cxn = self;
	cmsg->cb = callback;
	cmsg->cb_data = cb_data;
	cmsg->msg = soup_message_new_from_uri(method, uri);
	soup_uri_free(uri);

	if (priv->session_token) {
		gchar *cookie = g_strdup_printf("_aws_wt_session=%s", priv->session_token);
		soup_message_headers_append(cmsg->msg->request_headers, "Cookie", cookie);
		soup_message_headers_append(cmsg->msg->request_headers, "X-Chime-Auth-Token", cookie);
		g_free(cookie);
	}

	soup_message_headers_append(cmsg->msg->request_headers, "Accept", "*/*");
	soup_message_headers_append(cmsg->msg->request_headers, "User-Agent", "Pidgin-Chime " PACKAGE_VERSION);
	if (node) {
		gchar *body;
		gsize body_size;
		JsonGenerator *gen = json_generator_new();
		json_generator_set_root(gen, node);
		body = json_generator_to_data(gen, &body_size);
		soup_message_set_request(cmsg->msg, "application/json",
					 SOUP_MEMORY_TAKE,
					 body, body_size);
		g_object_unref(gen);
	}

	/* If we are already renewing the token, don't bother submitting it with the
	 * old token just for it to fail (and perhaps trigger *another* token reneawl
	 * which isn't even needed. */
	if (cmsg->cb != renew_cb && !g_queue_is_empty(priv->msgs_pending_auth))
		g_queue_push_tail(priv->msgs_pending_auth, cmsg);
	else {
		g_queue_push_tail(priv->msgs_queued, cmsg);
		g_object_ref(self);
		soup_session_queue_message(priv->soup_sess, cmsg->msg, soup_msg_cb, cmsg);
	}

	return cmsg->msg;
}

void chime_connection_new_contact(ChimeConnection *cxn, ChimeContact *contact)
{
	g_signal_emit(cxn, signals[NEW_CONTACT], 0, contact);
}

void chime_connection_new_room(ChimeConnection *cxn, ChimeRoom *room)
{
	g_signal_emit(cxn, signals[NEW_ROOM], 0, room);
}

void chime_connection_new_conversation(ChimeConnection *cxn, ChimeConversation *conversation)
{
	g_signal_emit(cxn, signals[NEW_CONVERSATION], 0, conversation);
}

void chime_connection_new_meeting(ChimeConnection *cxn, ChimeMeeting *meeting)
{
	g_signal_emit(cxn, signals[NEW_MEETING], 0, meeting);
}

void chime_connection_log(ChimeConnection *cxn, ChimeLogLevel level, const gchar *format, ...)
{
	va_list args;
	gchar *str;

	va_start(args, format);
	str = g_strdup_vprintf(format, args);
	va_end(args);
	g_signal_emit(cxn, signals[LOG_MESSAGE], 0, level, str);
	g_free(str);
}

void chime_connection_progress(ChimeConnection *cxn, int percent, const gchar *message)
{
	g_signal_emit(cxn, signals[PROGRESS], 0, percent, message);
}

SoupURI *soup_uri_new_printf(const gchar *base, const gchar *format, ...)
{
	SoupURI *uri;
	va_list args;
	gchar *constructed;
	gchar *append;

	va_start(args, format);
	append = g_strdup_vprintf(format, args);
	va_end(args);
	constructed = g_strdup_printf("%s%s%s", base,
				      g_str_has_suffix(base, "/") ? "" : "/",
				      append[0] == '/' ? append + 1 : append);
	uri = soup_uri_new(constructed);
	g_free(constructed);
	g_free(append);
	return uri;
}

gboolean parse_visibility(JsonNode *node, const gchar *member, gboolean *val)
{
	const gchar *str;

	if (!parse_string(node, member, &str))
		return FALSE;

	if (!strcmp(str, "visible"))
		*val = TRUE;
	else if (!strcmp(str, "hidden"))
		*val = FALSE;
	else
		return FALSE;

	return TRUE;
}

gboolean parse_notify_pref(JsonNode *node, const gchar *member,
			   ChimeNotifyPref *type)
{
	const gchar *str;

	if (!parse_string(node, member, &str))
		return FALSE;

	gpointer klass = g_type_class_ref(CHIME_TYPE_NOTIFY_PREF);
	GEnumValue *val = g_enum_get_value_by_nick(klass, str);
	g_type_class_unref(klass);

	if (!val)
		return FALSE;
	*type = val->value;
	return TRUE;
}

gboolean parse_int(JsonNode *node, const gchar *member, gint64 *val)
{
	node = json_object_get_member(json_node_get_object(node), member);
	if (!node)
		return FALSE;
	*val = json_node_get_int(node);
	return TRUE;
}

gboolean parse_boolean(JsonNode *node, const gchar *member, gboolean *val)
{
	gint64 intval;

	if (!parse_int(node, member, &intval))
		return FALSE;

	*val = !!intval;
	return TRUE;
}


/* Helper function to get a string from a JSON child node */
gboolean parse_string(JsonNode *parent, const gchar *name, const gchar **res)
{
	JsonObject *obj;
	JsonNode *node;
	const gchar *str;

	if (!parent)
		return FALSE;

	obj = json_node_get_object(parent);
	if (!obj)
		return FALSE;

	node = json_object_get_member(obj, name);
	if (!node)
		return FALSE;

	str = json_node_get_string(node);
	if (!str)
		return FALSE;

	*res = str;
	chime_debug("Got %s = %s\n", name, str);
	return TRUE;
}

gboolean iso8601_to_ms(const gchar *str, gint64 *ms)
{
	/* I *believe* this doesn't leak a new one every time! */
	GTimeZone *utc = g_time_zone_new_utc();
	GDateTime *dt;

	dt = g_date_time_new_from_iso8601(str, utc);
	if (!dt)
		return FALSE;

	*ms = (g_date_time_to_unix(dt) * 1000) +
		(g_date_time_get_microsecond(dt) / 1000000);

	g_date_time_unref(dt);
	return TRUE;
}

static void send_message_cb(ChimeConnection *self, SoupMessage *msg,
			    JsonNode *node, gpointer user_data)
{
	GTask *task = G_TASK(user_data);

	/* Nothing to do o nsuccess */
	if (!SOUP_STATUS_IS_SUCCESSFUL(msg->status_code)) {
		const gchar *reason = msg->reason_phrase;

		if (node)
			parse_string(node, "Message", &reason);

		g_task_return_new_error(task, CHIME_ERROR,
					CHIME_ERROR_NETWORK,
					_("Failed to send message: %d %s"),
					msg->status_code, reason);
	} else {
		JsonObject *obj = json_node_get_object(node);
		JsonNode *msg_node = json_object_get_member(obj, "Message");

		if (msg_node) {
			g_signal_emit_by_name(CHIME_OBJECT(g_task_get_task_data(task)), "message", msg_node);
			g_task_return_pointer(task, json_node_ref(msg_node), (GDestroyNotify)json_node_unref);
		} else
			g_task_return_new_error(task, CHIME_ERROR,
						CHIME_ERROR_NETWORK,
						_("Failed to send message"));
	}
	g_object_unref(task);
}

void
chime_connection_send_message_async(ChimeConnection *self,
				    ChimeObject *obj,
				    const gchar *message,
				    GCancellable *cancellable,
				    GAsyncReadyCallback callback,
				    gpointer user_data,
				    JsonObject *additional_json)
{
	g_return_if_fail(CHIME_IS_CONNECTION(self));
	ChimeConnectionPrivate *priv = chime_connection_get_private (self);

	GTask *task = g_task_new(self, cancellable, callback, user_data);
	g_task_set_task_data(task, g_object_ref(obj), g_object_unref);

	/* g_uuid_string_random() not till 2.52. So do this instead... */
	GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256);
	/* Mix the pointer itself into the randomness */
	g_checksum_update(sum, (void *)&message, sizeof(const gchar *));
	g_checksum_update(sum, (void *)message, strlen(message));
	gint64 t = g_get_monotonic_time();
	g_checksum_update(sum, (void *)&t, sizeof(t));
	guint32 r = g_random_int();
	g_checksum_update(sum, (void *)&r, sizeof(r));
	const gchar *uuid = g_checksum_get_string(sum);

	JsonBuilder *jb = json_builder_new();
	jb = json_builder_begin_object(jb);
	jb = json_builder_set_member_name(jb, "Content");
	jb = json_builder_add_string_value(jb, message);
	jb = json_builder_set_member_name(jb, "ClientRequestToken");
	jb = json_builder_add_string_value(jb, uuid);
	jb = json_builder_end_object(jb);

	SoupURI *uri = soup_uri_new_printf(priv->messaging_url, "/%ss/%s/messages",
					   CHIME_IS_ROOM(obj) ? "room" : "conversation",
					   chime_object_get_id(obj));
	JsonNode *node = json_builder_get_root(jb);
	JsonObject *json_obj = json_node_get_object(node);
	if (additional_json) {
		GList *members_head = json_object_get_members(additional_json);
		GList *member = members_head;
		while (member != NULL) {
			gchar *member_name = (gchar*)member->data;
			JsonNode *member_node = json_object_dup_member(additional_json, member_name);
			json_object_set_member(json_obj, member_name, member_node);
			member = member->next;
		}
		g_list_free(members_head);
	}
	chime_connection_queue_http_request(self, node, uri, "POST", send_message_cb, task);

	json_node_unref(node);
	g_object_unref(jb);

	g_checksum_free(sum);
}

JsonNode *
chime_connection_send_message_finish(ChimeConnection *self, GAsyncResult *result,
				     GError **error)
{
	g_return_val_if_fail(CHIME_IS_CONNECTION(self), FALSE);
	g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

	return g_task_propagate_pointer(G_TASK(result), error);
}

const gchar *chime_connection_get_profile_id(ChimeConnection *self)
{
	g_return_val_if_fail(CHIME_IS_CONNECTION(self), NULL);
	ChimeConnectionPrivate *priv = chime_connection_get_private (self);

	return priv->profile_id;
}

const gchar *chime_connection_get_display_name(ChimeConnection *self)
{
	g_return_val_if_fail(CHIME_IS_CONNECTION(self), NULL);
	ChimeConnectionPrivate *priv = chime_connection_get_private (self);

	return priv->display_name;
}

const gchar *chime_connection_get_email(ChimeConnection *self)
{
	g_return_val_if_fail(CHIME_IS_CONNECTION(self), NULL);
	ChimeConnectionPrivate *priv = chime_connection_get_private (self);

	return priv->email;
}

struct fetch_msg_data {
	ChimeObject *obj;
	GHashTable *query;
};
static void free_fetch_msg_data(gpointer _fmd)
{
	struct fetch_msg_data *fmd = _fmd;
	g_hash_table_destroy(fmd->query);
	g_object_unref(fmd->obj);
	g_free(fmd);
}

static void fetch_messages_req(ChimeConnection *self, GTask *task);

static void fetch_messages_cb(ChimeConnection *self, SoupMessage *msg,
			      JsonNode *node, gpointer user_data)
{
	GTask *task = G_TASK(user_data);
	struct fetch_msg_data *fmd = g_task_get_task_data(task);

	if (!SOUP_STATUS_IS_SUCCESSFUL(msg->status_code)) {
		const gchar *reason = msg->reason_phrase;

		if (node)
			parse_string(node, "error", &reason);

		g_task_return_new_error(task, CHIME_ERROR,
					CHIME_ERROR_NETWORK,
					_("Failed to fetch messages: %d %s"),
					msg->status_code, reason);
	} else {
		JsonObject *obj = json_node_get_object(node);
		JsonNode *msgs_node = json_object_get_member(obj, "Messages");
		JsonArray *msgs_array = json_node_get_array(msgs_node);
		guint i, len = json_array_get_length(msgs_array);

		for (i = 0; i < len; i++) {
			JsonNode *msg_node = json_array_get_element(msgs_array, i);
			const gchar *id;
			if (parse_string(msg_node, "MessageId", &id))
				g_signal_emit_by_name(fmd->obj, "message", msg_node);
		}

		const gchar *next_token;
		if (parse_string(node, "NextToken", &next_token)) {
			g_hash_table_insert(fmd->query, (void *)"next-token", g_strdup(next_token));
			fetch_messages_req(self, task);
			return;
		}

		g_task_return_boolean(task, TRUE);
	}
	g_object_unref(task);
}

static void fetch_messages_req(ChimeConnection *self, GTask *task)
{
	ChimeConnectionPrivate *priv = chime_connection_get_private (self);
	struct fetch_msg_data *fmd = g_task_get_task_data(task);

	SoupURI *uri = soup_uri_new_printf(priv->messaging_url, "/%ss/%s/messages",
					   CHIME_IS_ROOM(fmd->obj) ? "room" : "conversation",
					   chime_object_get_id(fmd->obj));
	soup_uri_set_query_from_form(uri, fmd->query);
	chime_connection_queue_http_request(self, NULL, uri, "GET", fetch_messages_cb, task);

}

void chime_connection_fetch_messages_async(ChimeConnection *self,
					   ChimeObject *obj,
					   const gchar *before,
					   const gchar *after,
					   GCancellable *cancellable,
					   GAsyncReadyCallback callback,
					   gpointer user_data)
{
	g_return_if_fail(CHIME_IS_CONNECTION(self));

	GTask *task = g_task_new(self, cancellable, callback, user_data);
	struct fetch_msg_data *fmd = g_new0(struct fetch_msg_data, 1);
	fmd->obj = g_object_ref(obj);
	fmd->query = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
	g_hash_table_insert(fmd->query, (void *)"max-results", g_strdup("50"));
	if (before)
		g_hash_table_insert(fmd->query, (void *)"before", g_strdup(before));
	if (after)
		g_hash_table_insert(fmd->query, (void *)"after", g_strdup(after));

	g_task_set_task_data(task, fmd, free_fetch_msg_data);
	fetch_messages_req(self, task);
}

gboolean
chime_connection_fetch_messages_finish(ChimeConnection *self, GAsyncResult *result,
				       GError **error)
{
	g_return_val_if_fail(CHIME_IS_CONNECTION(self), FALSE);
	g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

	return g_task_propagate_boolean(G_TASK(result), error);
}



static void update_last_read_cb(ChimeConnection *self, SoupMessage *msg,
				JsonNode *node, gpointer user_data)
{
	GTask *task = G_TASK(user_data);

	/* Nothing to do o nsuccess */
	if (!SOUP_STATUS_IS_SUCCESSFUL(msg->status_code)) {
		g_task_return_new_error(task, CHIME_ERROR,
					CHIME_ERROR_NETWORK,
					_("Failed to set LastReadMessageID: %d %s"),
					msg->status_code, msg->reason_phrase);
	} else
		g_task_return_boolean(task, TRUE);

	g_object_unref(task);
}


void chime_connection_update_last_read_async (ChimeConnection    *self,
					      ChimeObject        *obj,
					      const gchar        *msg_id,
					      GCancellable       *cancellable,
					      GAsyncReadyCallback callback,
					      gpointer            user_data)
{
	g_return_if_fail(CHIME_IS_CONNECTION(self));
	ChimeConnectionPrivate *priv = chime_connection_get_private (self);

	GTask *task = g_task_new(self, cancellable, callback, user_data);

	JsonBuilder *jb = json_builder_new();
	jb = json_builder_begin_object(jb);
	jb = json_builder_set_member_name(jb, "LastReadMessageId");
	jb = json_builder_add_string_value(jb, msg_id);
	jb = json_builder_end_object(jb);

	SoupURI *uri = soup_uri_new_printf(priv->messaging_url,
					   "/%ss/%s",
					   CHIME_IS_ROOM(obj) ? "room" : "conversation",
					   chime_object_get_id(obj));
	JsonNode *node = json_builder_get_root(jb);
	chime_connection_queue_http_request(self, node, uri, "POST", update_last_read_cb, task);
	json_node_unref(node);
	g_object_unref(jb);
}

gboolean chime_connection_update_last_read_finish (ChimeConnection  *self,
						   GAsyncResult     *result,
						   GError          **error)
{
	g_return_val_if_fail(CHIME_IS_CONNECTION(self), FALSE);
	g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

	return g_task_propagate_boolean(G_TASK(result), error);
}

static void logout_cb(ChimeConnection *self, SoupMessage *msg,
		       JsonNode *node, gpointer user_data)
{
	GTask *task = G_TASK(user_data);

	/* Nothing to do o nsuccess */
	if (!SOUP_STATUS_IS_SUCCESSFUL(msg->status_code)) {
		g_task_return_new_error(task, CHIME_ERROR,
					CHIME_ERROR_NETWORK,
					_("Failed to log out: %d %s"),
					msg->status_code, msg->reason_phrase);
	} else
		g_task_return_boolean(task, TRUE);

	g_object_unref(task);
}


void chime_connection_log_out_async (ChimeConnection    *self,
				     GCancellable       *cancellable,
				     GAsyncReadyCallback callback,
				     gpointer            user_data)
{
	g_return_if_fail(CHIME_IS_CONNECTION(self));
	ChimeConnectionPrivate *priv = chime_connection_get_private (self);

	GTask *task = g_task_new(self, cancellable, callback, user_data);

	SoupURI *uri = soup_uri_new_printf(priv->server, "/sessions");
	chime_connection_queue_http_request(self, NULL, uri, "DELETE", logout_cb, task);
}

gboolean chime_connection_log_out_finish (ChimeConnection  *self,
					  GAsyncResult     *result,
					  GError          **error)
{
	g_return_val_if_fail(CHIME_IS_CONNECTION(self), FALSE);
	g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

	return g_task_propagate_boolean(G_TASK(result), error);
}
