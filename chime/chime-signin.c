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

/*
 * The sign-in process in the official clients is handled by a web view widget,
 * just as if the user was signin into a web application.  We don't have a fully
 * blown embedded web browser to delegate on, therefore we need to implement
 * some web scrapping.
 *
 * OVERVIEW OF THE SIGN-IN PROCESS
 *
 * The initial login page presents a search form with a single input field: the
 * e-mail address.  This form is submitted by an AJAX request that expects a
 * JSON response indicating the auth provider to use and its entry point.  Two
 * different providers are recognized here: "amazon" and "wd" (WarpDrive).
 *
 * The Amazon provider is purely web based.  So by following HTTP re-directions,
 * tracking cookies and scrapping HTML forms (with hidden inputs) is enough.
 *
 * The WarpDrive provider implements ActiveDirectory based authentication over
 * the web.  Unfortunately, the final password submission is sent over GWT-RPC.
 * A GWT-RPC message requires a number of parameters that need to be discovered
 * by means of extra HTTP requests.  Therefore, this module includes a minimal
 * implementation of GWT-RPC based on the following documents:
 *
 * https://docs.google.com/document/d/1eG0YocsYYbNAtivkLtcaiEE5IOF5u4LUol8-LL0TIKU
 * https://blog.gdssecurity.com/labs/2009/10/8/gwt-rpc-in-a-nutshell.html
 *
 * Once the password has been sent (whatever the provider is), the server will
 * return an HTML response containing the session token as a "chime://" URI.
 * When present, it will signal the end of a successful authentication and the
 * token will allow this plugin to talk to all the necessary services.
 *
 * The pillars for the success of this implementation are:
 *
 *   - SOUP: HTTP re-direction handling and cookie tracking
 *   - LibXML2: HTML parsing and DOM navigation
 *   - JSON-GLib: JSON parsing
 *   - GLib: everything else
 */

#include <glib/gi18n.h>
#include <libxml/HTMLparser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>

#include "chime-connection-private.h"

#define GWT_ID_REGEX  "['\"]([A-Z0-9]{30,35})['\"]"  /* Magic */
#define WARPDRIVE_INTERFACE  "com.amazonaws.warpdrive.console.client.GalaxyInternalGWTService"

/* A parsed and navigable HTML document */
struct dom {
	xmlDoc *document;
	xmlXPathContext *context;
};

/* A scrapped form */
struct form {
	gchar *referer;
	gchar *method;
	gchar *action;
	gchar *email_name;
	gchar *password_name;
	GHashTable *params;
};

