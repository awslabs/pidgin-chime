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


#include "chime-connection-private.h"
#include "chime-contact.h"
#include "chime.h"

#include <glib/gi18n.h>

enum
{
	PROP_0,
	PROP_CONNECTION,
	PROP_PROFILE_ID,
	PROP_PROFILE_CHANNEL,
	PROP_PRESENCE_CHANNEL,
	PROP_EMAIL,
	PROP_FULL_NAME,
	PROP_DISPLAY_NAME,
	PROP_AVAILABILITY,
	PROP_CONTACTS_LIST,
	LAST_PROP,
};

static GParamSpec *props[LAST_PROP];

struct _ChimeContact {
	GObject parent_instance;

	/* We don't hold a reference on this because it references us */
	ChimeConnection *cxn;

	gchar *profile_id;
	gchar *presence_channel;
	gchar *profile_channel;
	gchar *email;
	gchar *full_name;
	gchar *display_name;

	ChimeAvailability availability;
	gint64 avail_revision;

	/* Is this contact from contacts list (as opposed to a conversation)? */
	gboolean contacts_list;
};

G_DEFINE_TYPE(ChimeContact, chime_contact, G_TYPE_OBJECT)

static void
chime_contact_finalize(GObject *object)
{
	ChimeContact *self = CHIME_CONTACT(object);

	g_free(self->profile_id);
	g_free(self->presence_channel);
	g_free(self->profile_channel);
	g_free(self->email);
	g_free(self->full_name);
	g_free(self->display_name);

	G_OBJECT_CLASS(chime_contact_parent_class)->finalize(object);
}

static gboolean contact_presence_jugg_cb(ChimeConnection *cxn, gpointer _self,
					 JsonNode *data_node);
static gboolean fetch_presences(gpointer _cxn);

static void chime_contact_dispose(GObject *object)
{
	ChimeContact *self = CHIME_CONTACT(object);

	chime_jugg_unsubscribe (self->cxn, self->presence_channel, "Presence",
				contact_presence_jugg_cb, self);
	chime_jugg_unsubscribe (self->cxn, self->profile_channel, NULL, NULL,
				NULL);

#if 0	/* Actually for now contacts will last for the entire lifetime of the
	   ChimeConnection so they'll be unreffed by the destructor on the
	   contacts_by_id hash table and we don't have to remove here... */

	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE(self->cxn);

	g_hash_table_remove(priv->contacts_by_id, self->profile_id);
	g_hash_table_remove(priv->contacts_by_email, self->email);
#endif

	G_OBJECT_CLASS(chime_contact_parent_class)->dispose(object);
}

