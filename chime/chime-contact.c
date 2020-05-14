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

#include <glib/gi18n.h>

enum
{
	PROP_0,
	PROP_PROFILE_CHANNEL,
	PROP_PRESENCE_CHANNEL,
	PROP_FULL_NAME,
	PROP_DISPLAY_NAME,
	PROP_AVAILABILITY,
	LAST_PROP,
};

static GParamSpec *props[LAST_PROP];

struct _ChimeContact {
	ChimeObject parent_instance;

	gboolean subscribed;
	ChimeConnection *cxn; /* For unsubscribing from jugg channels */

	gchar *presence_channel;
	gchar *profile_channel;
	gchar *full_name;
	gchar *display_name;

	ChimeAvailability availability;
	gint64 avail_revision;
};

G_DEFINE_TYPE(ChimeContact, chime_contact, CHIME_TYPE_OBJECT)

#define CHIME_AVAILABILITY_VALUES \
       { CHIME_AVAILABILITY_UNKNOWN,	"unknown",	"Unknown" },\
       { CHIME_AVAILABILITY_OFFLINE,	"offline",	"Offline" },\
       { CHIME_AVAILABILITY_AVAILABLE,	"available",	"Available" },\
       { CHIME_AVAILABILITY_AWAY,	"away",		"Away" },\
       { CHIME_AVAILABILITY_BUSY,	"busy",		"Busy" },\
       { CHIME_AVAILABILITY_MOBILE,	"mobile",	"Mobile" },\
       { CHIME_AVAILABILITY_PRIVATE,	"private",	"Private" },\
       { CHIME_AVAILABILITY_DO_NOT_DISTURB, "dnd",	"DoNotDisturb" },

CHIME_DEFINE_ENUM_TYPE(ChimeAvailability, chime_availability, CHIME_AVAILABILITY_VALUES)


const gchar *chime_availability_name(ChimeAvailability av)
{
	gpointer klass = g_type_class_ref(CHIME_TYPE_AVAILABILITY);
	GEnumValue *val = g_enum_get_value(klass, av);
	g_type_class_unref(klass);
	if (val)
		return val->value_name;
	else
		return _("Unknown");
}

static void unsubscribe_contact(gpointer key, gpointer val, gpointer data);
static void subscribe_contact(ChimeConnection *cxn, ChimeContact *contact);

static void
chime_contact_dispose(GObject *object)
{
	ChimeContact *self = CHIME_CONTACT(object);

	unsubscribe_contact(NULL, self, NULL);
	chime_debug("Contact disposed: %p\n", self);

	G_OBJECT_CLASS(chime_contact_parent_class)->dispose(object);
}

static void
chime_contact_finalize(GObject *object)
{
	ChimeContact *self = CHIME_CONTACT(object);

	g_free(self->presence_channel);
	g_free(self->profile_channel);
	g_free(self->full_name);
	g_free(self->display_name);

	G_OBJECT_CLASS(chime_contact_parent_class)->finalize(object);
}

static gboolean contact_presence_jugg_cb(ChimeConnection *cxn, gpointer _self,
					 JsonNode *data_node);
static gboolean fetch_presences(gpointer _cxn);


