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

#include <debug.h>
#include <glib/gi18n.h>
#include <request.h>

#include "login-private.h"

#define WARPDRIVE_INTERFACE  "com.amazonaws.warpdrive.console.client.GalaxyInternalGWTService"
#define GWT_BOOTSTRAP  "//script[contains(@src, '/WarpDriveLogin/')][1]/@src"
#define GWT_ID_REGEX  "['\"]([A-Z0-9]{30,35})['\"]"
#define GWT_RPC_PATH  "WarpDriveLogin/GalaxyInternalService"
#define USER_FIELD  "username"
#define PASS_FIELD  "password"
#define AUTH_TITLE  _("Corporate Login")
#define AUTH_FAIL  _("Authentication failed")
#define AUTH_SEND  _("Sign In")
#define AUTH_CANCEL  _("Cancel")

#define discovery_failure(state, label)					\
	do {								\
		purple_debug_error("chime", "%s: %s", G_STRLOC, (label)); \
		chime_login_bad_response((state), _("Error during corporate login setup")); \
	} while (0)

struct login_wd {
	struct login b;  /* Base */
	gchar *directory;
	gchar *client_id;
	gchar *redirect_url;
	gchar *region;
	gchar *username;
	/* GWT-RPC specific parameters */
	SoupURI *gwt_rpc_uri;
	gchar *gwt_module_base;
	gchar *gwt_permutation;
	gchar *gwt_policy;
};

/* Break dependency loop */
static void request_credentials(struct login_wd *state, gboolean retry);

static void free_wd_state(struct login_wd *state)
{
	g_free(state->directory);
	g_free(state->client_id);
	g_free(state->redirect_url);
	g_free(state->region);
	g_free(state->username);
	soup_uri_free(state->gwt_rpc_uri);
	g_free(state->gwt_module_base);
	g_free(state->gwt_permutation);
	g_free(state->gwt_policy);
}

static gchar *escape_backslash(const gchar *src)
{
	GString *dst = g_string_new("");
	guint i = 0;

	for (i = 0;  src[i] != '\0';  i++) {
		g_string_append_c(dst, src[i]);
		if (src[i] == '\\')
			g_string_append_c(dst, '\\');
	}
	return g_string_free(dst, FALSE);
}

static void set_username(struct login_wd *state, const gchar *username)
{
	if (state->username)
		g_free(state->username);

	state->username = escape_backslash(username);
}

/*
 * Compose a GWT-RPC request.  For more information about how these requests are
 * formatted, check the following links:
 *
 * https://docs.google.com/document/d/1eG0YocsYYbNAtivkLtcaiEE5IOF5u4LUol8-LL0TIKU
 * http://www.gdssecurity.com/l/b/page/2/
 */
static SoupMessage *gwt_request(struct login_wd *state,
				const gchar *interface,
				const gchar *method,
				guint field_count, ...)
{
	GHashTable *strings = g_hash_table_new(g_str_hash, g_str_equal);
	GHashTableIter iterator;
	GString *body = g_string_new("7|0|");
	SoupMessage *msg;
	const gchar **table;
	gpointer value, index;
	gulong i, sc = 0;  /* sc: strings count */
	va_list fields;

	/* Populate the strings table */
	g_hash_table_insert(strings, state->gwt_module_base, (gpointer) ++sc);
	g_hash_table_insert(strings, state->gwt_policy, (gpointer) ++sc);
	g_hash_table_insert(strings, (gchar *) interface, (gpointer) ++sc);
	g_hash_table_insert(strings, (gchar *) method, (gpointer) ++sc);
	va_start(fields, field_count);
	for (i = 0;  i < field_count;  i++) {
		gchar *field = va_arg(fields, gchar *);
		if (field && !g_hash_table_contains(strings, field))
			g_hash_table_insert(strings, field, (gpointer) ++sc);
	}
	va_end(fields);
	/* Write the strings table, sorted by table index */
	g_string_append_printf(body, "%lu|", sc);
	table = g_new(const gchar *, sc);
	g_hash_table_iter_init(&iterator, strings);
	while (g_hash_table_iter_next(&iterator, &value, &index))
		table[((gulong) index) - 1] = value;
	for (i = 0;  i < sc;  i++)
		g_string_append_printf(body, "%s|", table[i]);
	g_free(table);
	/* Now add the request components, by their table index (NULLs are
	   converted to zeroes) */
	g_string_append_printf(body, "%lu|", (gulong)
			       g_hash_table_lookup(strings, state->gwt_module_base));
	g_string_append_printf(body, "%lu|", (gulong)
			       g_hash_table_lookup(strings, state->gwt_policy));
	g_string_append_printf(body, "%lu|", (gulong)
			       g_hash_table_lookup(strings, interface));
	g_string_append_printf(body, "%lu|", (gulong)
			       g_hash_table_lookup(strings, method));
	g_string_append(body, "1|");  /* Argument count, only 1 supported */
	va_start(fields, field_count);
	for (i = 0;  i < field_count;  i++) {
		gchar *field = va_arg(fields, gchar *);
		if (field)
			g_string_append_printf(body, "%lu|", (gulong)
					       g_hash_table_lookup(strings, field));
		else
			g_string_append(body, "0|");
	}
	va_end(fields);
	/* The request body is ready, now add the headers */
	msg = soup_message_new_from_uri(SOUP_METHOD_POST, state->gwt_rpc_uri);
	soup_message_set_request(msg, "text/x-gwt-rpc; charset=utf-8",
				 SOUP_MEMORY_TAKE, body->str, body->len);
	soup_message_headers_append(msg->request_headers, "X-GWT-Module-Base",
				    state->gwt_module_base);
	soup_message_headers_append(msg->request_headers, "X-GWT-Permutation",
				    state->gwt_permutation);

	g_string_free(body, FALSE);
	g_hash_table_destroy(strings);
	return msg;
}

