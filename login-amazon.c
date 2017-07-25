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

#include "login-private.h"

#define SIGN_IN_FORM  "//form[@id='ap_signin_form']"
#define CONSENT_FORM  "//form[@name='consent-form']"
#define PASS_FIELD  "password"

struct chime_login_amzn {
	struct chime_login b;  /* Base */
	gchar *form_method;
	gchar *form_action;
	gchar *email_name;
	gchar *password_name;
	GHashTable *params;
};

/* Break dependency loop */
static void request_credentials(struct chime_login_amzn *state, gboolean retry);

static void clear_form(struct chime_login_amzn *state)
{
	g_clear_pointer(&state->form_method, g_free);
	g_clear_pointer(&state->form_action, g_free);
	g_clear_pointer(&state->email_name, g_free);
	g_clear_pointer(&state->password_name, g_free);
	g_clear_pointer(&state->params, g_hash_table_destroy);
}

static void send_consent(struct chime_login_amzn *state, gint choice)
{
	SoupMessage *msg;
	SoupSessionCallback handler;
	const gchar *action;

	if (choice == 1) {
		action = "consentApproved";
		handler = chime_login_token_cb;
	} else {
		action = "consentDenied";
		handler = chime_login_cancel_cb;
	}
	g_hash_table_insert(state->params, g_strdup(action), g_strdup(""));

	msg = soup_form_request_new_from_hash(state->form_method, state->form_action, state->params);
	soup_session_queue_message(chime_login_session(state), msg, handler, state);

	clear_form(state);
}

static void request_consent(struct chime_login_amzn *state)
{
	gchar *text;

	text = g_strdup_printf(_("Do you want to register %s into AWS Chime?"),
			       chime_login_account_email(state));
	purple_request_ok_cancel(chime_login_connection(state)->prpl_conn,
				 _("Confirm Registration"), text, NULL, 0,
				 chime_login_connection(state)->prpl_conn->account,
				 NULL, NULL, state,
				 G_CALLBACK(send_consent), G_CALLBACK(send_consent));
	g_free(text);
}

static void login_result_cb(SoupSession *session, SoupMessage *msg, gpointer data)
{
	struct chime_login_amzn *state = data;

	chime_login_fail_on_error(msg, state);

	state->params = chime_login_parse_form(msg, CONSENT_FORM, &state->form_method,
					       &state->form_action, NULL, NULL);
	if (state->params) {
		request_consent(state);
		return;
	}

	state->params = chime_login_parse_form(msg, SIGN_IN_FORM, &state->form_method,
					       &state->form_action, &state->email_name,
					       &state->password_name);
	if (state->params) {
		/* Authentication failed */
		if (!(state->email_name && state->password_name)) {
			chime_login_bad_response(state, _("Could not find Amazon login form"));
			return;
		}
		g_hash_table_insert(state->params, g_strdup(state->email_name),
				    g_strdup(chime_login_account_email(state)));
		request_credentials(state, TRUE);
		return;
	}

	chime_login_token_cb(session, msg, state);
}

static void send_credentials(struct chime_login_amzn *state, PurpleRequestFields *fields)
{
	SoupMessage *msg;
	const gchar *password;

	password = purple_request_fields_get_string(fields, PASS_FIELD);
	g_hash_table_insert(state->params, g_strdup(state->password_name), g_strdup(password));

	msg = soup_form_request_new_from_hash(state->form_method, state->form_action, state->params);
	soup_session_queue_message(chime_login_session(state), msg, login_result_cb, state);

	clear_form(state);
}

static void request_credentials(struct chime_login_amzn *state, gboolean retry)
{
	PurpleRequestField *password;
	PurpleRequestFieldGroup *group;
	PurpleRequestFields *fields;
	gchar *text;

	fields = purple_request_fields_new();
	group = purple_request_field_group_new(NULL);

	password = purple_request_field_string_new(PASS_FIELD, _("Password"), NULL, FALSE);
	purple_request_field_string_set_masked(password, TRUE);
	purple_request_field_set_required(password, TRUE);
	purple_request_field_group_add_field(group, password);

	purple_request_fields_add_group(fields, group);
	text = g_strdup_printf(_("Please enter the password for %s"),
			       chime_login_account_email(state));
	purple_request_fields(chime_login_connection(state)->prpl_conn,
			      _("Amazon Login"), text,
			      retry ? _("Authentication failed") : NULL,
			      fields,
			      _("Sign In"), G_CALLBACK(send_credentials),
			      _("Cancel"), G_CALLBACK(chime_login_cancel_ui),
			      chime_login_connection(state)->prpl_conn->account,
			      NULL, NULL, state);
}

void chime_login_amazon(SoupSession *session, SoupMessage *msg, gpointer data)
{
	struct chime_login_amzn *state;

	chime_login_fail_on_error(msg, data);
	state = chime_login_extend_state(data, sizeof(struct chime_login_amzn),
					 (GDestroyNotify) clear_form);

	state->params = chime_login_parse_form(msg, SIGN_IN_FORM, &state->form_method,
					       &state->form_action, &state->email_name,
					       &state->password_name);

	if (!(state->params && state->email_name && state->password_name)) {
		chime_login_bad_response(state, _("Could not find Amazon login form"));
		return;
	}

	g_hash_table_insert(state->params, g_strdup(state->email_name),
			    g_strdup(chime_login_account_email(state)));
	request_credentials(state, FALSE);
}
