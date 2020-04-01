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
				    JsonNode *record, time_t msg_time, gboolean new_msg)
{
	const gchar *sender, *message;
	gint64 sys;
	if (!parse_string(record, "Sender", &sender) ||
	    !parse_int(record, "IsSystemMessage", &sys))
		return FALSE;

	PurpleMessageFlags flags = 0;
	if (sys)
		flags |= PURPLE_MESSAGE_SYSTEM;
	if (!new_msg)
		flags |= PURPLE_MESSAGE_DELAYED;

	const gchar *email = chime_contact_get_email(im->peer);
	const gchar *from = _("Unknown sender");
	if (!strcmp(sender, chime_connection_get_profile_id(cxn))) {
		from = chime_connection_get_email(cxn);
	} else {
		ChimeContact *who = chime_connection_contact_by_id(cxn, sender);
		if (who)
			from = chime_contact_get_email(who);
	}

	ChimeAttachment *att = extract_attachment(record);
	if (att) {
		AttachmentContext *ctx = g_new(AttachmentContext, 1);
		ctx->conn = im->m.conn;
		ctx->chat_id = -1;
		ctx->from = from;
		ctx->im_email = email;
		ctx->when = msg_time;
		/* The attachment and context structs will be owned by the code doing the download and will be disposed of at the end. */
		download_attachment(cxn, att, ctx);
	}
	// Download messages don't have 'content' but normal messages do.
	// if you receive one, parse it:
	if (parse_string(record, "Content", &message)) {
		gchar *escaped = g_markup_escape_text(message, -1);

		if (!strcmp(sender, chime_connection_get_profile_id(cxn))) {
			/* Ick, how do we inject a message from ourselves? */
			PurpleAccount *account = im->m.conn->account;
			PurpleConversation *pconv = purple_find_conversation_with_account(
					PURPLE_CONV_TYPE_IM, email, account);
			if (!pconv) {
				pconv = purple_conversation_new(PURPLE_CONV_TYPE_IM, account,
												email);
				if (!pconv) {
					purple_debug_error("chime", "NO CONV FOR %s\n", email);
					g_free(escaped);
					return FALSE;
				}
			}
			purple_conversation_write(pconv, NULL, escaped,
					flags | PURPLE_MESSAGE_SEND, msg_time);
			purple_signal_emit(purple_connection_get_prpl(account->gc),
					"chime-got-convmsg", pconv, TRUE, record);
		} else {
			serv_got_im(im->m.conn, email, escaped, flags | PURPLE_MESSAGE_RECV,
						msg_time);

			/* If the conversation already had focus and unseen-count didn't change, fake
			 a PURPLE_CONV_UPDATE_UNSEEN notification anyway, so that we see that it's
			 (still) zero and tell the server it's read. */
			PurpleConversation *pconv = purple_find_conversation_with_account(
					PURPLE_CONV_TYPE_IM, email, im->m.conn->account);
			if (pconv) {
				purple_conversation_update(pconv, PURPLE_CONV_UPDATE_UNSEEN);
				purple_signal_emit(purple_connection_get_prpl(im->m.conn),
								   "chime-got-convmsg", pconv, FALSE, record);
			}

		}
		g_free(escaped);
	}
	return TRUE;
}

static void on_conv_membership(ChimeConversation *conv, JsonNode *member, struct chime_im *im)
{

	const gchar *profile_id;
	if (!parse_string(member, "ProfileId", &profile_id))
		return;

	/* We only care about the peer, not our own status */
	if (!strcmp(profile_id, chime_connection_get_profile_id(PURPLE_CHIME_CXN(im->m.conn))))
		return;

	PurpleConversation *pconv;
	pconv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM,
						      chime_contact_get_email(im->peer),
						      im->m.conn->account);
	if (!pconv)
		return;

	purple_signal_emit(purple_connection_get_prpl(im->m.conn),
			   "chime-conv-membership", pconv, member);
}

static void on_conv_typing(ChimeConversation *conv, ChimeContact *contact, gboolean state, struct chime_im *im)
{
	const gchar *email = chime_contact_get_email(contact);

	if (state)
		serv_got_typing(im->m.conn, email, 0, PURPLE_TYPING);
	else
		serv_got_typing_stopped(im->m.conn, email);
}