static void chime_contact_get_property(GObject *object, guint prop_id,
				       GValue *value, GParamSpec *pspec)
{
	ChimeContact *self = CHIME_CONTACT(object);

	switch (prop_id) {
	case PROP_PROFILE_CHANNEL:
		g_value_set_string(value, self->profile_channel);
		break;
	case PROP_PRESENCE_CHANNEL:
		g_value_set_string(value, self->presence_channel);
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
	case PROP_PROFILE_CHANNEL:
		g_free(self->profile_channel);
		self->profile_channel = g_value_dup_string(value);
		break;
	case PROP_PRESENCE_CHANNEL:
		g_free(self->presence_channel);
		self->presence_channel = g_value_dup_string(value);
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
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void chime_contact_class_init(ChimeContactClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->finalize = chime_contact_finalize;
	object_class->dispose = chime_contact_dispose;
	object_class->get_property = chime_contact_get_property;
	object_class->set_property = chime_contact_set_property;

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

	g_object_class_install_properties(object_class, LAST_PROP, props);
}

static void chime_contact_init(ChimeContact *self)
{
}

const gchar *chime_contact_get_profile_id(ChimeContact *contact)
{
	g_return_val_if_fail(CHIME_IS_CONTACT(contact), NULL);

	return chime_object_get_id(CHIME_OBJECT(contact));
}

const gchar *chime_contact_get_email(ChimeContact *contact)
{
	g_return_val_if_fail(CHIME_IS_CONTACT(contact), NULL);

	return chime_object_get_name(CHIME_OBJECT(contact));
}

const gchar *chime_contact_get_full_name(ChimeContact *contact)
{
	g_return_val_if_fail(CHIME_IS_CONTACT(contact), NULL);

	return contact->full_name;
}

const gchar *chime_contact_get_display_name(ChimeContact *contact)
{
	g_return_val_if_fail(CHIME_IS_CONTACT(contact), NULL);

	return contact->display_name;
}

ChimeAvailability chime_contact_get_availability(ChimeContact *contact)
{
	g_return_val_if_fail(CHIME_IS_CONTACT(contact), CHIME_AVAILABILITY_UNKNOWN);

	if (!contact->subscribed)
		subscribe_contact(contact->cxn, contact);

	return contact->availability;
}

gboolean chime_contact_get_contacts_list(ChimeContact *contact)
{
	g_return_val_if_fail(CHIME_IS_CONTACT(contact), FALSE);

	return !chime_object_is_dead(CHIME_OBJECT(contact));
}

static void
subscribe_contact(ChimeConnection *cxn, ChimeContact *contact)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE(cxn);

	contact->cxn = cxn;

	if (contact->presence_channel)
		chime_jugg_subscribe(cxn, contact->presence_channel, "Presence",
				     contact_presence_jugg_cb, contact);

	/* As well as subscribing to the channel, we'll need to fetch the
	 * initial presence information for this contact */
	priv->contacts_needed = g_slist_prepend(priv->contacts_needed, contact);
	if (!priv->contacts_src_id)
		priv->contacts_src_id = g_idle_add(fetch_presences, g_object_ref(cxn));
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
	ChimeContact *contact = g_hash_table_lookup(priv->contacts.by_id, id);

	if (!contact) {
		contact = g_object_new(CHIME_TYPE_CONTACT,
				       "name", email,
				       "id", id,
				       "presence-channel", presence_channel,
				       "full-name", full_name,
				       "display-name", display_name,
				       "profile-channel", profile_channel,
				       NULL);

		contact->cxn = cxn;

		/* If it's not being hashed, keep it because our caller owns it */
		if (!is_contact)
			g_object_ref(contact);
		chime_object_collection_hash_object(&priv->contacts, CHIME_OBJECT(contact), is_contact);

		chime_connection_new_contact(cxn, contact);

		return contact;
	}

	/* This should never happen? */
	if (email && g_strcmp0(email, chime_object_get_name(CHIME_OBJECT(contact)))) {
		chime_object_rename(CHIME_OBJECT(contact), email);
	}
	if (full_name && g_strcmp0(full_name, contact->full_name)) {
		g_free(contact->full_name);
		contact->full_name = g_strdup(full_name);
		g_object_notify(G_OBJECT(contact), "full-name");
	}
	if (display_name && g_strcmp0(display_name, contact->display_name)) {
		g_free(contact->display_name);
		contact->display_name = g_strdup(display_name);
		g_object_notify(G_OBJECT(contact), "display-name");
	}

	if (presence_channel && !contact->presence_channel) {
		contact->presence_channel = g_strdup(presence_channel);
		g_object_notify(G_OBJECT(contact), "presence-channel");
		if (contact->subscribed)
			subscribe_contact(cxn, contact);
	}
	if (profile_channel && !contact->profile_channel) {
		contact->profile_channel = g_strdup(profile_channel);
		g_object_notify(G_OBJECT(contact), "profile-channel");
	}

	if (is_contact)
		chime_object_collection_hash_object(&priv->contacts,
						    CHIME_OBJECT(contact), TRUE);
	else
		g_object_ref(contact);

	return contact;
}

/* Returns a ChimeContact which is in the contacts list, and
 * caller does not own a ref on it. */
ChimeContact *chime_connection_parse_contact(ChimeConnection *cxn, gboolean is_contact,
					     JsonNode *node, GError **error)
{
	g_return_val_if_fail(CHIME_IS_CONNECTION(cxn), NULL);
	const gchar *email, *full_name, *display_name, *profile_id;
	const gchar *profile_channel = NULL, *presence_channel = NULL;

	if (!parse_string(node, "email", &email) ||
	    !parse_string(node, "full_name", &full_name) ||
	    !parse_string(node, "display_name", &display_name) ||
	    !parse_string(node, "id", &profile_id)) {
		g_set_error(error, CHIME_ERROR,
			    CHIME_ERROR_BAD_RESPONSE,
			    _("Failed to parse Contact node"));
		return NULL;
	}
	parse_string(node, "presence_channel", &presence_channel);
	parse_string(node, "profile_channel", &profile_channel);

	return find_or_create_contact(cxn, profile_id, presence_channel,
				      profile_channel, email, full_name,
				      display_name, is_contact, error);
}

/* Returns a ChimeContact which is not necessarily in the contacts list,
 * and caller owns a ref on it. */
ChimeContact *chime_connection_parse_conversation_contact(ChimeConnection *cxn,
							  JsonNode *node,
							  GError **error)
{
	g_return_val_if_fail(CHIME_IS_CONNECTION(cxn), NULL);
	const gchar *email, *full_name, *presence_channel, *display_name,
		*profile_id;

	if (!parse_string(node, "Email", &email) ||
	    !parse_string(node, "FullName", &full_name) ||
	    !parse_string(node, "PresenceChannel", &presence_channel) ||
	    !parse_string(node, "DisplayName", &display_name) ||
	    !parse_string(node, "ProfileId", &profile_id)) {
		g_set_error(error, CHIME_ERROR,
			    CHIME_ERROR_BAD_RESPONSE,
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

	if (!priv->contacts.by_id) {
		g_set_error(error, CHIME_ERROR,
			    CHIME_ERROR_BAD_RESPONSE,
			    _("Contacts hash table is not set"));
		return FALSE;
	}

	if (!parse_string(node, "ProfileId", &id) ||
	    !parse_int(node, "Revision", &revision) ||
	    !parse_int(node, "Availability", &availability)) {
		g_set_error(error, CHIME_ERROR,
			    CHIME_ERROR_BAD_RESPONSE,
			    _("Required fields in presence update not found"));
		return FALSE;
	}

	ChimeContact *contact = g_hash_table_lookup(priv->contacts.by_id, id);
	if (!contact) {
		g_set_error(error, CHIME_ERROR,
			    CHIME_ERROR_BAD_RESPONSE,
			    _("Contact %s not found; cannot update presence"),
			    id);
		return FALSE;
	}

	/* We already have newer data */
	if (revision < contact->avail_revision)
		return TRUE;

	contact->avail_revision = revision;
	if (contact->availability != availability) {
		contact->availability = availability;
		g_object_notify(G_OBJECT(contact), "availability");
	}

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
	GPtrArray *ids = g_ptr_array_new();

	while (priv->contacts_needed) {
		ChimeContact *contact = priv->contacts_needed->data;
		priv->contacts_needed = g_slist_remove(priv->contacts_needed,
						       contact);
		if (!contact || contact->avail_revision)
			continue;

		g_ptr_array_add(ids, (gpointer)chime_object_get_id(CHIME_OBJECT(contact)));
	}
	if (ids->len > 0) {
		g_ptr_array_add(ids, NULL);

		gchar *query = g_strjoinv(",", (gchar **)ids->pdata);

		SoupURI *uri = soup_uri_new_printf(priv->presence_url, "/presence");
		soup_uri_set_query_from_fields(uri, "profile-ids", query, NULL);
		g_free(query);

		chime_connection_queue_http_request(cxn, NULL, uri, "GET",
						    presence_cb, NULL);
	}
	g_ptr_array_unref(ids);
	priv->contacts_src_id = 0;
	g_object_unref(cxn);
	return FALSE;
}

static void fetch_contacts(ChimeConnection *cxn, const gchar *next_token);

static void contacts_cb(ChimeConnection *cxn, SoupMessage *msg, JsonNode *node,
			gpointer _unused)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	/* If it got invalidated while in transit, refetch */
	if (priv->contacts_sync != CHIME_SYNC_FETCHING) {
		priv->contacts_sync = CHIME_SYNC_IDLE;
		fetch_contacts(cxn, NULL);
		return;
	}

	if (SOUP_STATUS_IS_SUCCESSFUL(msg->status_code) && node) {
		JsonArray *arr = json_node_get_array(node);
		guint i, len = json_array_get_length(arr);

		for (i = 0; i < len; i++) {
			chime_connection_parse_contact(cxn, TRUE,
						       json_array_get_element(arr, i),
						       NULL);
		}

		const gchar *next_token = soup_message_headers_get_one(msg->response_headers, "aws-ucbuzz-nexttoken");;
		if (next_token)
			fetch_contacts(cxn, next_token);
		else {
			priv->contacts_sync = CHIME_SYNC_IDLE;

			chime_object_collection_expire_outdated(&priv->contacts);

			if (!priv->contacts_online) {
				priv->contacts_online = TRUE;
				chime_connection_calculate_online(cxn);
			}
		}
	} else {
		const gchar *reason = msg->reason_phrase;

		parse_string(node, "error", &reason);

		chime_connection_fail(cxn, CHIME_ERROR_NETWORK,
				      _("Failed to fetch contacts (%d): %s\n"),
				      msg->status_code, reason);
	}
}

static void fetch_contacts(ChimeConnection *cxn, const gchar *next_token)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	if (!next_token) {
		/* Actually we could listen for the 'starting' flag on the message,
		 * and as long as *that* hasn't happened yet we don't need to refetch
		 * as it'll get up-to-date information. */
		switch (priv->contacts_sync) {
		case CHIME_SYNC_FETCHING:
			priv->contacts_sync = CHIME_SYNC_STALE;
		case CHIME_SYNC_STALE:
			return;

		case CHIME_SYNC_IDLE:
			priv->contacts.generation++;
			priv->contacts_sync = CHIME_SYNC_FETCHING;
		}
	}

	SoupURI *uri = soup_uri_new_printf(priv->contacts_url, "/contacts");
	if (next_token)
		soup_uri_set_query_from_fields(uri, "next_token", next_token, NULL);

	chime_connection_queue_http_request(cxn, NULL, uri, "GET", contacts_cb,
					    NULL);
}

void chime_init_contacts(ChimeConnection *cxn)
{
	g_return_if_fail(CHIME_IS_CONNECTION(cxn));
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	chime_object_collection_init(cxn, &priv->contacts);

	fetch_contacts(cxn, NULL);
}

static void unsubscribe_contact(gpointer key, gpointer val, gpointer data)
{
	ChimeContact *contact = CHIME_CONTACT (val);
	if (contact->cxn) {
		ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (contact->cxn);

		priv->contacts_needed = g_slist_remove(priv->contacts_needed,
						       contact);

		if (contact->subscribed)
			chime_jugg_unsubscribe(contact->cxn, contact->presence_channel, "Presence",
					       contact_presence_jugg_cb, contact);

		contact->cxn = NULL;
	}
}

void chime_destroy_contacts(ChimeConnection *cxn)
{
	g_return_if_fail(CHIME_IS_CONNECTION(cxn));
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	if (priv->contacts_src_id) {
		g_source_remove(priv->contacts_src_id);
		priv->contacts_src_id = 0;
	}
	if (priv->contacts_needed) {
		g_slist_free(priv->contacts_needed);
		priv->contacts_needed = NULL;
	}
	if (priv->contacts.by_id)
		g_hash_table_foreach(priv->contacts.by_id, unsubscribe_contact, NULL);

	chime_object_collection_destroy(&priv->contacts);
}

ChimeContact *chime_connection_contact_by_email(ChimeConnection *cxn,
						const gchar *email)
{
	g_return_val_if_fail(CHIME_IS_CONNECTION(cxn), NULL);
	g_return_val_if_fail(email != NULL, NULL);
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	return g_hash_table_lookup(priv->contacts.by_name, email);
}

ChimeContact *chime_connection_contact_by_id(ChimeConnection *cxn,
					     const gchar *id)
{
	g_return_val_if_fail(CHIME_IS_CONNECTION(cxn), NULL);
	g_return_val_if_fail(id != NULL, NULL);
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	return g_hash_table_lookup(priv->contacts.by_id, id);
}

struct foreach_contact_st {
	ChimeConnection *cxn;
	ChimeContactCB cb;
	gpointer cbdata;
};

void chime_connection_foreach_contact(ChimeConnection *cxn, ChimeContactCB cb,
				      gpointer cbdata)
{
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);
	chime_object_collection_foreach_object(cxn, &priv->contacts, (ChimeObjectCB)cb, cbdata);
}