/*
 * Parse a GWT-RPC response, returning an array of strings, its length and the
 * success status.
 *
 * GWT-RPC responses have a very peculiar format.  For details, please check the
 * following links:
 *
 * https://docs.google.com/document/d/1eG0YocsYYbNAtivkLtcaiEE5IOF5u4LUol8-LL0TIKU
 * https://blog.gdssecurity.com/labs/2009/10/8/gwt-rpc-in-a-nutshell.html
 */
static gchar **parse_gwt(SoupMessage *msg, gboolean *ok, guint *count)
{
	GError *error = NULL;
	JsonArray *body, *strings;
	JsonNode *node;
	JsonParser *parser;
	const gchar *ctype;
	gchar **fields = NULL;
	guint i, length, max;

	*count = 0;
	ctype = soup_message_headers_get_content_type(msg->response_headers, NULL);
	if (g_strcmp0(ctype, "application/json") || !msg->response_body ||
	    msg->response_body->length < 5 ||  /* "//OK" or "//EX" */
	    !g_str_has_prefix(msg->response_body->data, "//")) {
		purple_debug_error("chime", "Unexpected GWT response format");
		return fields;
	}
	*ok = !strncmp(msg->response_body->data + 2, "OK", 2);

	/* Parse the JSON content */
	parser = json_parser_new();
	if (!json_parser_load_from_data(parser, msg->response_body->data + 4,
					msg->response_body-> length - 4, &error)) {
		purple_debug_error("chime", "GWT-JSON parsing error: %s", error->message);
		goto out;
	}
	/* Get the content array */
	node = json_parser_get_root(parser);
	if (!JSON_NODE_HOLDS_ARRAY(node)) {
		purple_debug_error("chime", "Unexpected GWT-JSON type %d", JSON_NODE_TYPE(node));
		goto out;
	}
	body = json_node_get_array(node);
	length = json_array_get_length(body);
	if (length < 4) {
		purple_debug_error("chime", "GWT response array length %d too short", length);
		goto out;
	}
	/* Get the strings table */
	length -= 3;
	node = json_array_get_element(body, length);
	if (!JSON_NODE_HOLDS_ARRAY(node)) {
		purple_debug_error("chime", "Could not find GWT response strings table");
		goto out;
	}
	strings = json_node_get_array(node);
	max = json_array_get_length(strings);
	/* Traverse the rest of the elements in reverse order, replacing the
	   indices by the (copied) string values in the result array */
	*count = length;
	fields = g_new0(gchar *, length + 1);
	for (i = 0;  i < length;  i++) {
		const gchar *value = NULL;
		gint64 j = json_array_get_int_element(body, length - i - 1);
		if (j > 0 && j <= max)
			value = json_array_get_string_element(strings, j - 1);
		fields[i] = g_strdup(value);
	}
 out:
	g_error_free(error);
	g_object_unref(parser);
	return fields;
}