/* Sign-in process state information */
struct signin {
	ChimeConnection *connection;
	SoupSession *session;
	gchar *email;
	/* Amazon provider state fields */
	struct form *form;
	/* WarpDrive provider state fields */
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

static void free_dom(struct dom *d)
{
	if (!d) return;
	xmlXPathFreeContext(d->context);
	xmlFreeDoc(d->document);
	g_free(d);
}

static void free_form(struct form *f)
{
	if (!f) return;
	g_free(f->referer);
	g_free(f->method);
	g_free(f->action);
	g_free(f->email_name);
	g_free(f->password_name);
	g_hash_table_destroy(f->params);
	g_free(f);
}

static void free_signin(struct signin *s)
{
	if (!s) return;
	g_free(s->email);
	g_free(s->gwt_policy);
	g_free(s->gwt_permutation);
	g_free(s->gwt_module_base);
	soup_uri_free(s->gwt_rpc_uri);
	g_free(s->username);
	g_free(s->region);
	g_free(s->redirect_url);
	g_free(s->client_id);
	g_free(s->directory);
	free_form(s->form);
	soup_session_abort(s->session);
	g_object_unref(s->session);
	g_object_unref(s->connection);
}

static void fail(struct signin *state, GError *error)
{
	g_assert(state != NULL && error != NULL);

	chime_debug("Sign-in failure: %s\n", error->message);
	chime_connection_fail_error(state->connection, error);
	g_error_free(error);
	free_signin(state);
}

static void fail_bad_response(struct signin *state, const gchar *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	fail(state, g_error_new_valist(CHIME_ERROR, CHIME_ERROR_BAD_RESPONSE, fmt, args));
	va_end(args);
}

static void fail_response_error(struct signin *state, const gchar *location, SoupMessage *msg)
{
	chime_debug("Server returned error %d %s (%s)\n", msg->status_code, msg->reason_phrase, location);
	fail(state, g_error_new(CHIME_ERROR, CHIME_ERROR_REQUEST_FAILED,
				_("A request failed during sign-in")));
}



#define fail_on_response_error(msg, state)				\
	do {								\
		if (!SOUP_STATUS_IS_SUCCESSFUL((msg)->status_code)) {	\
			fail_response_error((state), G_STRLOC, (msg));	\
			return;						\
		}							\
	} while (0)

#define fail_gwt_discovery(state, ...)					\
	do {								\
		chime_debug(__VA_ARGS__);				\
		fail_bad_response((state), _("Error during corporate authentication setup")); \
	} while (0)

static void cancel_signin(struct signin *state)
{
	fail(state, g_error_new(CHIME_ERROR, CHIME_ERROR_AUTH_FAILED,
				_("Sign-in canceled by the user")));
}

static void cancel_signin_cb(SoupSession *session, SoupMessage *msg, gpointer data)
{
	cancel_signin(data);
}

/*
 * XPath helpers to simplify querying the DOM of a parsed HTML response.
 */

static gboolean xpath_exists(struct dom *dom, const gchar *fmt, ...)
{
	gboolean found;
	gchar *expression;
	va_list args;
	xmlXPathObject *results;

	if (!dom)
		return FALSE;

	va_start(args, fmt);
	expression = g_strdup_vprintf(fmt, args);
	va_end(args);
	results = xmlXPathEval(BAD_CAST expression, dom->context);
	found = results && results->type == XPATH_NODESET &&
		results->nodesetval && results->nodesetval->nodeNr > 0;
	xmlXPathFreeObject(results);
	g_free(expression);
	return found;
}

static xmlNode **xpath_nodes(struct dom *dom, guint *count, const char *fmt, ...)
{
	gchar *expression;
	va_list args;
	xmlNode **nodes;
	xmlXPathObject *results;

	if (!dom)
		return NULL;

	va_start(args, fmt);
	expression = g_strdup_vprintf(fmt, args);
	va_end(args);
	results = xmlXPathEval(BAD_CAST expression, dom->context);
	if (results && results->type == XPATH_NODESET && results->nodesetval) {
		*count = (guint) results->nodesetval->nodeNr;
		nodes = g_memdup(results->nodesetval->nodeTab,
				 results->nodesetval->nodeNr * sizeof(xmlNode *));
	} else {
		*count = 0;
		nodes = NULL;
	}
	xmlXPathFreeObject(results);
	g_free(expression);
	return nodes;
}

static gchar *xpath_string(struct dom *dom, const gchar *fmt, ...)
{
	gchar *expression, *wrapped, *value = NULL;
	va_list args;
	xmlXPathObject *results;

	if (!dom)
		return NULL;

	va_start(args, fmt);
	expression = g_strdup_vprintf(fmt, args);
	va_end(args);
	wrapped = g_strdup_printf("string(%s)", expression);
	results = xmlXPathEval(BAD_CAST wrapped, dom->context);
	if (results && results->type == XPATH_STRING)
		value = g_strdup((gchar *) results->stringval);
	xmlXPathFreeObject(results);
	g_free(wrapped);
	g_free(expression);
	return value;
}

/*
 * Convenience helper to scrap an HTML form with the necessary information for
 * later submission.  This is something we are going to need repeatedly.
 */
static struct form *scrap_form(struct dom *dom, SoupURI *action_base, const gchar *form_xpath)
{
	gchar *action;
	guint i, n;
	struct form *form = NULL;
	xmlNode **inputs;

	if (!xpath_exists(dom, form_xpath)) {
		chime_debug("XPath query returned no results: %s\n", form_xpath);
		return form;
	}

	form = g_new0(struct form, 1);

	form->referer = soup_uri_to_string(action_base, FALSE);
	form->method = xpath_string(dom, "%s/@method", form_xpath);
	if (form->method) {
		for (i = 0;  form->method[i] != '\0';  i++)
			form->method[i] = g_ascii_toupper(form->method[i]);
	} else {
		form->method = g_strdup(SOUP_METHOD_GET);
	}