static void contact_invited_cb(ChimeConnection *cxn, SoupMessage *msg,
			       JsonNode *node, gpointer user_data)
{
	GTask *task = G_TASK(user_data);

	if (!SOUP_STATUS_IS_SUCCESSFUL(msg->status_code)) {
		const gchar *reason = msg->reason_phrase;

		parse_string(node, "error", &reason);
		g_task_return_new_error(task, CHIME_ERROR,
					CHIME_ERROR_NETWORK,
					_("Failed to add/invite contact: %s"),
					reason);
	} else {
		g_task_return_boolean(task, TRUE);

		/* There is weirdness here. If this is a known person, then we can
		 * *immediately* fetch their full name and other information by
		 * refetching *all* buddies. So why in $DEITY's name does it not
		 * get returned to us in the reply? I can't even see any way to
		 * fetch just this single buddy, either; we have to refetch them
		 * all. */
		fetch_contacts(cxn, NULL);
	}

	g_object_unref(task);
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
		g_task_return_new_error(task, CHIME_ERROR,
					CHIME_ERROR_NETWORK,
					_("Failed to remove contact: %s"),
					reason);

		/* We'll put it back */
		fetch_contacts(cxn, NULL);
	} else {
		g_task_return_boolean(task, TRUE);
	}

	g_object_unref(task);
}


