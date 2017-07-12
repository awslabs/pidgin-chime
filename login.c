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

#include <string.h>
#include <prpl.h>
#include <debug.h>
#include <request.h>
#include <glib/gi18n.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <libxml/HTMLparser.h>
#include <libxml/xpath.h>
#include <libxml/tree.h>

#include "chime.h"
#include "chime-connection-private.h"

/*
 * All the login processes require multiple requests, and in some cases there
 * are bits of information that will be (re)used in future steps.  Therefore, we
 * need to save some state.
 */
typedef struct chime_login {
	/* Common state */
	SoupSession *session;
	ChimeConnection *connection;
	gchar *email;
	/* WarpDrive state */
	gchar *wd_directory;
	gchar *wd_client_id;
	gchar *wd_redirect_url;
	gchar *wd_region;
	/* GWT-RPC parameters, used for WarpDrive */
	gchar *gwt_rpc_url;
	gchar *gwt_module_base;
	gchar *gwt_permutation;
	gchar *gwt_policy;
} ChimeLogin;

static void free_login_state(ChimeLogin *state)
{
	soup_session_abort(state->session);
	g_object_unref(state->session);
	g_object_unref(state->connection);
	g_free(state->email);
	g_free(state->wd_directory);
	g_free(state->wd_client_id);
	g_free(state->wd_redirect_url);
	g_free(state->wd_region);
	g_free(state->gwt_rpc_url);
	g_free(state->gwt_module_base);
	g_free(state->gwt_permutation);
	g_free(state->gwt_policy);
	g_free(state);
}

static void cancel_login(void *data, PurpleRequestFields *fields)
{
	free_login_state(data);
}

/*
 * Macro and function duet to handle unexpected network and content failures.
 * More frequent errors such as invalid credentials or unrecognized provider
 * should NOT be handled with these.
 */
#define login_fail(state, ...) do_login_fail((state), G_STRFUNC, __VA_ARGS__)

static void do_login_fail(ChimeLogin *state, const gchar *func, const gchar *format, ...)
{
	va_list args;
	gchar *message;

	va_start(args, format);
	message = g_strdup_vprintf(format, args);
	va_end(args);
	purple_debug_error("chime", "Login error at %s, %s", func, message);
	chime_connection_fail(state->connection, CHIME_ERROR_AUTH_FAILED,
			      _("Unexpected error during login"));
	g_free(message);
	free_login_state(state);
}

/*
 * XPath helpers.
 *
 * There is a lot of web scrapping involved in all the processes.  Therefore,
 * XPath is a key tool to search specific bits of information among all the HTML
 * text.
 */

static xmlNode **xpath_nodes(const gchar *expression, xmlXPathContext *ctx, guint *count)
{
	xmlXPathObject *results;
	xmlNode **nodes = NULL;

	*count = 0;
	results = xmlXPathEval(BAD_CAST expression, ctx);
	if (results == NULL)
		goto out;
	if (results->type == XPATH_NODESET) {
		*count = (guint) results->nodesetval->nodeNr;
		nodes = g_memdup(results->nodesetval->nodeTab,
				 results->nodesetval->nodeNr * sizeof(xmlNode *));
	}
	xmlXPathFreeObject(results);
 out:
	return nodes;
}

static gchar *xpath_string(const gchar *expression, xmlXPathContext *ctx)
{
	xmlXPathObject *results;
	gchar *wrapper, *value = NULL;

	wrapper = g_strdup_printf("string(%s)", expression);
	results = xmlXPathEval(BAD_CAST wrapper, ctx);
	if (results == NULL)
		goto out;
	if (results->type == XPATH_STRING)
		value = g_strdup((gchar *) results->stringval);
	xmlXPathFreeObject(results);
 out:
	g_free(wrapper);
	return value;

}

/*
 * SoupMessage response handling helpers.
 */

static xmlDoc *parse_html(SoupMessage *msg)
{
	GHashTable *params;
	SoupURI * uri;
	const gchar *ctype;
	gchar *url;
	xmlDoc *document = NULL;

	ctype = soup_message_headers_get_content_type(msg->response_headers, &params);
	if (g_strcmp0(ctype, "text/html") != 0 || !msg->response_body ||
	    msg->response_body->length <= 0)
		goto out;
	uri = soup_message_get_uri(msg);
	url = soup_uri_to_string(uri, FALSE);
	document = htmlReadMemory(msg->response_body->data,
				  msg->response_body->length,
				  url, g_hash_table_lookup(params, "charset"),
				  HTML_PARSE_NODEFDTD | HTML_PARSE_NOERROR |
				  HTML_PARSE_NOWARNING | HTML_PARSE_NONET |
				  HTML_PARSE_RECOVER);
	g_free(url);
 out:
	g_hash_table_destroy(params);
	return document;
}