	action = xpath_string(dom, "%s/@action", form_xpath);
	if (action) {
		SoupURI *dst = soup_uri_new_with_base(action_base, action);
		form->action = soup_uri_to_string(dst, FALSE);
		soup_uri_free(dst);
	} else {
		form->action = soup_uri_to_string(action_base, FALSE);
	}

	form->email_name = xpath_string(dom, "%s//input[@type='email'][1]/@name", form_xpath);
	form->password_name = xpath_string(dom, "%s//input[@type='password'][1]/@name", form_xpath);

	form->params = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	inputs = xpath_nodes(dom, &n, "%s//input[@type='hidden']", form_xpath);
	for (i = 0;  i < n;  i++) {
		gchar *name, *value;
		xmlChar *text;
		xmlAttr *attribute = xmlHasProp(inputs[i], BAD_CAST "name");
		if (!attribute)
			continue;
		text = xmlNodeGetContent((xmlNode *) attribute);
		name = g_strdup((gchar *) text);  /* Avoid mixing allocators */
		xmlFree(text);
		attribute = xmlHasProp(inputs[i], BAD_CAST "value");
		if (attribute) {
			text = xmlNodeGetContent((xmlNode *) attribute);
			value = g_strdup((gchar *) text);
			xmlFree(text);
		} else {
			value = g_strdup("");
		}
		g_hash_table_insert(form->params, name, value);
	}

