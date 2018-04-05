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

#ifndef __CHIME_LOGIN_PRIVATE_H__
#define __CHIME_LOGIN_PRIVATE_H__

#include "chime.h"

struct login {
	SoupSession *session;
	ChimeConnection *connection;
	GDestroyNotify release_sub;
};

struct login_form {
	gchar *method;
	gchar *action;
	gchar *email_name;
	gchar *password_name;
	GHashTable *params;
	GDestroyNotify release;
};

gpointer chime_login_extend_state(gpointer data, gsize size, GDestroyNotify destroy);
void chime_login_free_state(struct login *state);

void chime_login_cancel_ui(struct login *state, gpointer foo);
void chime_login_cancel_cb(SoupSession *session, SoupMessage *msg, gpointer data);
void chime_login_token_cb(SoupSession *session, SoupMessage *msg, gpointer data);

void chime_login_request_failed(gpointer state, const gchar *location, SoupMessage *msg);
void chime_login_bad_response(gpointer state, const gchar *fmt, ...);


gchar *chime_login_parse_regex(SoupMessage *msg, const gchar *regex, guint group);
gchar **chime_login_parse_xpaths(SoupMessage *msg, guint count, ...);
GHashTable *chime_login_parse_json_object(SoupMessage *msg);
struct login_form *chime_login_parse_form(SoupMessage *msg, const gchar *form_xpath);

/* Each provider is implemented in a separate .c file */
void chime_login_amazon(SoupSession *session, SoupMessage *msg, gpointer data);
void chime_login_warpdrive(SoupSession *sessioin, SoupMessage *msg, gpointer data);

#define login_session(state)			\
	(((struct login *) (state))->session)
#define login_connection(state)			\
	(((struct login *) (state))->connection)
#define login_prplconn(state)			\
	((PurpleConnection *)login_connection(state)->prpl_conn)
#define login_account(state)			\
	(login_prplconn(state)->account)
#define login_account_email(state)		\
	(login_account(state)->username)

#define login_fail_on_error(msg, state)				\
	do {								\
		if (!SOUP_STATUS_IS_SUCCESSFUL((msg)->status_code)) {	\
			chime_login_request_failed((state), G_STRLOC, (msg)); \
			return;						\
		}							\
	} while (0)

#define login_free_form(form)				\
	g_clear_pointer(&(form), (form)->release)

#endif  /* __CHIME_LOGIN_PRIVATE_H__ */