/* Helper for the next function */
static void add_string_member(JsonObject *object, const gchar *member, JsonNode *node, gpointer table)
{
	if (JSON_NODE_HOLDS_VALUE(node))
		g_hash_table_insert(table, g_strdup(member),
				    g_strdup(json_node_get_string(node)));
}

static GHashTable *parse_json_object(SoupMessage *msg)
{
	const gchar *ctype;
	GError *error;
	JsonParser *parser;
	JsonNode *root;
	GHashTable *object = g_hash_table_new_full(g_str_hash, g_str_equal,
						   g_free, g_free);

	ctype = soup_message_headers_get_content_type(msg->response_headers, NULL);
	if (g_strcmp0(ctype, "application/json") != 0 || !msg->response_body ||
	    msg->response_body->length <= 0)
		return object;
	parser = json_parser_new();
	if (!json_parser_load_from_data(parser, msg->response_body->data,
					msg->response_body->length, &error)) {
		purple_debug_error("chime", "JSON parsing error at login: %s",
				   error->message);
		g_error_free(error);
		goto out;
	}
	root = json_parser_get_root(parser);
	if (!JSON_NODE_HOLDS_OBJECT(root))
		goto out;
	json_object_foreach_member(json_node_get_object(root),
				   add_string_member,
				   object);
 out:
	g_object_unref(parser);
	return object;
}

static gchar *parse_regex(SoupMessage *msg, const gchar *regex, guint group)
{
	GRegex *matcher;
	GMatchInfo *match;
	gchar *text = NULL;

	if (!msg->response_body || msg->response_body->length <= 0)
		return text;
	matcher = g_regex_new(regex, 0, 0, NULL);
	if (g_regex_match_full(matcher, msg->response_body->data,
			       msg->response_body->length, 0, 0, &match, NULL))
		text = g_match_info_fetch(match, group);
	g_match_info_free(match);
	g_regex_unref(matcher);
	return text;
}

/*
 * GWT-RPC responses have a very peculiar format.  For details, please check the
 * following links:
 *
 * https://docs.google.com/document/d/1eG0YocsYYbNAtivkLtcaiEE5IOF5u4LUol8-LL0TIKU
 * http://www.gdssecurity.com/l/b/page/2/
 */
static gchar **parse_gwt(SoupMessage *msg, gboolean *ok, guint *count)
{
	const gchar *ctype;
	JsonParser *parser;
	JsonNode *node;
	JsonArray *body, *strings;
	guint i, length, max;
	gchar **fields = NULL;

	*count = 0;
	ctype = soup_message_headers_get_content_type(msg->response_headers, NULL);
	if (g_strcmp0(ctype, "application/json") || !msg->response_body ||
	    msg->response_body->length < 5 ||  /* "//OK" or "//EX" */
	    !g_str_has_prefix(msg->response_body->data, "//"))
		return fields;
	*ok = strncmp(msg->response_body->data + 2, "OK", 2) == 0;

	/* Parse the JSON content */
	parser = json_parser_new();
	if (!json_parser_load_from_data(parser, msg->response_body->data + 4,
					msg->response_body-> length - 4, NULL))
		goto out;
	/* Get the content array */
	node = json_parser_get_root(parser);
	if (!JSON_NODE_HOLDS_ARRAY(node))
		goto out;
	body = json_node_get_array(node);
	length = json_array_get_length(body);
	if (length < 4)
		goto out;
	/* Get the strings table */
	length -= 3;
	node = json_array_get_element(body, length);
	if (!JSON_NODE_HOLDS_ARRAY(node))
		goto out;
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
	g_object_unref(parser);
	return fields;
}

/*
 * Other miscellaneous helpers
 */

static void add_input_node(xmlNode *node, GHashTable *form)
{
	xmlAttr *attribute;
	xmlChar *text;
	gchar *name, *value;

	attribute = xmlHasProp(node, BAD_CAST "name");
	if (attribute == NULL)
		return;
	text = xmlNodeGetContent((xmlNode *) attribute);
	name = g_strdup((gchar *) text);  /* Avoid mixing allocators */
	xmlFree(text);
	attribute = xmlHasProp(node, BAD_CAST "value");
	if (attribute != NULL) {
		text = xmlNodeGetContent((xmlNode *) attribute);
		value = g_strdup((gchar *) text);
		xmlFree(text);
	} else {
		value = g_strdup("");
	}
	g_hash_table_insert(form, name, value);
}

