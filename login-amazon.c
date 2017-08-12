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
#define PASS_TITLE  _("Amazon Login")
#define PASS_LABEL  _("Please enter the password for <%s>")
#define PASS_FAIL   _("Authentication failed")
#define PASS_OK  _("Sign In")
#define PASS_CANCEL  _("Cancel")

struct login_amzn {
	struct login b;  /* Base */
	struct login_form *form;
};

/* Break dependency loop */
static void request_credentials(struct login_amzn *state, gboolean retry);

static void clear_form(struct login_amzn *state)
{
	g_return_if_fail(state != NULL && state->form != NULL);
	login_free_form(state->form);
}

static void login_result_cb(SoupSession *session, SoupMessage *msg, gpointer data)
{
	struct login_amzn *state = data;
	struct login_form *form;

	login_fail_on_error(msg, state);

	/* Here the same HTML document is parsed several times, but this is
	   better than having a more complicated private API for the sake of
	   reducing CPU usage */
	form = chime_login_parse_form(msg, CONSENT_FORM);
	if (form) {
		SoupMessage *next;

		/* It appears that this is the first login ever, so the server
		   has presented a consent form;  we obviously say "yes" */
		g_hash_table_insert(form->params, g_strdup("consentApproved"), g_strdup(""));
		next = soup_form_request_new_from_hash(form->method,
						       form->action,
						       form->params);

		soup_session_queue_message(login_session(state), next, login_result_cb, state);

		login_free_form(form);
		return;
	}

	form = chime_login_parse_form(msg, SIGN_IN_FORM);
	if (form) {
		if (!(form->email_name && form->password_name)) {
			chime_login_bad_response(state, _("Could not find Amazon login form"));
			return;
		}
		/* Authentication failed */
		g_hash_table_insert(form->params,
				    g_strdup(form->email_name),
				    g_strdup(login_account_email(state)));
		state->form = form;
		request_credentials(state, TRUE);
		return;
	}

	chime_login_token_cb(session, msg, state);
}

static void send_credentials(struct login_amzn *state, const gchar *password)
{
	SoupMessage *msg;

	if (!(password && *password)) {
		request_credentials(state, TRUE);
		return;
	}

	g_hash_table_insert(state->form->params,
			    g_strdup(state->form->password_name),
			    g_strdup(password));

	msg = soup_form_request_new_from_hash(state->form->method,
					      state->form->action,
					      state->form->params);
	soup_session_queue_message(login_session(state), msg, login_result_cb, state);

	clear_form(state);
}

static void gather_credentials_and_send(struct login_amzn *state, PurpleRequestFields *fields)
{
	send_credentials(state, purple_request_fields_get_string(fields, PASS_FIELD));
}

static void request_credentials_with_fields(struct login_amzn *state, gboolean retry)
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
	text = g_strdup_printf(PASS_LABEL, login_account_email(state));

	purple_request_fields(login_connection(state)->prpl_conn,
			      PASS_TITLE, text, retry ? PASS_FAIL : NULL, fields,
			      PASS_OK, G_CALLBACK(gather_credentials_and_send),
			      PASS_CANCEL, G_CALLBACK(chime_login_cancel_ui),
			      login_connection(state)->prpl_conn->account,
			      NULL, NULL, state);
}

static void request_credentials_with_input(struct login_amzn *state, gboolean retry)
{
	gchar *text;

	text = g_strdup_printf(PASS_LABEL, login_account_email(state));

	purple_request_input(login_connection(state)->prpl_conn,
			     PASS_TITLE, text, retry ? PASS_FAIL : NULL, NULL,
			     FALSE, TRUE, (gchar *) "password",
			     PASS_OK, G_CALLBACK(send_credentials),
			     PASS_CANCEL, G_CALLBACK(chime_login_cancel_ui),
			     login_connection(state)->prpl_conn->account,
			     NULL, NULL, state);
}

static void request_credentials(struct login_amzn *state, gboolean retry)
{
	/* When loging in with Amazon, we only request a password.  Therefore we
	   may only use request_input.  However, request_fields provides a
	   better user experience, so we still prefer it */
	if (purple_request_get_ui_ops()->request_fields)
		request_credentials_with_fields(state, retry);
	else
		request_credentials_with_input(state, retry);
}

void chime_login_amazon(SoupSession *session, SoupMessage *msg, gpointer data)
{
	struct login_amzn *state;

	login_fail_on_error(msg, data);
	state = chime_login_extend_state(data, sizeof(struct login_amzn),
					 (GDestroyNotify) clear_form);

	state->form = chime_login_parse_form(msg, SIGN_IN_FORM);
	if (!(state->form && state->form->email_name && state->form->password_name)) {
		chime_login_bad_response(state, _("Could not find Amazon login form"));
		return;
	}

	g_hash_table_insert(state->form->params, g_strdup(state->form->email_name),
			    g_strdup(login_account_email(state)));
	request_credentials(state, FALSE);
}