	g_free(inputs);
	g_free(action);
	return form;
}

static gchar *escaped(const gchar *src)
{
	GString *dst = g_string_new("");
	guint i = 0;

	for (i = 0;  src[i] != '\0';  i++)
		switch (src[i]) {
		case '\\':
			g_string_append(dst, "\\\\");
			break;
		case '|':
			/* GWT escapes the pipe character with backslash exclamation */
			g_string_append(dst, "\\!");
			break;
		default:
			g_string_append_c(dst, src[i]);
		}
	return g_string_free(dst, FALSE);
}

/*
 * Helper function to compose a GWT-RPC request.  See the comments at the top of
 * the file for more information about GWT-RPC reverse engineering.
 */
static SoupMessage *gwt_request(struct signin *state,
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
 * SoupMessage response parsing helpers.
 */

static struct dom *parse_html(SoupMessage *msg)
{
	GHashTable *params;
	const gchar *ctype;
	gchar *url = NULL;
	struct dom *dom = NULL;
	xmlDoc *document;
	xmlXPathContext *context;

	ctype = soup_message_headers_get_content_type(msg->response_headers, &params);
	if (g_strcmp0(ctype, "text/html") || !msg->response_body ||
	    msg->response_body->length <= 0) {
		chime_debug("Empty HTML response or unexpected content %s\n", ctype);
		goto out;
	}

	url = soup_uri_to_string(soup_message_get_uri(msg), FALSE);
	document = htmlReadMemory(msg->response_body->data,
				  msg->response_body->length,
				  url, g_hash_table_lookup(params, "charset"),
				  HTML_PARSE_NODEFDTD | HTML_PARSE_NOERROR |
				  HTML_PARSE_NOWARNING | HTML_PARSE_NONET |
				  HTML_PARSE_RECOVER);
	if (!document) {
		chime_debug("Failed to parse HTML\n");
		goto out;
	}

	context = xmlXPathNewContext(document);
	if (!context) {
		chime_debug("Failed to create XPath context\n");
		xmlFreeDoc(document);
		goto out;
	}

	dom = g_new0(struct dom, 1);
	dom->document = document;
	dom->context = context;
 out:
	g_free(url);
	g_hash_table_destroy(params);
	return dom;
}

static GHashTable *parse_json(SoupMessage *msg)
{
	GError *error = NULL;
	GHashTable *result = NULL;
	GList *members, *member;
	JsonNode *node;
	JsonObject *object;
	JsonParser *parser;
	const gchar *ctype;

	ctype = soup_message_headers_get_content_type(msg->response_headers, NULL);
	if (g_strcmp0(ctype, "application/json") || !msg->response_body ||
	    msg->response_body->length <= 0) {
		chime_debug("Empty JSON response or unexpected content %s\n", ctype);
		return result;
	}

	parser = json_parser_new();
	if (!json_parser_load_from_data(parser, msg->response_body->data,
					msg->response_body->length, &error)) {
		chime_debug("JSON parsing error: %s\n", error->message);
		goto out;
	}

	node = json_parser_get_root(parser);
	if (!JSON_NODE_HOLDS_OBJECT(node)) {
		chime_debug("Unexpected JSON type %d\n", JSON_NODE_TYPE(node));
		goto out;
	}

	object = json_node_get_object(node);
	result = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

	members = json_object_get_members(object);
	for (member = g_list_first(members);  member != NULL;  member = member->next) {
		node = json_object_get_member(object, member->data);
		if (JSON_NODE_HOLDS_VALUE(node))
			g_hash_table_insert(result, g_strdup(member->data),
					    g_strdup(json_node_get_string(node)));
	}
	g_list_free(members);
 out:
	g_error_free(error);
	g_object_unref(parser);
	return result;
}

static gchar *parse_regex(SoupMessage *msg, const gchar *regex, guint group)
{
	GMatchInfo *match;
	GRegex *matcher;
	gchar *text = NULL;

	if (!msg->response_body || msg->response_body->length <= 0) {
		chime_debug("Empty text response\n");
		return text;
	}

	matcher = g_regex_new(regex, 0, 0, NULL);
	if (g_regex_match_full(matcher, msg->response_body->data,
			       msg->response_body->length, 0, 0, &match, NULL))
		text = g_match_info_fetch(match, group);

	g_match_info_free(match);
	g_regex_unref(matcher);
	return text;
}

/*
 * Parse a GWT-RPC response, returning an array of strings, its length and the
 * success status.  See the comments at the top of the file for more information
 * about GWT-RPC reverse engineering.
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
		chime_debug("Unexpected GWT response format\n");
		return fields;
	}
	*ok = !strncmp(msg->response_body->data + 2, "OK", 2);

	/* Parse the JSON content */
	parser = json_parser_new();
	if (!json_parser_load_from_data(parser, msg->response_body->data + 4,
					msg->response_body-> length - 4, &error)) {
		chime_debug("GWT-JSON parsing error: %s\n", error->message);
		goto out;
	}
	/* Get the content array */
	node = json_parser_get_root(parser);
	if (!JSON_NODE_HOLDS_ARRAY(node)) {
		chime_debug("Unexpected GWT-JSON type %d\n", JSON_NODE_TYPE(node));
		goto out;
	}
	body = json_node_get_array(node);
	length = json_array_get_length(body);
	if (length < 4) {
		chime_debug("GWT response array length %d too short\n", length);
		goto out;
	}
	/* Get the strings table */
	length -= 3;
	node = json_array_get_element(body, length);
	if (!JSON_NODE_HOLDS_ARRAY(node)) {
		chime_debug("Could not find GWT response strings table\n");
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

static void amazon_prepare_signin_form(struct signin *state, struct dom *dom, SoupMessage *msg)
{
	if (state->form) {
		free_form(state->form);
		state->form = NULL;
	}

	state->form = scrap_form(dom, soup_message_get_uri(msg), "//form[@name='signIn']");
	if (state->form && state->form->email_name)
		g_hash_table_insert(state->form->params,
				    g_strdup(state->form->email_name),
				    g_strdup(state->email));
}

/*
 * Soup and signal callbacks implementing the different sign-in types.  Defined
 * in reverse order to chain them together without using forward declarations.
 * Therefore, for better understanding of real sign-in step sequence, go to the
 * bottom of the file and read the functions upwards.
 */

static void session_token_cb(SoupSession *session, SoupMessage *msg, gpointer data)
{
	gchar *token;
	struct signin *state = data;

	fail_on_response_error(msg, state);

	token = parse_regex(msg, "['\"]chime://sso_sessions\\?Token=([^'\"]+)['\"]", 1);
	if (!token) {
		chime_debug("Could not find session token in final sign-in response\n");
		fail_bad_response(state, _("Unable to retrieve session token"));
		return;
	}

	chime_connection_set_session_token(state->connection, token);
	chime_connection_connect(state->connection);
	free_signin(state);
	g_free(token);
}

static void amazon_signin_result_cb(SoupSession *session, SoupMessage *msg, gpointer data)
{
	struct dom *dom;
	struct form *form;
	struct signin *state = data;

	fail_on_response_error(msg, state);

	dom = parse_html(msg);
	form = scrap_form(dom, soup_message_get_uri(msg), "//form[@name='consent-form']");
	if (form) {
		SoupMessage *next;

		g_hash_table_insert(form->params, g_strdup("consentApproved"), g_strdup(""));
		next = soup_form_request_new_from_hash(form->method, form->action, form->params);
		soup_session_queue_message(session, next, session_token_cb, state);
		goto out;
	}

	amazon_prepare_signin_form(state, dom, msg);
	if (state->form) {
		if (state->form->email_name && state->form->password_name)
			g_signal_emit_by_name(state->connection, "authenticate", state, FALSE);
		else
			fail_bad_response(state, _("Unexpected Amazon sign-in form during retry"));
	} else {
		session_token_cb(session, msg, state);
	}
 out:
	free_form(form);
	free_dom(dom);
}

static void amazon_send_credentials(struct signin *state, const gchar *password)
{
	SoupMessage *msg;

	if (!(password && *password)) {
		g_signal_emit_by_name(state->connection, "authenticate", state, FALSE);
		return;
	}

	g_hash_table_insert(state->form->params,
			    g_strdup(state->form->password_name),
			    g_strdup(password));
	msg = soup_form_request_new_from_hash(state->form->method,
					      state->form->action,
					      state->form->params);
	soup_message_headers_append(msg->request_headers, "Referer", state->form->referer);
	soup_message_headers_append(msg->request_headers, "Accept-Language", "en-US,en;q=0.5");
	soup_session_queue_message(state->session, msg, amazon_signin_result_cb, state);

	free_form(state->form);
	state->form = NULL;
}

static void amazon_signin_cb(SoupSession *session, SoupMessage *msg, gpointer data)
{
	struct dom *dom = NULL;
	struct signin *state = data;

	fail_on_response_error(msg, state);

	dom = parse_html(msg);
	amazon_prepare_signin_form(state, dom, msg);
	if (!(state->form && state->form->email_name && state->form->password_name))
		fail_bad_response(state, _("Could not find Amazon sign in form"));
	else
		g_signal_emit_by_name(state->connection, "authenticate", state, FALSE);
	free_dom(dom);
}

static void wd_credentials_response_cb(SoupSession *session, SoupMessage *msg, gpointer data)
{
	SoupMessage *next;
	gboolean ok;
	gchar **response;
	guint count;
	struct signin *state = data;

	fail_on_response_error(msg, state);

	response = parse_gwt(msg, &ok, &count);
	if (!response) {
		fail_gwt_discovery(state, "Unable to parse authentication response\n");
		return;
	}
	if (!ok) {
		if (count > 3 && !g_strcmp0(response[3], "AuthenticationFailedException"))
			g_signal_emit_by_name(state->connection, "authenticate", state, TRUE);
		else
			fail_bad_response(state, _("Unexpected corporate authentication failure"));
		goto out;
	}
	next = soup_form_request_new(SOUP_METHOD_GET, state->redirect_url,
				     "organization", state->directory,
				     "region", state->region,
				     "auth_code", response[2], NULL);
	soup_session_queue_message(session, next, session_token_cb, state);
 out:
	g_strfreev(response);
}

static void wd_send_credentials(struct signin *state, const gchar *user, const gchar *password)
{
	SoupMessage *msg;
	gchar *safe_user, *safe_password;
	static const gchar *type = "com.amazonaws.warpdrive.console.shared.LoginRequest_v4/3859384737";

	safe_user = escaped(user);
	safe_password = escaped(password);

	msg = gwt_request(state, WARPDRIVE_INTERFACE, "authenticateUser", 11,
			  type, type, "", "", state->client_id, "", NULL,
			  state->directory, safe_password, "", safe_user);

	soup_session_queue_message(state->session, msg, wd_credentials_response_cb, state);
	g_free(safe_password);
	g_free(safe_user);
}

static void gwt_region_cb(SoupSession *session, SoupMessage *msg, gpointer data)
{
	gboolean ok;
	gchar **response;
	guint count;
	struct signin *state = data;

	fail_on_response_error(msg, state);

	response = parse_gwt(msg, &ok, &count);
	if (!response) {
		fail_gwt_discovery(state, "Region response parsed NULL\n");
		return;
	}
	if (!ok) {
		fail_gwt_discovery(state, "GWT exception during region discovery\n");
		goto out;
	}

	state->region = g_strdup(response[count - 1]);
	if (!state->region) {
		fail_gwt_discovery(state, "NULL region value\n");
		goto out;
	}

	g_signal_emit_by_name(state->connection, "authenticate", state, TRUE);
 out:
	g_strfreev(response);
}

static void gwt_policy_cb(SoupSession *session, SoupMessage *msg, gpointer data)
{
	SoupMessage *next;
	static const gchar *type = "com.amazonaws.warpdrive.console.shared.ValidateClientRequest_v2/2136236667";
	struct signin *state = data;

	fail_on_response_error(msg, state);

	state->gwt_policy = parse_regex(msg, GWT_ID_REGEX, 1);
	if (!state->gwt_policy) {
		fail_gwt_discovery(state, "No GWT policy found\n");
		return ;
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
	struct signin *state = data;

	fail_on_response_error(msg, state);

	state->gwt_permutation = parse_regex(msg, GWT_ID_REGEX, 1);
	if (!state->gwt_permutation) {
		fail_gwt_discovery(state, "No GWT permutation found\n");
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
 * Initial WD sign in page scrapping.
 *
 * Ironically, most of the relevant data coming from this response is placed in
 * the GET parameters (both from the initial URL and the redirection).  From the
 * HTML body we only want the location of the GWT bootstrapping Javascript code.
 */
static void wd_signin_cb(SoupSession *session, SoupMessage *msg, gpointer data)
{
	GHashTable *params;
	SoupMessage *next;
	SoupURI *initial, *base;
	gchar *sep, *js = NULL;
	struct dom *dom = NULL;
	struct signin *state = data;

	fail_on_response_error(msg, state);

	initial = soup_message_get_first_party(msg);
	params = soup_form_decode(soup_uri_get_query(initial));
	state->directory = g_strdup(g_hash_table_lookup(params, "directory"));
	if (!state->directory) {
		fail_gwt_discovery(state, "Directory identifier not found\n");
		goto out;
	}

	g_hash_table_destroy(params);  /* Reuse the variable */
	base = soup_message_get_uri(msg);
	params = soup_form_decode(soup_uri_get_query(base));
	state->client_id = g_strdup(g_hash_table_lookup(params, "client_id"));
	state->redirect_url = g_strdup(g_hash_table_lookup(params, "redirect_uri"));
	if (!(state->client_id && state->redirect_url)) {
		fail_gwt_discovery(state, "Client ID or callback missing\n");
		goto out;
	}
	state->gwt_rpc_uri = soup_uri_new_with_base(base, "WarpDriveLogin/GalaxyInternalService");

	dom = parse_html(msg);
	js = xpath_string(dom, "//script[contains(@src, '/WarpDriveLogin/')][1]/@src");
	if (!(dom && js)) {
		fail_gwt_discovery(state, "JS bootstrap URL not found\n");
		goto out;
	}
	sep = strrchr(js, '/');
	state->gwt_module_base = g_strndup(js, (sep - js) + 1);

	next = soup_message_new(SOUP_METHOD_GET, js);
	soup_session_queue_message(session, next, gwt_entry_point_cb, state);
 out:
	g_free(js);
	free_dom(dom);
	g_hash_table_destroy(params);
}

static void signin_search_result_cb(SoupSession *session, SoupMessage *msg, gpointer data)
{
	GHashTable *provider_info;
	SoupMessage *next;
	SoupSessionCallback handler;
	SoupURI *destination;
	gchar *type, *path;
	struct signin *state = data;

	if (msg->status_code == 400) {
		/* This is a known quirk */
		fail_bad_response(state, _("Invalid e-mail address <%s>"), state->email);
		return;
	}

	fail_on_response_error(msg, state);

	provider_info = parse_json(msg);
	if (!provider_info) {
		fail_bad_response(state, _("Error searching for sign-in provider"));
		return;
	}

	type = g_hash_table_lookup(provider_info, "provider");
	if (!g_strcmp0(type, "amazon")) {
		handler = amazon_signin_cb;
	} else if (!g_strcmp0(type, "wd")) {
		handler = wd_signin_cb;
	} else {
		chime_debug("Unrecognized sign-in provider %s\n", type);
		fail_bad_response(state, _("Unknown sign-in provider"));
		goto out;
	}

	path = g_hash_table_lookup(provider_info, "path");
	if (!path) {
		chime_debug("Server did not provide a path\n");
		fail_bad_response(state, _("Incomplete provider response"));
		goto out;
	}

	destination = soup_uri_new_with_base(soup_message_get_uri(msg), path);
	next = soup_message_new_from_uri(SOUP_METHOD_GET, destination);
	soup_message_set_first_party(next, destination);
	soup_session_queue_message(session, next, handler, state);
	soup_uri_free(destination);
 out:
	g_hash_table_destroy(provider_info);
}

static void signin_page_cb(SoupSession *session, SoupMessage *msg, gpointer data)
{
	SoupMessage *next;
	struct signin *state = data;
	struct dom *dom = NULL;
	struct form *form = NULL;

	fail_on_response_error(msg, state);

	dom = parse_html(msg);
	form = scrap_form(dom, soup_message_get_uri(msg), "//form[@id='picker_email']");
	if (!(form && form->email_name)) {
		fail_bad_response(state, _("Error initiating sign in"));
		goto out;
	}

	g_hash_table_insert(form->params, g_strdup(form->email_name),
			    g_strdup(state->email));
	next = soup_form_request_new_from_hash(form->method, form->action, form->params);
	soup_session_queue_message(session, next, signin_search_result_cb, state);
 out:
	free_form(form);
	free_dom(dom);
}

/*
 * Sign-in process entry point.
 *
 * This is where the plugin initiates the authentication process.  Control is
 * transferred to this module until a connection is canceled or restarted once
 * we have the session token.
 *
 * A new, independent, SoupSession is created to keep track of the massive
 * amount of cookies only until we obtain the authentication token.
 */
void chime_connection_signin(ChimeConnection *self)
{
	SoupMessage *msg;
	gchar *server;
	guint signal_id;
	gulong handler;
	struct signin *state;

	g_return_if_fail(CHIME_IS_CONNECTION(self));

	/* Make sure the "authenticate" signal is connected */
	signal_id = g_signal_lookup("authenticate", G_OBJECT_TYPE(self));
	g_assert(signal_id != 0);
	handler = g_signal_handler_find(self, G_SIGNAL_MATCH_ID, signal_id, 0,
					NULL, NULL, NULL);
	if (handler == 0 || !g_signal_handler_is_connected(self, handler)) {
		chime_debug("Signal \"authenticate\" must be connected to complete sign-in\n");
		chime_connection_fail(self, CHIME_ERROR_AUTH_FAILED, _("Internal API error"));
		return;
	}

	state = g_new0(struct signin, 1);
	state->connection = g_object_ref(self);
	state->session = soup_session_new_with_options(SOUP_SESSION_ADD_FEATURE_BY_TYPE,
						       SOUP_TYPE_COOKIE_JAR,
						       SOUP_SESSION_USER_AGENT,
						       "libchime " PACKAGE_VERSION " ",
						       NULL);

	g_object_get(self, "account-email", &state->email, NULL);
	if (!(state->email && *state->email)) {
		chime_debug("The ChimeConnection object does not indicate an account name\n");
		fail(state, g_error_new(CHIME_ERROR, CHIME_ERROR_AUTH_FAILED,
					_("Internal API error")));
		return;
	}

	if (getenv("CHIME_DEBUG") && atoi(getenv("CHIME_DEBUG")) > 1) {
		SoupLogger *l = soup_logger_new(SOUP_LOGGER_LOG_BODY, -1);
		soup_session_add_feature(state->session, SOUP_SESSION_FEATURE(l));
		g_object_unref(l);
	}

	g_object_get(self, "server", &server, NULL);
	msg = soup_message_new(SOUP_METHOD_GET, server);
	soup_session_queue_message(state->session, msg, signin_page_cb, state);
	g_free(server);
}

/*
 * Credentials submission function.
 *
 * Because authentication needs interaction, the process hands control to the
 * client in order query the user.  Once the credentials have been collected,
 * the client should return the control back to the authentication process using
 * this function.
 *
 * If the credentials are NULL, it is interpreted as an error and the sign-in
 * procedure is canceled.
 */
void chime_connection_authenticate(gpointer opaque, const gchar *user, const gchar *password)
{
	struct signin *state = opaque;
	g_assert(opaque != NULL);

	if (state->region && user && *user && password && *password)
		wd_send_credentials(state, user, password);
	else if (state->form && password && *password)
		amazon_send_credentials(state, password);
	else
		cancel_signin(state);
}