static void chime_contact_get_property(GObject *object, guint prop_id,
				       GValue *value, GParamSpec *pspec)
{
	ChimeContact *self = CHIME_CONTACT(object);

	switch (prop_id) {
	case PROP_CONNECTION:
		g_value_set_pointer(value, self->cxn);
		break;
	case PROP_PROFILE_ID:
		g_value_set_string(value, self->profile_id);
		break;
	case PROP_PROFILE_CHANNEL:
		g_value_set_string(value, self->profile_channel);
		break;
	case PROP_PRESENCE_CHANNEL:
		g_value_set_string(value, self->presence_channel);
		break;
	case PROP_EMAIL:
		g_value_set_string(value, self->email);
		break;
	case PROP_FULL_NAME:
		g_value_set_string(value, self->full_name);
		break;
	case PROP_DISPLAY_NAME:
		g_value_set_string(value, self->display_name);
		break;
	case PROP_AVAILABILITY:
		g_value_set_int(value, self->availability);
		break;
	case PROP_CONTACTS_LIST:
		g_value_set_boolean(value, self->contacts_list);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void chime_contact_set_property(GObject *object, guint prop_id,
				       const GValue *value, GParamSpec *pspec)
{
	ChimeContact *self = CHIME_CONTACT(object);

	switch (prop_id) {
	case PROP_CONNECTION:
		self->cxn = g_value_get_pointer(value);
		break;
	case PROP_PROFILE_ID:
		g_free(self->profile_id);
		self->profile_id = g_value_dup_string(value);
		break;
	case PROP_PROFILE_CHANNEL:
		g_free(self->profile_channel);
		self->profile_channel = g_value_dup_string(value);
		break;
	case PROP_PRESENCE_CHANNEL:
		g_free(self->presence_channel);
		self->presence_channel = g_value_dup_string(value);
		break;
	case PROP_EMAIL:
		g_free(self->email);
		self->email = g_value_dup_string(value);
		break;
	case PROP_FULL_NAME:
		g_free(self->full_name);
		self->full_name = g_value_dup_string(value);
		break;
	case PROP_DISPLAY_NAME:
		g_free(self->display_name);
		self->display_name = g_value_dup_string(value);
		break;
	case PROP_AVAILABILITY:
		self->availability = g_value_get_int(value);
		break;
	case PROP_CONTACTS_LIST:
		self->contacts_list = g_value_get_boolean(value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void chime_contact_constructed(GObject *object)
{
	ChimeContact *self = CHIME_CONTACT(object);
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE(self->cxn);

        G_OBJECT_CLASS (chime_contact_parent_class)->constructed (object);

	chime_jugg_subscribe (self->cxn, self->presence_channel, "Presence",
			      contact_presence_jugg_cb, self);

	if (self->profile_channel)
		chime_jugg_subscribe (self->cxn, self->profile_channel,
				      NULL, NULL, NULL);

	/* As well as subscribing to the channel, we'll need to fetch the
	 * initial presence information for this contact */
	priv->contacts_needed = g_slist_prepend(priv->contacts_needed, self);
	g_idle_add(fetch_presences, self->cxn);

	g_hash_table_insert(priv->contacts_by_id, self->profile_id, self);
	g_hash_table_insert(priv->contacts_by_email, self->email, self);

	/* Emit signal on ChimeConnection to admit existence of new contact */
	chime_connection_new_contact(self->cxn, self);
}


static void chime_contact_class_init(ChimeContactClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->finalize = chime_contact_finalize;
	object_class->dispose = chime_contact_dispose;
	object_class->constructed = chime_contact_constructed;
	object_class->get_property = chime_contact_get_property;
	object_class->set_property = chime_contact_set_property;

	props[PROP_CONNECTION] =
		g_param_spec_pointer("connection",
				     "connection",
				     "connection",
				     G_PARAM_READWRITE |
				     G_PARAM_CONSTRUCT_ONLY |
				     G_PARAM_STATIC_STRINGS);

	props[PROP_PROFILE_ID] =
		g_param_spec_string("profile-id",
				    "profile id",
				    "profile id",
				    NULL,
				    G_PARAM_READWRITE |
				    G_PARAM_CONSTRUCT_ONLY |
				    G_PARAM_STATIC_STRINGS);

	props[PROP_PROFILE_CHANNEL] =
		g_param_spec_string("profile-channel",
				    "profile channel",
				    "profile channel",
				    NULL,
				    G_PARAM_READWRITE |
				    G_PARAM_CONSTRUCT_ONLY |
				    G_PARAM_STATIC_STRINGS);

	props[PROP_PRESENCE_CHANNEL] =
		g_param_spec_string("presence-channel",
				    "presence channel",
				    "presence channel",
				    NULL,
				    G_PARAM_READWRITE |
				    G_PARAM_CONSTRUCT_ONLY |
				    G_PARAM_STATIC_STRINGS);

	props[PROP_EMAIL] =
		g_param_spec_string("email",
				    "email",
				    "email",
				    NULL,
				    G_PARAM_READWRITE |
				    G_PARAM_CONSTRUCT |
				    G_PARAM_STATIC_STRINGS);

	props[PROP_FULL_NAME] =
		g_param_spec_string("full-name",
				    "full name",
				    "full name",
				    NULL,
				    G_PARAM_READWRITE |
				    G_PARAM_CONSTRUCT |
				    G_PARAM_STATIC_STRINGS);

	props[PROP_DISPLAY_NAME] =
		g_param_spec_string("display-name",
				    "display name",
				    "display name",
				    NULL,
				    G_PARAM_READWRITE |
				    G_PARAM_CONSTRUCT |
				    G_PARAM_STATIC_STRINGS);

	props[PROP_AVAILABILITY] =
		g_param_spec_int("availability",
				 "availability",
				 "availability",
				 0, CHIME_AVAILABILITY_LAST - 1,
				 CHIME_AVAILABILITY_UNKNOWN,
				 G_PARAM_READWRITE |
				 G_PARAM_CONSTRUCT |
				 G_PARAM_STATIC_STRINGS);

	props[PROP_CONTACTS_LIST] =
		g_param_spec_boolean("contacts-list",
				     "contacts list",
				     "contacts list",
				     FALSE,
				     G_PARAM_READWRITE |
				     G_PARAM_CONSTRUCT |
				     G_PARAM_STATIC_STRINGS);

	g_object_class_install_properties(object_class, LAST_PROP, props);
}

static void chime_contact_init(ChimeContact *self)
{
}

static ChimeContact *find_or_create_contact(ChimeConnection *cxn, const gchar *id,
					    const gchar *presence_channel,
					    const gchar *profile_channel,
					    const gchar *email,
					    const gchar *full_name,
					    const gchar *display_name,
					    gboolean is_contact,
					    GError **error)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE(cxn);
	ChimeContact *contact = g_hash_table_lookup(priv->contacts_by_id, id);

	if (!contact)
		return g_object_new(CHIME_TYPE_CONTACT,
				    "connection", cxn,
				    "email", email,
				    "profile-id", id,
				    "presence-channel", presence_channel,
				    "full-name", full_name,
				    "display-name", display_name,
				    "profile-channel", profile_channel,
				    "contacts-list", is_contact,
				    NULL);

	if (email && g_strcmp0(email, contact->email)) {
		g_hash_table_remove(priv->contacts_by_email, contact->email);
		g_free(contact->email);
		contact->email = g_strdup(email);
		g_hash_table_insert(priv->contacts_by_email, contact->email, contact);
		g_object_notify(G_OBJECT(contact), "email");
	}
	if (full_name && g_strcmp0(full_name, contact->full_name)) {
		g_free(contact->full_name);
		contact->email = g_strdup(full_name);
		g_object_notify(G_OBJECT(contact), "full-name");
	}
	if (display_name && g_strcmp0(display_name, contact->display_name)) {
		g_free(contact->display_name);
		contact->display_name = g_strdup(display_name);
		g_object_notify(G_OBJECT(contact), "display-name");
	}
	if (is_contact && !contact->contacts_list) {
		contact->contacts_list = is_contact;
		g_object_notify(G_OBJECT(contact), "contacts-list");
	}
	return contact;
}


ChimeContact *chime_connection_parse_contact(ChimeConnection *cxn,
					     JsonNode *node, GError **error)
{
	const gchar *email, *full_name, *presence_channel, *display_name,
		*profile_id, *profile_channel;

	if (!parse_string(node, "email", &email) ||
	    !parse_string(node, "full_name", &full_name) ||
	    !parse_string(node, "presence_channel", &presence_channel) ||
	    !parse_string(node, "profile_channel", &profile_channel) ||
	    !parse_string(node, "display_name", &display_name) ||
	    !parse_string(node, "id", &profile_id)) {
		g_set_error(error, CHIME_CONNECTION_ERROR,
			    CHIME_CONNECTION_ERROR_PARSE,
			    _("Failed to parse Contact node"));
		return NULL;
	}

	return find_or_create_contact(cxn, profile_id, presence_channel,
				      profile_channel, email, full_name,
				      display_name, TRUE, error);
}

ChimeContact *chime_connection_parse_conversation_contact(ChimeConnection *cxn,
							  JsonNode *node,
							  GError **error)
{
	const gchar *email, *full_name, *presence_channel, *display_name,
		*profile_id;

	if (!parse_string(node, "Email", &email) ||
	    !parse_string(node, "FullName", &full_name) ||
	    !parse_string(node, "PresenceChannel", &presence_channel) ||
	    !parse_string(node, "DisplayName", &display_name) ||
	    !parse_string(node, "ProfileId", &profile_id)) {
		g_set_error(error, CHIME_CONNECTION_ERROR,
			    CHIME_CONNECTION_ERROR_PARSE,
			    _("Failed to parse Contact node"));
		return NULL;
	}

	return find_or_create_contact(cxn, profile_id, presence_channel, NULL,
				      email, full_name, display_name, FALSE,
				      error);
}

/* Update contact presence with a node obtained with via a juggernaut
 * channel or explicit request. */
static gboolean set_contact_presence(ChimeConnection *cxn, JsonNode *node,
				     GError **error)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);
	gint64 availability, revision;
	const gchar *id;

	if (!priv->contacts_by_id) {
		g_set_error(error, CHIME_CONNECTION_ERROR,
			    CHIME_CONNECTION_ERROR_PARSE,
			    _("Contacts hash table is not set"));
		return FALSE;
	}

	if (!parse_string(node, "ProfileId", &id) ||
	    !parse_int(node, "Revision", &revision) ||
	    !parse_int(node, "Availability", &availability)) {
		g_set_error(error, CHIME_CONNECTION_ERROR,
			    CHIME_CONNECTION_ERROR_PARSE,
			    _("Required fields in presence update not found"));
		return FALSE;
	}

	ChimeContact *contact = g_hash_table_lookup(priv->contacts_by_id, id);
	if (!contact) {
		g_set_error(error, CHIME_CONNECTION_ERROR,
			    CHIME_CONNECTION_ERROR_PARSE,
			    _("Contact %s not found; cannot update presence"),
			    id);
		return FALSE;
	}

	/* We already have newer data */
	if (revision < contact->avail_revision)
		return TRUE;

	contact->avail_revision = revision;
	contact->availability = availability;
	g_object_notify(G_OBJECT(contact), "availability");

	return TRUE;
}

/* Callback for Juggernaut notifications about status */
static gboolean contact_presence_jugg_cb(ChimeConnection *cxn, gpointer _unused,
					 JsonNode *data_node)
{
	JsonObject *obj = json_node_get_object(data_node);
	JsonNode *record = json_object_get_member(obj, "record");
	if (!record)
		return FALSE;

	return set_contact_presence(cxn, record, NULL);
}

static void presence_cb(ChimeConnection *cxn, SoupMessage *msg,
			 JsonNode *node, gpointer _unused)
{
	if (!SOUP_STATUS_IS_SUCCESSFUL(msg->status_code))
		return;

	JsonObject *obj = json_node_get_object(node);
	node = json_object_get_member(obj, "Presences");
	if (!node)
		return;

	JsonArray *arr = json_node_get_array(node);
	int i, len = json_array_get_length(arr);
	for (i = 0; i < len; i++)
		set_contact_presence(cxn, json_array_get_element(arr, i), NULL);
}

static gboolean fetch_presences(gpointer _cxn)
{
	ChimeConnection *cxn = _cxn;
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);
	int i, len = g_slist_length(priv->contacts_needed);
	if (!len)
		return FALSE;

	gchar **ids = g_new0(gchar *, len + 1);
	i = 0;
	while (priv->contacts_needed) {
		ChimeContact *contact = priv->contacts_needed->data;
		priv->contacts_needed = g_slist_remove(priv->contacts_needed,
						       contact);
		if (!contact || contact->avail_revision)
			continue;

		ids[i++] = contact->profile_id;
	}
	/* We don't actually need any */
	if (i) {
		ids[i++] = NULL;
		gchar *query = g_strjoinv(",", ids);

		SoupURI *uri = soup_uri_new_printf(priv->presence_url, "/presence");
		soup_uri_set_query_from_fields(uri, "profile-ids", query, NULL);
		g_free(query);

		chime_connection_queue_http_request(cxn, NULL, uri, "GET",
						    presence_cb, NULL);
	}
	g_free(ids);
	return FALSE;
}


static void contacts_cb(ChimeConnection *cxn, SoupMessage *msg, JsonNode *node,
			gpointer _unused)
{
	if (SOUP_STATUS_IS_SUCCESSFUL(msg->status_code) && node) {
		JsonArray *arr = json_node_get_array(node);
		guint i, len = json_array_get_length(arr);
		for (i = 0; i < len; i++)
			chime_connection_parse_contact(cxn,
						       json_array_get_element(arr, i),
						       NULL);
	}
}

static void fetch_contacts(ChimeConnection *cxn)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);
	SoupURI *uri = soup_uri_new_printf(priv->contacts_url, "/contacts");
	chime_connection_queue_http_request(cxn, NULL, uri, "GET", contacts_cb,
					    NULL);
}


