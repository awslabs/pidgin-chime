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

#include <glib/gi18n.h>
#include <glib/glist.h>

#include <prpl.h>
#include <blist.h>
#include <debug.h>

#include "chime.h"

#include <libsoup/soup.h>

struct chime_im {
	struct chime_msgs m;
	ChimeContact *peer;
};

/* Called for all deliveries of incoming conversation messages, at startup and later */
static gboolean do_conv_deliver_msg(ChimeConnection *cxn, struct chime_im *im,
				    JsonNode *record, time_t msg_time)
{
	const gchar *sender, *message;
	gint64 sys;

	if (!parse_string(record, "Sender", &sender) ||
	    !parse_string(record, "Content", &message) ||
	    !parse_int(record, "IsSystemMessage", &sys))
		return FALSE;

	PurpleMessageFlags flags = 0;
	if (sys)
		flags |= PURPLE_MESSAGE_SYSTEM;

	const gchar *email = chime_contact_get_email(im->peer);
	gchar *escaped = g_markup_escape_text(message, -1);
	if (!strcmp(sender, chime_connection_get_profile_id(cxn))) {
		/* Ick, how do we inject a message from ourselves? */
		PurpleAccount *account = im->m.conn->account;
		PurpleConversation *pconv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM,
										  email, account);
		if (!pconv) {
			pconv = purple_conversation_new(PURPLE_CONV_TYPE_IM, account, email);
			if (!pconv) {
				purple_debug_error("chime", "NO CONV FOR %s\n", email);
				g_free(escaped);
				return FALSE;
			}
		}
		purple_conversation_write(pconv, NULL, escaped, flags | PURPLE_MESSAGE_SEND, msg_time);
	} else {
		serv_got_im(im->m.conn, email, escaped, flags | PURPLE_MESSAGE_RECV, msg_time);
	}
	g_free(escaped);
	return TRUE;
}

static void on_conv_typing(ChimeConversation *conv, ChimeContact *contact, gboolean state, struct chime_im *im)
{
	const gchar *email = chime_contact_get_email(contact);

	if (state)
		serv_got_typing(im->m.conn, email, 0, PURPLE_TYPING);
	else
		serv_got_typing_stopped(im->m.conn, email);
}

void on_chime_new_conversation(ChimeConnection *cxn, ChimeConversation *conv, PurpleConnection *conn)
{
	struct purple_chime *pc = purple_connection_get_protocol_data(conn);

	GList *members = chime_conversation_get_members(conv);
	if (g_list_length(members) != 2) {
		on_chime_new_group_conv(cxn, conv, conn);
		return;
	}
	struct chime_im *im = g_new0(struct chime_im, 1);
	im->peer = members->data;

	const gchar *profile_id = chime_contact_get_profile_id(im->peer);
	if (!strcmp(chime_connection_get_profile_id(cxn), profile_id)) {
		im->peer = members->next->data;
		profile_id = chime_contact_get_profile_id(im->peer);
	}
	g_list_free(members);
	g_object_ref(im->peer);

	const gchar *email = chime_contact_get_email(im->peer);
	/* Where multiple profiles exist with the same email address (yes, it happens!),
	 * we want to prefer the one that has a sane display_name (and not just the
	 * email address in the display_name field, as incomplete profiles tend to have).
	 * Theoretically we could look at the ProfileType but we don't actually hav
	 * that right now.
	 *
	 * So insert into the ims_by_email hash table if email != display_name
	 * or (obviously) if there wasn't already an IM for this email address. */
	if (strcmp(email, chime_contact_get_display_name(im->peer)) ||
	    !g_hash_table_lookup(pc->ims_by_email, email))
		g_hash_table_insert(pc->ims_by_email, (void *)email, im);

	g_hash_table_insert(pc->ims_by_profile_id, (void *)profile_id, im);

	g_signal_connect(conv, "typing", G_CALLBACK(on_conv_typing), im);

	purple_debug(PURPLE_DEBUG_INFO, "chime", "New conversation %s with %s\n", chime_object_get_id(CHIME_OBJECT(im->peer)),
		     chime_contact_get_email(im->peer));

	init_msgs(conn, &im->m, CHIME_OBJECT(conv), (chime_msg_cb)do_conv_deliver_msg, chime_contact_get_email(im->peer), NULL);

}

static void im_destroy(gpointer _im)
{
	struct chime_im *im = _im;

	g_signal_handlers_disconnect_matched(im->m.obj, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, im);
	g_object_unref(im->peer);
	cleanup_msgs(&im->m);
	g_free(im);
}

void purple_chime_init_conversations(struct purple_chime *pc)
{
	pc->ims_by_email = g_hash_table_new(g_str_hash, g_str_equal);
	pc->ims_by_profile_id = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, im_destroy);
}

void purple_chime_destroy_conversations(struct purple_chime *pc)
{
	g_clear_pointer(&pc->ims_by_email, g_hash_table_destroy);
	g_clear_pointer(&pc->ims_by_profile_id, g_hash_table_destroy);
}

struct im_send_data {
	PurpleConnection *conn;
	struct chime_im *im;
	ChimeContact *contact;
	gchar *who;
	gchar *message;
	PurpleMessageFlags flags;
};

static void im_send_error(ChimeConnection *cxn, struct im_send_data *imd,
			  const gchar *format, ...)
{
	va_list args;

	va_start(args, format);
	gchar *msg = g_strdup_vprintf(format, args);
	va_end(args);

	PurpleConversation *pconv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_ANY,
									  imd->who,
									  imd->conn->account);
	if (pconv)
		purple_conversation_write(pconv, NULL, msg, PURPLE_MESSAGE_ERROR, time(NULL));

	g_free(msg);
}