static gchar *resolve_url(SoupURI *base, const gchar *destination)
{
	SoupURI *final;
	gchar *url;

	final = soup_uri_new_with_base(base, (destination != NULL ? destination : ""));
	url = soup_uri_to_string(final, FALSE);
	soup_uri_free(final);
	return url;
}

/*
 * Compose a GWT-RPC request.  For more information about how these requests are
 * formatted, check the following links:
 *
 * https://docs.google.com/document/d/1eG0YocsYYbNAtivkLtcaiEE5IOF5u4LUol8-LL0TIKU
 * http://www.gdssecurity.com/l/b/page/2/
 */
static SoupMessage *gwt_request(ChimeLogin *state,
				const gchar *interface,
				const gchar *method,
				guint field_count, ...)
{
	va_list fields;
	const gchar **table;
	gpointer value, index;
	gulong i, sc = 0;  /* sc: strings count */
	SoupMessage *msg;
	GHashTableIter iterator;
	GHashTable *strings = g_hash_table_new(g_str_hash, g_str_equal);
	GString *body = g_string_new("7|0|");

	/* Populate the strings table */
	g_hash_table_insert(strings, state->gwt_module_base, (gpointer) ++sc);
	g_hash_table_insert(strings, state->gwt_policy, (gpointer) ++sc);
	g_hash_table_insert(strings, (gchar *) interface, (gpointer) ++sc);
	g_hash_table_insert(strings, (gchar *) method, (gpointer) ++sc);
	va_start(fields, field_count);
	for (i = 0;  i < field_count;  i++) {
		gchar *field = va_arg(fields, gchar *);
		if (field != NULL && !g_hash_table_contains(strings, field))
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
		if (field != NULL)
			g_string_append_printf(body, "%lu|", (gulong)
					       g_hash_table_lookup(strings, field));
		else
			g_string_append(body, "0|");
	}
	va_end(fields);
	/* The requesty body is ready, now add the headers */
	msg = soup_message_new(SOUP_METHOD_POST, state->gwt_rpc_url);
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
 * Login step callbacks.  Each function handles an HTTP response and queues the
 * next request.  Ownership of the login state structure is transferred from one
 * callback to the next.  Forward declarations are also used so the functions
 * can be defined in their natural sequence order.
 */

/* Common login sequence */
static void sign_in_page_cb          (SoupSession *session, SoupMessage *msg, gpointer data);
static void sign_in_search_result_cb (SoupSession *session, SoupMessage *msg, gpointer data);
static void sign_in_token_cb         (SoupSession *session, SoupMessage *msg, gpointer data);

/* WarpDrive provider sequence */
static void wd_login_page_cb         (SoupSession *session, SoupMessage *msg, gpointer data);
static void wd_gwt_entry_point_cb    (SoupSession *session, SoupMessage *msg, gpointer data);
static void wd_gwt_policy_cb         (SoupSession *session, SoupMessage *msg, gpointer data);
static void wd_gwt_region_cb         (SoupSession *session, SoupMessage *msg, gpointer data);
static void wd_request_credentials   (ChimeLogin *state, gboolean retry);
static void wd_send_credentials      (void *data, PurpleRequestFields *fields);
static void wd_gwt_auth_cb           (SoupSession *session, SoupMessage *msg, gpointer data);

/* Amazon provider sequence */
static void amazon_login_page_cb     (SoupSession *session, SoupMessage *msg, gpointer data);

#define FAIL_ON_ERROR(msg, state) do {					\
	if (!SOUP_STATUS_IS_SUCCESSFUL((msg)->status_code)) {		\
		login_fail((state), "response failed with code %d",	\
			   (msg)->status_code);				\
		return;							\
	}								\
} while (0)