void chime_init_contacts(ChimeConnection *cxn)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);
	priv->contacts_by_id = g_hash_table_new_full(g_str_hash, g_str_equal,
						     NULL, g_object_unref);
	priv->contacts_by_email = g_hash_table_new(g_str_hash, g_str_equal);

	fetch_contacts(cxn);
}

void chime_destroy_contacts(ChimeConnection *cxn)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);
	g_hash_table_destroy(priv->contacts_by_email);
	g_hash_table_destroy(priv->contacts_by_id);
	priv->contacts_by_email = priv->contacts_by_id = NULL;
}

ChimeContact *chime_connection_contact_by_email(ChimeConnection *cxn,
						const gchar *email)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	return g_hash_table_lookup(priv->contacts_by_email, email);
}

struct foreach_contact_st {
	ChimeContactCB cb;
	gpointer cbdata;
};

static void foreach_contact_cb(gpointer key, gpointer value, gpointer _data)
{
	struct foreach_contact_st *data = _data;
	ChimeContact *contact = CHIME_CONTACT(value);

	data->cb(contact->cxn, contact, data->cbdata);
}

void chime_connection_foreach_contact(ChimeConnection *cxn, ChimeContactCB cb,
				      gpointer cbdata)
{
	struct foreach_contact_st data = {
		.cb = cb,
		.cbdata = cbdata,
	};

	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE(cxn);
	g_hash_table_foreach(priv->contacts_by_id, foreach_contact_cb, &data);
}