unsigned int chime_send_typing(PurpleConnection *conn, const char *name, PurpleTypingState state)
{
	/* We can't get to PURPLE_TYPED unless we've already sent PURPLE_TYPING... */
	if (state == PURPLE_TYPED)
		return 0;

	struct purple_chime *pc = purple_connection_get_protocol_data(conn);
	struct chime_im *im = g_hash_table_lookup(pc->ims_by_email, name);
	if (!im)
		return 0;

	chime_conversation_send_typing(pc->cxn, CHIME_CONVERSATION(im->m.obj), state == PURPLE_TYPING);

	return 0;
}

static void sent_im_cb(GObject *source, GAsyncResult *result, gpointer _imd)
{
	struct im_send_data *imd = _imd;
	ChimeConnection *cxn = CHIME_CONNECTION(source);
	GError *error = NULL;

	JsonNode *msgnode = chime_connection_send_message_finish(cxn, result, &error);
	if (msgnode) {
		const gchar *msg_id;
		if (!parse_string(msgnode, "MessageId", &msg_id))
			im_send_error(cxn, imd, _("Failed to send message"));
		json_node_unref(msgnode);
	} else {
		im_send_error(cxn, imd, error->message);
		g_clear_error(&error);
	}

	if (imd->contact)
		g_object_unref(imd->contact);
	g_free(imd->who);
	g_free(imd->message);
	g_free(imd);
}


static void create_im_cb(GObject *source, GAsyncResult *result, gpointer _imd)
{
	ChimeConnection *cxn = CHIME_CONNECTION(source);
	struct im_send_data *imd = _imd;
	ChimeConversation *conv = chime_connection_create_conversation_finish(cxn, result, NULL);
	struct purple_chime *pc = purple_connection_get_protocol_data(imd->conn);

	if (conv) {
		g_object_unref(conv);

		imd->im = g_hash_table_lookup(pc->ims_by_email, imd->who);
		if (!imd->im) {
			purple_debug(PURPLE_DEBUG_INFO, "chime", "No im for %s\n", imd->who);
			goto bad;
		}

		chime_connection_send_message_async(cxn, imd->im->m.obj, imd->message, NULL, sent_im_cb, imd);
		return;
	}
 bad:
	im_send_error(cxn, imd, _("Failed to create IM conversation"));
	if (imd->contact)
		g_object_unref(imd->contact);
	g_free(imd->who);
	g_free(imd->message);
	g_free(imd);
}

static void find_im_cb(GObject *source, GAsyncResult *result, gpointer _imd)
{
	ChimeConnection *cxn = CHIME_CONNECTION(source);
	struct im_send_data *imd = _imd;
	ChimeConversation *conv = chime_connection_find_conversation_finish(cxn, result, NULL);
	struct purple_chime *pc = purple_connection_get_protocol_data(imd->conn);

	if (conv) {
		g_object_unref(conv);

		imd->im = g_hash_table_lookup(pc->ims_by_email, imd->who);
		if (!imd->im) {
			purple_debug(PURPLE_DEBUG_INFO, "chime", "No im for %s\n", imd->who);
			g_object_unref(imd->contact);
			g_free(imd->who);
			g_free(imd->message);
			g_free(imd);
		} else {
			chime_connection_send_message_async(cxn, imd->im->m.obj, imd->message, NULL, sent_im_cb, imd);
		}
		return;
	}

	GSList *l = g_slist_append(NULL, imd->contact);
	chime_connection_create_conversation_async(cxn, l, NULL, create_im_cb, imd);
	g_slist_free_1(l);
}

static void autocomplete_im_cb(GObject *source, GAsyncResult *result, gpointer _imd)
{
	ChimeConnection *cxn = CHIME_CONNECTION(source);
	struct im_send_data *imd = _imd;
	GSList *contacts = chime_connection_autocomplete_contact_finish(cxn, result, NULL);

	while (contacts) {
		ChimeContact *contact = contacts->data;
		if (!strcmp(imd->who, chime_contact_get_email(contact))) {
			GSList *l = g_slist_append(NULL, contact);
			imd->contact = g_object_ref(contact);
			chime_connection_find_conversation_async(cxn, l, NULL, find_im_cb, imd);
			g_slist_free_1(l);
			g_slist_free_full(contacts, g_object_unref);
			return;
		}
		g_object_unref(contact);
		contacts = g_slist_remove(contacts, contact);
	}

	im_send_error(cxn, imd, _("Failed to find user"));
	g_free(imd->who);
	g_free(imd->message);
	g_free(imd);
}

int chime_purple_send_im(PurpleConnection *gc, const char *who, const char *message, PurpleMessageFlags flags)
{
	struct purple_chime *pc = purple_connection_get_protocol_data(gc);

	struct im_send_data *imd = g_new0(struct im_send_data, 1);
	imd->conn = gc;
	purple_markup_html_to_xhtml(message, NULL, &imd->message);
	imd->who = g_strdup(who);
	imd->flags = flags;

	imd->im = g_hash_table_lookup(pc->ims_by_email, who);
	if (imd->im) {
		chime_connection_send_message_async(pc->cxn, imd->im->m.obj, imd->message, NULL, sent_im_cb, imd);
		return 0;
	}

	ChimeContact *contact = chime_connection_contact_by_email(pc->cxn, who);
	if (contact) {
		GSList *l = g_slist_append(NULL, contact);
		imd->contact = g_object_ref(contact);
		chime_connection_find_conversation_async(pc->cxn, l, NULL, find_im_cb, imd);
		g_slist_free_1(l);
		return 0;
	}

	chime_connection_autocomplete_contact_async(pc->cxn, who, NULL, autocomplete_im_cb, imd);
	return 0;
}