static void gwt_auth_cb(SoupSession *session, SoupMessage *msg, gpointer data)
{
	SoupMessage *next;
	gboolean ok;
	gchar **response;
	guint count;
	struct login_wd *state = data;

	login_fail_on_error(msg, state);

	response = parse_gwt(msg, &ok, &count);
	if (!response) {
		purple_debug_error("chime", "NULL parsed GWT response during auth");
		chime_login_bad_response(state, _("Unexpected authentication failure"));
		return;
	}
	if (!ok) {
		if (count > 3 && !g_strcmp0(response[3], "AuthenticationFailedException"))
			request_credentials(state, TRUE);
		else
			chime_login_bad_response(state, _("Unexpected authentication failure"));
		goto out;
	}
	next = soup_form_request_new(SOUP_METHOD_GET, state->redirect_url,
				     "organization", state->directory,
				     "region", state->region,
				     "auth_code", response[2], NULL);
	soup_session_queue_message(session, next, chime_login_token_cb, state);
 out:
	g_strfreev(response);
}

static void send_credentials(struct login_wd *state, const gchar *password)
{
	SoupMessage *msg;
	gchar *escaped;
	static const gchar *type = "com.amazonaws.warpdrive.console.shared.LoginRequest_v4/3859384737";

	if (!(password && *password)) {
		request_credentials(state, TRUE);
		return;
	}

	escaped = escape_backslash(password);

	msg = gwt_request(state, WARPDRIVE_INTERFACE, "authenticateUser", 11,
			  type, type, "", "", state->client_id, "", NULL,
			  state->directory, escaped, "", state->username);

	soup_session_queue_message(login_session(state), msg, gwt_auth_cb, state);

	g_free(escaped);
}

static void gather_credentials_and_send(struct login_wd *state, PurpleRequestFields *fields)
{
	const gchar *username, *password;

	username = purple_request_fields_get_string(fields, USER_FIELD);
	password = purple_request_fields_get_string(fields, PASS_FIELD);
	if (!(username && *username && password && *password)) {
		request_credentials(state, TRUE);
		return;
	}

	set_username(state, username);
	send_credentials(state, password);
}

static void request_credentials_with_fields(struct login_wd *state, gboolean retry)
{
	PurpleRequestField *username, *password;
	PurpleRequestFieldGroup *group;
	PurpleRequestFields *fields;

	fields = purple_request_fields_new();
	group = purple_request_field_group_new(NULL);

	username = purple_request_field_string_new(USER_FIELD, _("Username"), NULL, FALSE);
	purple_request_field_set_required(username, TRUE);
	purple_request_field_group_add_field(group, username);

	password = purple_request_field_string_new(PASS_FIELD, _("Password"), NULL, FALSE);
	purple_request_field_string_set_masked(password, TRUE);
	purple_request_field_set_required(password, TRUE);
	purple_request_field_group_add_field(group, password);

	purple_request_fields_add_group(fields, group);

	purple_request_fields(login_connection(state)->prpl_conn, AUTH_TITLE,
			      _("Please sign in with your corporate credentials"),
			      retry ? AUTH_FAIL : NULL, fields,
			      AUTH_SEND, G_CALLBACK(gather_credentials_and_send),
			      AUTH_CANCEL, G_CALLBACK(chime_login_cancel_ui),
			      login_connection(state)->prpl_conn->account,
			      NULL, NULL, state);
}

static void request_password_with_input(struct login_wd *state, const gchar *username)
{
	if (!(username && *username)) {
		request_credentials(state, TRUE);
		return;
	}

	set_username(state, username);

	purple_request_input(login_connection(state)->prpl_conn, AUTH_TITLE,
			     _("Corporate password"), NULL,
			     NULL, FALSE, TRUE, (gchar *) "password",
			     AUTH_SEND, G_CALLBACK(send_credentials),
			     AUTH_CANCEL, G_CALLBACK(chime_login_cancel_ui),
			     login_connection(state)->prpl_conn->account,
			     NULL, NULL, state);
}

static void request_username_with_input(struct login_wd *state, gboolean retry)
{
	purple_request_input(login_connection(state)->prpl_conn, AUTH_TITLE,
			     _("Corporate username"), retry ? AUTH_FAIL : NULL,
			     NULL, FALSE, FALSE, NULL,
			     _("OK"), G_CALLBACK(request_password_with_input),
			     AUTH_CANCEL, G_CALLBACK(chime_login_cancel_ui),
			     login_connection(state)->prpl_conn->account,
			     NULL, NULL, state);
}

static void request_credentials(struct login_wd *state, gboolean retry)
{
	if (purple_request_get_ui_ops()->request_fields)
		request_credentials_with_fields(state, retry);
	else
		request_username_with_input(state, retry);
}