void chime_connection_remove_contact_async(ChimeConnection *cxn,
					   const gchar *email,
					   GCancellable *cancellable,
					   GAsyncReadyCallback callback,
					   gpointer user_data)
{
	g_return_if_fail(CHIME_IS_CONNECTION(cxn));
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	ChimeContact *contact = g_hash_table_lookup(priv->contacts.by_name,
						    email);
	if (!contact) {
		g_task_report_new_error(cxn, callback, user_data,
		                        chime_connection_remove_contact_async,
		                        CHIME_ERROR,
					CHIME_ERROR_NETWORK,
					_("Failed to remove unknown contact %s"),
					email);
		return;
	}

	GTask *task = g_task_new(cxn, cancellable, callback, user_data);

	SoupURI *uri = soup_uri_new_printf(priv->contacts_url, "/contacts/%s",
					   chime_object_get_id(CHIME_OBJECT(contact)));
	chime_connection_queue_http_request(cxn, NULL, uri, "DELETE",
					    contact_removed_cb, task);

	/* Assume success; we'll refetch and reinstate it on failure */
	chime_object_collection_hash_object(&priv->contacts, CHIME_OBJECT(contact), FALSE);
}

gboolean chime_connection_remove_contact_finish(ChimeConnection *self,
						GAsyncResult *result,
						GError **error)
{
	g_return_val_if_fail(CHIME_IS_CONNECTION(self), FALSE);
	g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

	return g_task_propagate_boolean(G_TASK(result), error);
}