/* Return TRUE or set *peer to the one other member */
static gboolean is_group_conv(ChimeConnection *cxn, ChimeConversation *conv, ChimeContact **peer)
{
	GList *members = chime_conversation_get_members(conv);
	if (g_list_length(members) != 2) {
		g_list_free(members);
		return TRUE;
	}

	/* We want the one that *isn't* the local user */
	if (!strcmp(chime_connection_get_profile_id(cxn), chime_contact_get_profile_id(members->data)))
		*peer = g_object_ref(members->next->data);
	else
		*peer = g_object_ref(members->data);

	g_list_free(members);

	return FALSE;
}

static void refresh_convlist(ChimeObject *obj, GParamSpec *pspec, PurpleConnection *conn);

void on_chime_new_conversation(ChimeConnection *cxn, ChimeConversation *conv, PurpleConnection *conn)
{
	struct purple_chime *pc = purple_connection_get_protocol_data(conn);
	ChimeContact *peer = NULL;

	/* If we are displaying the Recent Connections dialog, update it. */
	refresh_convlist(NULL, NULL, conn);

	if (is_group_conv(cxn, conv, &peer)) {
		on_chime_new_group_conv(cxn, conv, conn);
		return;
	}

	struct chime_im *im = g_new0(struct chime_im, 1);
	im->peer = peer;

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

	g_hash_table_insert(pc->ims_by_profile_id, (void *)chime_contact_get_profile_id(im->peer), im);

	g_signal_connect(conv, "typing", G_CALLBACK(on_conv_typing), im);
	g_signal_connect(conv, "membership", G_CALLBACK(on_conv_membership), im);

	purple_debug(PURPLE_DEBUG_INFO, "chime", "New conversation %s with %s\n", chime_object_get_id(CHIME_OBJECT(im->peer)),
		     chime_contact_get_email(im->peer));

	init_msgs(conn, &im->m, CHIME_OBJECT(conv), (chime_msg_cb)do_conv_deliver_msg, chime_contact_get_email(im->peer), NULL);

}

struct im_send_data {
	PurpleConnection *conn;
	struct chime_im *im;
	ChimeContact *contact;
	gchar *who;
	gchar *message;
	PurpleMessageFlags flags;
};

static void free_imd(struct im_send_data *imd)
{
	if (imd->contact)
		g_object_unref(imd->contact);
	g_free(imd->who);
	g_free(imd->message);
	g_free(imd);
}

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

	free_imd(imd);
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

		if (imd->message)
			chime_connection_send_message_async(cxn, imd->im->m.obj, imd->message, NULL, sent_im_cb, imd, NULL);
		else
			free_imd(imd);
		return;
	}
 bad:
	im_send_error(cxn, imd, _("Failed to create IM conversation"));
	free_imd(imd);
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
			free_imd(imd);
		} else {
			if (imd->message)
				chime_connection_send_message_async(cxn, imd->im->m.obj, imd->message, NULL, sent_im_cb, imd, NULL);
			else
				free_imd(imd);
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
	free_imd(imd);
}