static void sign_in_page_cb(SoupSession *session, SoupMessage *msg, gpointer data)
{
	ChimeLogin *state = data;
	xmlDoc *html;
	xmlXPathContext *ctx;
	xmlNode **inputs;
	guint i, n;
	gchar *action, *method, *email, *url;
	GHashTable *form;
	SoupMessage *next;

	FAIL_ON_ERROR(msg, state);

	html = parse_html(msg);
	if (html == NULL) {
		login_fail(state, "HTML parsing failed");
		return;
	}

	#define SEARCH_FORM "//form[@id='picker_email']"
	ctx = xmlXPathNewContext(html);
	action = xpath_string(SEARCH_FORM "/@action", ctx);
	method = xpath_string(SEARCH_FORM "/@method", ctx);
	email = xpath_string(SEARCH_FORM "//input[@type='email'][1]/@name", ctx);
	inputs = xpath_nodes(SEARCH_FORM "//input[@type='hidden']", ctx, &n);
	if (action == NULL || method == NULL || email == NULL) {
		login_fail(state, "could not find search form");
		goto out;
	}
	/* Upcase the method name, in place */
	for (i = 0;  method[i];  i++)
		method[i] = g_ascii_toupper(method[i]);

	form = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	g_hash_table_insert(form, g_strdup(email), g_strdup(state->email));
	for (i = 0;  i < n;  i++)
		add_input_node(inputs[i], form);

	url = resolve_url(soup_message_get_uri(msg), action);
	next = soup_form_request_new_from_hash(method, url, form);
	soup_session_queue_message(session, next, sign_in_search_result_cb, data);

	g_hash_table_destroy(form);
 out:
	g_free(inputs);
	g_free(email);
	g_free(method);
	g_free(action);
	xmlXPathFreeContext(ctx);
	xmlFreeDoc(html);
}

static void sign_in_search_result_cb(SoupSession *session, SoupMessage *msg, gpointer data)
{
	ChimeLogin *state = data;
	gchar *type, *path;
	GHashTable *provider;
	SoupURI *destination;
	SoupSessionCallback handler;
	SoupMessage *next;

	/* Bad request usually means invalid e-mail */
	if (msg->status_code == 400) {
		chime_connection_fail(state->connection, CHIME_ERROR_AUTH_FAILED,
				      _("Invalid email address \"%s\""), state->email);
		free_login_state(state);
		return;
	}

	FAIL_ON_ERROR(msg, state);

	provider = parse_json_object(msg);
	if (g_hash_table_size(provider) == 0) {
		login_fail(state, "no object parsed");
		goto out;
	}

	type = g_hash_table_lookup(provider, "provider");
	path = g_hash_table_lookup(provider, "path");
	if (type == NULL || path == NULL) {
		login_fail(state, "no provider information found");
		goto out;
	}

	if (!g_strcmp0(type, "amazon")) {
		handler = amazon_login_page_cb;
	} else if (!g_strcmp0(type, "wd")) {
		handler = wd_login_page_cb;
	} else {
		chime_connection_fail(state->connection, CHIME_ERROR_AUTH_FAILED,
				      _("Unknown login provider \"%s\""), type);
		free_login_state(state);
		goto out;
	}

	destination = soup_uri_new_with_base(soup_message_get_uri(msg), path);
	next = soup_message_new_from_uri(SOUP_METHOD_GET, destination);
	soup_message_set_first_party(next, destination);
	soup_session_queue_message(session, next, handler, data);
	soup_uri_free(destination);
 out:
	g_hash_table_destroy(provider);
}

static void sign_in_token_cb(SoupSession *session, SoupMessage *msg, gpointer data)
{
	ChimeLogin *state = data;
	gchar *token;

	FAIL_ON_ERROR(msg, state);

	token = parse_regex(msg, "['\"]chime://sso_sessions\\?Token=([^'\"]+)['\"]", 1);
	if (token == NULL) {
		login_fail(state, "expected token not found");
		goto out;
	}
	chime_connection_set_session_token(state->connection, token);
	chime_connection_connect(state->connection);
	free_login_state(state);
 out:
	g_free(token);
}

/*
 * WarpDrive login process.
 *
 * What makes this so convoluted is the usage of GWT-RPC, coded all in
 * Javascript, and that it has to be partly emulated in C.
 */

#define WARPDRIVE_INTERFACE "com.amazonaws.warpdrive.console.client.GalaxyInternalGWTService"
#define GWT_ID_REGEX "['\"]([A-Z0-9]{30,35})['\"]"
#define WD_USERNAME_FIELD "username"
#define WD_PASSWORD_FIELD "password"

/*
 * Initial WD login scrapping.
 *
 * Ironically, most of the relevant data coming from this response is placed in
 * the GET parameters (both from the inital URL and the redirection).  From the
 * HTML body we only want the location of the GWT bootstrapping Javascript code.
 */
