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

#ifndef __CHIME_CONVERSATION_H__
#define __CHIME_CONVERSATION_H__

#include <glib-object.h>

#include <json-glib/json-glib.h>

#include "chime-connection.h"
#include "chime-object.h"

G_BEGIN_DECLS

#define CHIME_TYPE_CONVERSATION (chime_conversation_get_type ())
G_DECLARE_FINAL_TYPE (ChimeConversation, chime_conversation, CHIME, CONVERSATION, ChimeObject)

const gchar *chime_conversation_get_id(ChimeConversation *self);

const gchar *chime_conversation_get_name(ChimeConversation *self);

const gchar *chime_conversation_get_channel(ChimeConversation *self);

gboolean chime_conversation_get_favourite(ChimeConversation *self);

gboolean chime_conversation_get_visibility(ChimeConversation *self);

GList *chime_conversation_get_members(ChimeConversation *self);
gboolean chime_conversation_has_member(ChimeConversation *conv, const gchar *member_id);

const gchar *chime_conversation_get_last_sent(ChimeConversation *self);

const gchar *chime_conversation_get_updated_on(ChimeConversation *self);

ChimeConversation *chime_connection_conversation_by_name(ChimeConnection *cxn,
					 const gchar *name);
ChimeConversation *chime_connection_conversation_by_id(ChimeConnection *cxn,
				       const gchar *id);

/* Designed to match the NEW_CONVERSATION signal handler */
typedef void (*ChimeConversationCB) (ChimeConnection *, ChimeConversation *, gpointer);
void chime_connection_foreach_conversation(ChimeConnection *cxn, ChimeConversationCB cb,
				   gpointer cbdata);

void chime_conversation_send_typing(ChimeConnection *cxn, ChimeConversation *conv,
				    gboolean typing);


void chime_connection_create_conversation_async(ChimeConnection *cxn,
						GSList *contacts,
						GCancellable *cancellable,
						GAsyncReadyCallback callback,
						gpointer user_data);

ChimeConversation *chime_connection_create_conversation_finish(ChimeConnection *self,
							       GAsyncResult *result,
							       GError **error);

void chime_connection_find_conversation_async(ChimeConnection *cxn,
					      GSList *contacts,
					      GCancellable *cancellable,
					      GAsyncReadyCallback callback,
					      gpointer user_data);

ChimeConversation *chime_connection_find_conversation_finish(ChimeConnection *self,
							     GAsyncResult *result,
							     GError **error);

G_END_DECLS

#endif /* __CHIME_CONVERSATION_H__ */