int chime_purple_send_im(PurpleConnection *gc, const char *who, const char *message, PurpleMessageFlags flags)
{
	struct purple_chime *pc = purple_connection_get_protocol_data(gc);

	struct im_send_data *imd = g_new0(struct im_send_data, 1);
	imd->conn = gc;
	if (message)
		purple_markup_html_to_xhtml(message, NULL, &imd->message);
	imd->who = g_strdup(who);
	imd->flags = flags;

	imd->im = g_hash_table_lookup(pc->ims_by_email, who);
	if (imd->im) {
		if (message)
			chime_connection_send_message_async(pc->cxn, imd->im->m.obj, imd->message, NULL, sent_im_cb, imd, NULL);
		else
			free_imd(imd);
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

static void unsub_conv_object(ChimeConnection *cxn, ChimeObject *obj, PurpleConnection *conn)
{
	g_signal_handlers_disconnect_matched(obj, G_SIGNAL_MATCH_FUNC|G_SIGNAL_MATCH_DATA, 0, 0, NULL,
					     G_CALLBACK(refresh_convlist), conn);
}

static void convlist_closed_cb(gpointer _conn)
{
	PurpleConnection *conn = _conn;
	struct purple_chime *pc = purple_connection_get_protocol_data(conn);

	if (!pc)
		return;

	if (pc->convlist_refresh_id) {
		g_source_remove(pc->convlist_refresh_id);
		pc->convlist_refresh_id = 0;
	}
	pc->convlist_handle = NULL;

	/* Unsubscribe from all the signals that were updating the dialog contents */
	chime_connection_foreach_conversation(PURPLE_CHIME_CXN(conn), (void *)unsub_conv_object, conn);
	chime_connection_foreach_contact(PURPLE_CHIME_CXN(conn), (void *)unsub_conv_object, conn);

}

static void open_im_conv(PurpleConnection *conn, GList *row, gpointer _unused)
{
	ChimeConnection *cxn = PURPLE_CHIME_CXN(conn);
	ChimeConversation *conv = chime_connection_conversation_by_name(cxn, row->data);

	if (!conv)
		return;

	ChimeContact *peer = NULL;
	if (is_group_conv(cxn, conv, &peer)) {
		do_join_chat(conn, cxn, CHIME_OBJECT(conv), NULL, NULL);
	} else {
		PurpleConversation *pconv = purple_conversation_new(PURPLE_CONV_TYPE_IM,
								   purple_connection_get_account(conn),
								   chime_contact_get_email(peer));
		g_object_unref(peer);
		purple_conversation_present(pconv);
	}
}

static gint compare_conv_date(ChimeConversation *a, ChimeConversation *b)
{
	return g_strcmp0(chime_conversation_get_updated_on(b),
			 chime_conversation_get_updated_on(a));
}

static void insert_conv(ChimeConnection *cxn, ChimeConversation *conv, gpointer _convs)
{
	GList **convs = _convs;

	/* We don't ref it as we'll use it immediately before anything else can happen */
	*convs = g_list_insert_sorted(*convs, conv, (GCompareFunc) compare_conv_date);
}

static PurpleNotifySearchResults *generate_recent_convs(PurpleConnection *conn)
{
	PurpleNotifySearchResults *results = purple_notify_searchresults_new();
	PurpleNotifySearchColumn *column;

	column = purple_notify_searchresults_column_new(_("Who"));
	purple_notify_searchresults_column_add(results, column);
	column = purple_notify_searchresults_column_new(_("Updated"));
	purple_notify_searchresults_column_add(results, column);
	column = purple_notify_searchresults_column_new(_("Availability"));
	purple_notify_searchresults_column_add(results, column);

	purple_notify_searchresults_button_add(results, PURPLE_NOTIFY_BUTTON_IM, open_im_conv);

	GList *convs = NULL;
	chime_connection_foreach_conversation(PURPLE_CHIME_CXN(conn), insert_conv, &convs);

	gpointer klass = g_type_class_ref(CHIME_TYPE_AVAILABILITY);

	while (convs) {
		ChimeConversation *conv = convs->data;
		convs = g_list_delete_link(convs, convs);

		GList *row = NULL;
		row = g_list_append(row, g_strdup(chime_conversation_get_name(conv)));
		row = g_list_append(row, g_strdup(chime_conversation_get_updated_on(conv)));

		ChimeContact *peer = NULL;
		if (is_group_conv(PURPLE_CHIME_CXN(conn), conv, &peer)) {
			row = g_list_append(row, g_strdup("(N/A)"));
		} else {
			GEnumValue *val = g_enum_get_value(klass, chime_contact_get_availability(peer));
			row = g_list_append(row, g_strdup(_(val->value_nick)));
			g_signal_handlers_disconnect_matched(peer, G_SIGNAL_MATCH_FUNC|G_SIGNAL_MATCH_DATA, 0, 0, NULL,
						     G_CALLBACK(refresh_convlist), conn);
			g_signal_connect(peer, "notify::availability", G_CALLBACK(refresh_convlist), conn);
			g_object_unref(peer);
		}

		purple_notify_searchresults_row_add(results, row);

		g_signal_handlers_disconnect_matched(conv, G_SIGNAL_MATCH_FUNC|G_SIGNAL_MATCH_DATA, 0, 0, NULL,
						     G_CALLBACK(refresh_convlist), conn);
		g_signal_connect(conv, "notify::name", G_CALLBACK(refresh_convlist), conn);
		g_signal_connect(conv, "notify::updated_on", G_CALLBACK(refresh_convlist), conn);
	}

	g_type_class_unref(klass);
	return results;
}

static gboolean update_convlist(gpointer _conn)
{
	PurpleConnection *conn = _conn;
	struct purple_chime *pc = purple_connection_get_protocol_data(conn);

	PurpleNotifySearchResults *results = generate_recent_convs(conn);
	purple_notify_searchresults_new_rows(conn, results, pc->convlist_handle);

	pc->convlist_refresh_id = 0;
	return FALSE;
}

static void refresh_convlist(ChimeObject *obj, GParamSpec *pspec, PurpleConnection *conn)
{
	struct purple_chime *pc = purple_connection_get_protocol_data(conn);

	if (!pc->convlist_handle || pc->convlist_refresh_id)
		return;

	pc->convlist_refresh_id = g_idle_add(update_convlist, conn);
}

void chime_purple_recent_conversations(PurplePluginAction *action)
{
	PurpleConnection *conn = (PurpleConnection *) action->context;
	struct purple_chime *pc = purple_connection_get_protocol_data(conn);

	if (pc->convlist_handle) {
		if (!pc->convlist_refresh_id)
			pc->convlist_refresh_id = g_idle_add(update_convlist, conn);
		return;
	}

	PurpleNotifySearchResults *results = generate_recent_convs(conn);
	pc->convlist_handle = purple_notify_searchresults(conn, _("Recent Chime Conversations"),
							  _("Recent conversations:"),
							  conn->account->username, results,
							  convlist_closed_cb, conn);
	if (!pc->convlist_handle) {
		purple_notify_error(conn, NULL,
				    _("Unable to display recent conversations."),
				    NULL);
		convlist_closed_cb(conn);
		return;
	}
}

static void im_destroy(gpointer _im)
{
	struct chime_im *im = _im;

	g_signal_handlers_disconnect_matched(im->m.obj, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, im);
	g_object_unref(im->peer);
	cleanup_msgs(&im->m);
	/* im == &im->m, and it's freed by cleanup_msgs */
}

static void chime_conv_created_cb(PurpleConversation *conv, PurpleConnection *conn)
{
	if (conv->account != conn->account)
		return;

	if (purple_conversation_get_type(conv) != PURPLE_CONV_TYPE_CHAT)
		return;

	purple_debug(PURPLE_DEBUG_INFO, "chime",
		     "Conversation '%s' created\n", conv->name);

	struct purple_chime *pc = purple_connection_get_protocol_data(conn);

	/* If the conversation isn't already known, find or create it.
	 * Use the chime_purple_send_im() call chain to do that, with
	 * a NULL message. */
	if (!g_hash_table_lookup(pc->ims_by_email, conv->name))
		chime_purple_send_im(conn, conv->name, NULL, 0);
}

void purple_chime_init_conversations(PurpleConnection *conn)
{
	struct purple_chime *pc = purple_connection_get_protocol_data(conn);

	pc->ims_by_email = g_hash_table_new(g_str_hash, g_str_equal);
	pc->ims_by_profile_id = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, im_destroy);

	purple_signal_connect(purple_conversations_get_handle(),
			      "conversation-created", conn,
			      PURPLE_CALLBACK(chime_conv_created_cb), conn);
}

void purple_chime_destroy_conversations(PurpleConnection *conn)
{
	struct purple_chime *pc = purple_connection_get_protocol_data(conn);

	purple_signal_disconnect(purple_conversations_get_handle(),
				 "conversation-created", conn,
				 PURPLE_CALLBACK(chime_conv_created_cb));

	g_clear_pointer(&pc->ims_by_email, g_hash_table_destroy);
	g_clear_pointer(&pc->ims_by_profile_id, g_hash_table_destroy);

	if (pc->convlist_handle)
		convlist_closed_cb(conn);
}