static void wd_login_page_cb(SoupSession *session, SoupMessage *msg, gpointer data)
{
	ChimeLogin *state = data;
	SoupURI *initial, *base;
	GHashTable *params;
	xmlDoc *html;
	xmlXPathContext *ctx;
	gchar *gwt, *sep;
	SoupMessage *next;

	FAIL_ON_ERROR(msg, state);

	initial = soup_message_get_first_party(msg);
	params = soup_form_decode(soup_uri_get_query(initial));
	state->wd_directory = g_strdup(g_hash_table_lookup(params, "directory"));
	if (state->wd_directory == NULL) {
		login_fail(state, "directory identifier not found");
		goto out1;
	}

	g_hash_table_destroy(params);  /* Reuse the variable */
	base = soup_message_get_uri(msg);
	params = soup_form_decode(soup_uri_get_query(base));
	state->wd_client_id = g_strdup(g_hash_table_lookup(params, "client_id"));
	state->wd_redirect_url = g_strdup(g_hash_table_lookup(params, "redirect_uri"));
	if (state->wd_client_id == NULL || state->wd_redirect_url == NULL) {
		login_fail(state, "client ID or callback missing");
		goto out1;
	}
	state->gwt_rpc_url = resolve_url(base, "WarpDriveLogin/GalaxyInternalService");

	html = parse_html(msg);
	if (html == NULL) {
		login_fail(state, "HTML parsing failed");
		goto out1;
	}
	ctx = xmlXPathNewContext(html);
	gwt = xpath_string("//script[contains(@src, '/WarpDriveLogin/')][1]/@src", ctx);
	if (gwt == NULL) {
		login_fail(state, "could not find GWT bootstrap code");
		goto out2;
	}
	sep = strrchr(gwt, '/');
	state->gwt_module_base = g_strndup(gwt, (sep - gwt) + 1);

	next = soup_message_new(SOUP_METHOD_GET, gwt);
	soup_session_queue_message(session, next, wd_gwt_entry_point_cb, data);

	g_free(gwt);
 out2:
	xmlXPathFreeContext(ctx);
	xmlFreeDoc(html);
 out1:
	g_hash_table_destroy(params);
}

static void wd_gwt_entry_point_cb(SoupSession *session, SoupMessage *msg, gpointer data)
{
	ChimeLogin *state = data;
	gchar *policy_path;
	SoupURI *base, *destination;
	SoupMessage *next;

	FAIL_ON_ERROR(msg, state);

	state->gwt_permutation = parse_regex(msg, GWT_ID_REGEX, 1);
	if (state->gwt_permutation == NULL) {
		login_fail(state, "could not find any GWT permutation");
		return;
	}

	policy_path = g_strdup_printf("deferredjs/%s/5.cache.js",
				      state->gwt_permutation);
	base = soup_uri_new(state->gwt_module_base);
	destination = soup_uri_new_with_base(base, policy_path);
	next = soup_message_new_from_uri(SOUP_METHOD_GET, destination);
	soup_session_queue_message(session, next, wd_gwt_policy_cb, data);

	soup_uri_free(destination);
	soup_uri_free(base);
	g_free(policy_path);
}

static void wd_gwt_policy_cb(SoupSession *session, SoupMessage *msg, gpointer data)
{
	static const gchar *type = "com.amazonaws.warpdrive.console.shared.ValidateClientRequest_v2/2136236667";
	ChimeLogin *state = data;
	SoupMessage *next;

	state->gwt_policy = parse_regex(msg, GWT_ID_REGEX, 1);
	if (state->gwt_policy == NULL) {
		login_fail(state, "could not find any GWT policy");
		return;
	}

	next = gwt_request(state, WARPDRIVE_INTERFACE, "validateClient", 8,
			   type, type, "ONFAILURE", state->wd_client_id,
			   state->wd_directory, NULL, NULL, state->wd_redirect_url);

	soup_session_queue_message(session, next, wd_gwt_region_cb, data);
}

static void wd_gwt_region_cb(SoupSession *session, SoupMessage *msg, gpointer data)
{
	ChimeLogin *state = data;
	gboolean ok;
	guint count;
	gchar **response;

	FAIL_ON_ERROR(msg, state);

	response = parse_gwt(msg, &ok, &count);
	if (response == NULL) {
		login_fail(state, "unable to parse GWT response");
		return;
	}
	if (!ok) {
		login_fail(state, "GWT exception occurred");
		goto out;
	}

	state->wd_region = g_strdup(response[count - 1]);
	if (state->wd_region == NULL) {
		login_fail(state, "could not retrieve the AWS region");
		goto out;
	}

	wd_request_credentials(state, FALSE);
 out:
	g_strfreev(response);
}