static void contact_invited_cb(ChimeConnection *cxn, SoupMessage *msg,
			       JsonNode *node, gpointer user_data)
{
	GTask *task = G_TASK(user_data);

	if (!SOUP_STATUS_IS_SUCCESSFUL(msg->status_code)) {
		const gchar *reason = msg->reason_phrase;

		parse_string(node, "error", &reason);
		g_task_return_new_error(task, CHIME_CONNECTION_ERROR,
					CHIME_CONNECTION_ERROR_NETWORK,
					_("Failed to add/invite contact: %s"),
					reason);
		return;
	}

	g_task_return_boolean(task, TRUE);

	/* There is weirdness here. If this is a known person, then we can
	 * *immediately* fetch their full name and other information by
	 * refetching *all* buddies. So why in $DEITY's name does it not
	 * get returned to us in the reply? I can't even see any way to
	 * fetch just this single buddy, either; we have to refetch them
	 * all. */
	fetch_contacts(cxn);
}

void chime_connection_invite_contact_async(ChimeConnection *cxn,
					   const gchar *email,
					   GCancellable *cancellable,
					   GAsyncReadyCallback callback,
					   gpointer user_data)
{
	g_return_if_fail(CHIME_IS_CONNECTION(cxn));
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	GTask *task = g_task_new(cxn, cancellable, callback, user_data);
	JsonBuilder *builder = json_builder_new();
	builder = json_builder_begin_object(builder);
	builder = json_builder_set_member_name(builder, "profile");
	builder = json_builder_begin_object(builder);
	builder = json_builder_set_member_name(builder, "email");
	builder = json_builder_add_string_value(builder, email);
	builder = json_builder_end_object(builder);
	builder = json_builder_end_object(builder);
	JsonNode *node = json_builder_get_root(builder);

	SoupURI *uri = soup_uri_new_printf(priv->contacts_url, "/invites");
	chime_connection_queue_http_request(cxn, node, uri, "POST",
					    contact_invited_cb, task);

	json_node_unref(node);
	g_object_unref(builder);
}

