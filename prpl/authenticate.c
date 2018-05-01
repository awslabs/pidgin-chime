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
#include <request.h>

#include "chime.h"

#define TITLE_LABEL  _("Chime Sign In Authentication")
#define USER_LABEL  _("Username")
#define PASS_LABEL  _("Password")
#define SEND_LABEL  _("Sign In")
#define CANCEL_LABEL  _("Cancel")
#define USER_FIELD  "username"
#define PASS_FIELD  "password"

struct auth_data {
	PurpleConnection *conn;
	gpointer state;
	gboolean user_required;
	gchar *username;
	gchar *password;
};

static void send_credentials(struct auth_data *data)
{
	chime_connection_authenticate(data->state, data->username, data->password);
	g_free(data->username);
	g_free(data->password);
	g_free(data);
}

static void gather_credentials_from_fields(struct auth_data *data, PurpleRequestFields *fields)
{
	if (data->user_required)
		data->username = g_strdup(purple_request_fields_get_string(fields, USER_FIELD));
	data->password = g_strdup(purple_request_fields_get_string(fields, PASS_FIELD));
	send_credentials(data);
}

static void request_credentials_with_fields(struct auth_data *data)
{
	PurpleRequestField *username, *password;
	PurpleRequestFieldGroup *group;
	PurpleRequestFields *fields;

	fields = purple_request_fields_new();
	group = purple_request_field_group_new(NULL);

	if (data->user_required) {
		username = purple_request_field_string_new(USER_FIELD, USER_LABEL, NULL, FALSE);
		purple_request_field_set_required(username, TRUE);
		purple_request_field_group_add_field(group, username);
	}

	password = purple_request_field_string_new(PASS_FIELD, PASS_LABEL, NULL, FALSE);
	purple_request_field_string_set_masked(password, TRUE);
	purple_request_field_set_required(password, TRUE);
	purple_request_field_group_add_field(group, password);

	purple_request_fields_add_group(fields, group);

	purple_request_fields(data->conn, TITLE_LABEL, NULL, NULL, fields,
			      SEND_LABEL, G_CALLBACK(gather_credentials_from_fields),
			      CANCEL_LABEL, G_CALLBACK(send_credentials),
			      data->conn->account, NULL, NULL, data);
}

static void gather_password_from_input(struct auth_data *data, const gchar *password)
{
	data->password = g_strdup(password);
	send_credentials(data);
}

static void request_password_with_input(struct auth_data *data, const gchar *username)
{
	data->username = g_strdup(username);
	purple_request_input(data->conn, TITLE_LABEL, PASS_LABEL,
			     NULL, NULL, FALSE, TRUE, (gchar *) PASS_FIELD,
			     SEND_LABEL, G_CALLBACK(gather_password_from_input),
			     CANCEL_LABEL, G_CALLBACK(send_credentials),
			     data->conn->account, NULL, NULL, data);
}

static void request_username_with_input(struct auth_data *data)
{
	if (data->user_required)
		purple_request_input(data->conn, TITLE_LABEL, USER_LABEL,
				     NULL, NULL, FALSE, FALSE, (gchar *) USER_FIELD,
				     _("OK"), G_CALLBACK(request_password_with_input),
				     CANCEL_LABEL, G_CALLBACK(send_credentials),
				     data->conn->account, NULL, NULL, data);
	else
		request_password_with_input(data, NULL);
}

void purple_request_credentials(PurpleConnection *conn, gpointer state, gboolean user_required)
{
	struct auth_data *data = g_new0(struct auth_data, 1);
	data->conn = conn;
	data->state = state;
	data->user_required = user_required;
	if (purple_request_get_ui_ops()->request_fields)
		request_credentials_with_fields(data);
	else
		request_username_with_input(data);
}