static void wd_request_credentials(ChimeLogin *state, gboolean retry)
{
	PurpleRequestFields *fields;
	PurpleRequestFieldGroup *group;
	PurpleRequestField *username, *password;

	fields = purple_request_fields_new();
	group = purple_request_field_group_new(NULL);

	username = purple_request_field_string_new(WD_USERNAME_FIELD, _("Username"), NULL, FALSE);
	purple_request_field_set_required(username, TRUE);
	purple_request_field_group_add_field(group, username);

	password = purple_request_field_string_new(WD_PASSWORD_FIELD, _("Password"), NULL, FALSE);
	purple_request_field_string_set_masked(password, TRUE);
	purple_request_field_set_required(password, TRUE);
	purple_request_field_group_add_field(group, password);

	purple_request_fields_add_group(fields, group);

	purple_request_fields(state->connection->prpl_conn->account,
			      _("Corporate Login"),
			      _("Please sign in with your corporate credentials"),
			      retry ? _("Authentication failed") : NULL,
			      fields,
			      _("Sign In"), G_CALLBACK(wd_send_credentials),
			      _("Cancel"), G_CALLBACK(cancel_login),
			      NULL, NULL, NULL, state);
}

static void wd_send_credentials(void *data, PurpleRequestFields *fields)
{
	static const gchar *type = "com.amazonaws.warpdrive.console.shared.LoginRequest_v4/3859384737";
	ChimeLogin *state = data;
	const gchar *username, *password;
	SoupMessage *msg;

	username = purple_request_fields_get_string(fields, WD_USERNAME_FIELD);
	password = purple_request_fields_get_string(fields, WD_PASSWORD_FIELD);

	msg = gwt_request(state, WARPDRIVE_INTERFACE, "authenticateUser", 11,
			  type, type, "", "", state->wd_client_id, "", NULL,
			  state->wd_directory, password, "", username);

	soup_session_queue_message(state->session, msg, wd_gwt_auth_cb, data);
}

static void wd_gwt_auth_cb(SoupSession *session, SoupMessage *msg, gpointer data)
{
	ChimeLogin *state = data;
	gboolean ok;
	guint count;
	gchar **response;
	SoupMessage *next;

	FAIL_ON_ERROR(msg, state);

	response = parse_gwt(msg, &ok, &count);
	if (response == NULL) {
		login_fail(state, "unable to parse GWT response");
		return;
	}
	if (!ok) {
		if (count > 3 && !g_strcmp0(response[3], "AuthenticationFailedException"))
			wd_request_credentials(state, TRUE);
		else
			login_fail(state, "unexpected authentication failure");
		goto out;
	}
	next = soup_form_request_new(SOUP_METHOD_GET, state->wd_redirect_url,
				     "organization", state->wd_directory,
				     "region", state->wd_region,
				     "auth_code", response[2], NULL);
	soup_session_queue_message(session, next, sign_in_token_cb, data);
 out:
	g_strfreev(response);
}

/*
 * Amazon login process.
 *
 * TODO
 */
static void amazon_login_page_cb(SoupSession *session, SoupMessage *msg, gpointer data)
{
	ChimeLogin *state = data;

	chime_connection_fail(state->connection, CHIME_ERROR_AUTH_FAILED,
			      _("Amazon login not supported yet"));
	free_login_state(state);
}

void chime_initial_login(ChimeConnection *cxn)
{
	ChimeLogin *state;
	ChimeConnectionPrivate *priv;
	SoupMessage *msg;

	g_return_if_fail(CHIME_IS_CONNECTION(cxn));

	state = g_new0(ChimeLogin, 1);
	state->connection = g_object_ref(cxn);
	state->email = g_strdup(cxn->prpl_conn->account->username);
	state->session = soup_session_new_with_options(SOUP_SESSION_ADD_FEATURE_BY_TYPE,
						       SOUP_TYPE_COOKIE_JAR, NULL);
	/* TODO: This needs to go somewhere else */
	if (getenv("CHIME_DEBUG") && atoi(getenv("CHIME_DEBUG")) > 0) {
		SoupLogger *l = soup_logger_new(SOUP_LOGGER_LOG_BODY, -1);
		soup_session_add_feature(state->session, SOUP_SESSION_FEATURE(l));
		g_object_unref(l);
	}

	priv = CHIME_CONNECTION_GET_PRIVATE(cxn);
	msg = soup_message_new(SOUP_METHOD_GET, priv->server);
	soup_session_queue_message(state->session, msg, sign_in_page_cb, state);
}