gboolean chime_connection_invite_contact_finish(ChimeConnection *self,
						GAsyncResult *result,
						GError **error)
{
	g_return_val_if_fail(CHIME_IS_CONNECTION(self), FALSE);
	g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

	return g_task_propagate_boolean(G_TASK(result), error);
}

static void contact_removed_cb(ChimeConnection *cxn, SoupMessage *msg,
			       JsonNode *node, gpointer user_data)
{
	GTask *task = G_TASK(user_data);

	if (!SOUP_STATUS_IS_SUCCESSFUL(msg->status_code)) {
		const gchar *reason = msg->reason_phrase;

		parse_string(node, "error", &reason);
		g_task_return_new_error(task, CHIME_CONNECTION_ERROR,
					CHIME_CONNECTION_ERROR_NETWORK,
					_("Failed to remove contact: %s"),
					reason);

		/* We'll put it back */
		fetch_contacts(cxn);
		return;
	}

	g_task_return_boolean(task, TRUE);
}


void chime_connection_remove_contact_async(ChimeConnection *cxn,
					   const gchar *email,
					   GCancellable *cancellable,
					   GAsyncReadyCallback callback,
					   gpointer user_data)
{
	g_return_if_fail(CHIME_IS_CONNECTION(cxn));
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	GTask *task = g_task_new(cxn, cancellable, callback, user_data);
	ChimeContact *contact = g_hash_table_lookup(priv->contacts_by_email,
						    email);
	if (!contact) {
		g_task_return_new_error(task, CHIME_CONNECTION_ERROR,
					CHIME_CONNECTION_ERROR_NETWORK,
					_("Failed to remove unknown contact %s"),
					email);
		return;
	}

	/* Assume success; we'll refetch and reinstate it on failure */
	contact->contacts_list = FALSE;
	g_object_notify(G_OBJECT(contact), "contacts-list");

	SoupURI *uri = soup_uri_new_printf(priv->contacts_url, "/contacts/%s",
					   contact->profile_id);
	chime_connection_queue_http_request(cxn, NULL, uri, "DELETE",
					    contact_removed_cb, task);
}

gboolean chime_connection_remove_contact_finish(ChimeConnection *self,
						GAsyncResult *result,
						GError **error)
{
	g_return_val_if_fail(CHIME_IS_CONNECTION(self), FALSE);
	g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

	return g_task_propagate_boolean(G_TASK(result), error);
}