static void gwt_region_cb(SoupSession *session, SoupMessage *msg, gpointer data)
{
	gboolean ok;
	gchar **response;
	guint count;
	struct login_wd *state = data;

	login_fail_on_error(msg, state);

	response = parse_gwt(msg, &ok, &count);
	if (!response) {
		discovery_failure(state, "region response parsed NULL");
		return;
	}
	if (!ok) {
		discovery_failure(state, "GWT exception during region discovery");
		goto out;
	}

	state->region = g_strdup(response[count - 1]);
	if (!state->region) {
		discovery_failure(state, "NULL region value");
		goto out;
	}

	request_credentials(state, FALSE);
 out:
	g_strfreev(response);
}

static void gwt_policy_cb(SoupSession *session, SoupMessage *msg, gpointer data)
{
	SoupMessage *next;
	static const gchar *type = "com.amazonaws.warpdrive.console.shared.ValidateClientRequest_v2/2136236667";
	struct login_wd *state = data;

	login_fail_on_error(msg, state);

	state->gwt_policy = chime_login_parse_regex(msg, GWT_ID_REGEX, 1);
	if (!state->gwt_policy) {
		discovery_failure(state, "no GWT policy found");
		return;
	}

	next = gwt_request(state, WARPDRIVE_INTERFACE, "validateClient", 8,
			   type, type, "ONFAILURE", state->client_id,
			   state->directory, NULL, NULL, state->redirect_url);

	soup_session_queue_message(session, next, gwt_region_cb, state);
}

static void gwt_entry_point_cb(SoupSession *session, SoupMessage *msg, gpointer data)
{
	SoupMessage *next;
	SoupURI *base, *destination;
	gchar *policy_path;
	struct login_wd *state = data;

	login_fail_on_error(msg, state);

	state->gwt_permutation = chime_login_parse_regex(msg, GWT_ID_REGEX, 1);
	if (!state->gwt_permutation) {
		discovery_failure(state, "no GWT permutation found");
		return;
	}

	policy_path = g_strdup_printf("deferredjs/%s/5.cache.js",
				      state->gwt_permutation);
	base = soup_uri_new(state->gwt_module_base);
	destination = soup_uri_new_with_base(base, policy_path);

	next = soup_message_new_from_uri(SOUP_METHOD_GET, destination);
	soup_session_queue_message(session, next, gwt_policy_cb, state);

	soup_uri_free(destination);
	soup_uri_free(base);
	g_free(policy_path);
}

/*
 * Initial WD login scrapping.
 *
 * Ironically, most of the relevant data coming from this response is placed in
 * the GET parameters (both from the initial URL and the redirection).  From the
 * HTML body we only want the location of the GWT bootstrapping Javascript code.
 */
void chime_login_warpdrive(SoupSession *session, SoupMessage *msg, gpointer data)
{
	GHashTable *params;
	SoupMessage *next;
	SoupURI *initial, *base;
	gchar *sep, **gwt = NULL;
	struct login_wd *state;

	login_fail_on_error(msg, data);
	state = chime_login_extend_state(data, sizeof(struct login_wd),
					 (GDestroyNotify) free_wd_state);

	initial = soup_message_get_first_party(msg);
	params = soup_form_decode(soup_uri_get_query(initial));
	state->directory = g_strdup(g_hash_table_lookup(params, "directory"));
	if (!state->directory) {
		discovery_failure(state, "directory identifier not found");
		goto out;
	}

	g_hash_table_destroy(params);  /* Reuse the variable */
	base = soup_message_get_uri(msg);
	params = soup_form_decode(soup_uri_get_query(base));
	state->client_id = g_strdup(g_hash_table_lookup(params, "client_id"));
	state->redirect_url = g_strdup(g_hash_table_lookup(params, "redirect_uri"));
	if (!(state->client_id && state->redirect_url)) {
		discovery_failure(state, "client ID or callback missing");
		goto out;
	}
	state->gwt_rpc_uri = soup_uri_new_with_base(base, GWT_RPC_PATH);

	gwt = chime_login_parse_xpaths(msg, 1, GWT_BOOTSTRAP);
	if (!(gwt && gwt[0])) {
		discovery_failure(state, "JS bootstrap URL not found");
		goto out;
	}
	sep = strrchr(gwt[0], '/');
	state->gwt_module_base = g_strndup(gwt[0], (sep - gwt[0]) + 1);

	next = soup_message_new(SOUP_METHOD_GET, gwt[0]);
	soup_session_queue_message(session, next, gwt_entry_point_cb, state);
 out:
	g_strfreev(gwt);
	g_hash_table_destroy(params);
}