static void autocomplete_cb(ChimeConnection *cxn, SoupMessage *msg,
			    JsonNode *node, gpointer user_data)
{
	GTask *task = G_TASK(user_data);

	if (SOUP_STATUS_IS_SUCCESSFUL(msg->status_code) && node) {
		GSList *results = NULL;
		ChimeContact *contact;

		JsonArray *arr = json_node_get_array(node);
		guint i, len = json_array_get_length(arr);

		for (i = 0; i < len; i++) {
			contact = chime_connection_parse_contact(cxn, FALSE,
								 json_array_get_element(arr, i),
								 NULL);
			if (contact)
				results = g_slist_append(results, contact);
		}
		g_task_return_pointer(task, results, NULL);
	} else {
		const gchar *reason = msg->reason_phrase;

		parse_string(node, "error", &reason);

		g_task_return_new_error(task, CHIME_ERROR,
					CHIME_ERROR_NETWORK,
					_("Failed to autocomplete: %s"),
					reason);
	}
	g_object_unref(task);
}

void chime_connection_autocomplete_contact_async(ChimeConnection *cxn,
						 const gchar *query,
						 GCancellable *cancellable,
						 GAsyncReadyCallback callback,
						 gpointer user_data)
{

	g_return_if_fail(CHIME_IS_CONNECTION(cxn));
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE (cxn);

	GTask *task = g_task_new(cxn, cancellable, callback, user_data);

	SoupURI *uri = soup_uri_new_printf(priv->express_url, "/bazl/contact-auto-completes");
	JsonBuilder *jb = json_builder_new();
	jb = json_builder_begin_object(jb);
	jb = json_builder_set_member_name(jb, "q");
	jb = json_builder_add_string_value(jb, query);
	jb = json_builder_end_object(jb);

	JsonNode *node = json_builder_get_root(jb);
	chime_connection_queue_http_request(cxn, node, uri, "POST", autocomplete_cb, task);
	json_node_unref(node);
	g_object_unref(jb);
}

GSList *chime_connection_autocomplete_contact_finish(ChimeConnection *self,
					       GAsyncResult *result,
					       GError **error)
{
	g_return_val_if_fail(CHIME_IS_CONNECTION(self), FALSE);
	g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

	return g_task_propagate_pointer(G_TASK(result), error);
}
