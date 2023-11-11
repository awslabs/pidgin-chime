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
#include <roomlist.h>
#include <debug.h>

#include "chime.h"
#include "chime-room.h"

#include <libsoup/soup.h>

struct room_sort {
	struct room_sort *next;
	gboolean unread;
	gboolean mention;
	gint64 when;
	ChimeRoom *room;
};

static gboolean cmp_room(struct room_sort *a, struct room_sort *b)
{
	if (a->mention != b->mention)
		return a->mention;
	if (a->unread != b->unread)
		return a->unread;
	if (a->when > b->when)
		return TRUE;
	return FALSE;
}

static void sort_room(ChimeConnection *cxn, ChimeRoom *room, gpointer _rs_list)
{
	struct room_sort **rs_list = _rs_list;
	struct room_sort *rs = g_new0(struct room_sort, 1);
	const char *tm;

	rs->room = room;
	rs->unread = chime_room_has_unread(room);
	rs->mention = chime_room_has_mention(room);

	tm = chime_room_get_last_sent(room);
	if (!tm || !iso8601_to_ms(tm, &rs->when)) {
		tm = chime_room_get_created_on(room);
		if (tm)
			iso8601_to_ms(tm, &rs->when);
	}
	while (*rs_list && cmp_room(*rs_list, rs))
		rs_list = &((*rs_list)->next);
	rs->next = *rs_list;
	*rs_list = rs;
}

PurpleRoomlist *chime_purple_roomlist_get_list(PurpleConnection *conn)
{
	ChimeConnection *cxn = PURPLE_CHIME_CXN(conn);
	PurpleRoomlist *roomlist;
	struct room_sort *rooms = NULL, *tmp_rs;
	GList *fields = NULL;

	roomlist = purple_roomlist_new(conn->account);
	fields = g_list_append(fields, purple_roomlist_field_new(PURPLE_ROOMLIST_FIELD_STRING, "", "RoomId", TRUE));
	fields = g_list_append(fields, purple_roomlist_field_new(PURPLE_ROOMLIST_FIELD_STRING, _("Status"), "Status", FALSE));
	fields = g_list_append(fields, purple_roomlist_field_new(PURPLE_ROOMLIST_FIELD_STRING, _("Last Sent"), "Last Sent", FALSE));
	purple_roomlist_set_fields(roomlist, fields);

	chime_connection_foreach_room(cxn, sort_room, &rooms);

	while (rooms) {
		ChimeRoom *room = rooms->room;
		PurpleRoomlistRoom *proom = purple_roomlist_room_new(PURPLE_ROOMLIST_ROOMTYPE_ROOM,
								     chime_room_get_name(room), NULL);
		purple_roomlist_room_add_field(roomlist, proom, chime_room_get_id(room));
		purple_roomlist_room_add_field(roomlist, proom, rooms->mention ? "@" : (rooms->unread ? "•" : ""));
		purple_roomlist_room_add_field(roomlist, proom, chime_room_get_last_sent(room) ? : chime_room_get_created_on(room));
		purple_roomlist_room_add(roomlist, proom);
		tmp_rs = rooms;
		rooms = rooms->next;
		g_free(tmp_rs);
	}

	purple_roomlist_set_in_progress(roomlist, FALSE);
	return roomlist;
}

gchar *chime_purple_roomlist_room_serialize(PurpleRoomlistRoom *room)
{
	/* We use the RoomId as it *uniquely* identifies the room */
	return g_strdup((char *)room->fields->data);
}

GList *chime_purple_chat_info(PurpleConnection *conn)
{
	struct proto_chat_entry *pce = g_new0(struct proto_chat_entry, 1);
	GList *l;
	pce->label = _("Name:");
	pce->identifier = "Name";
	pce->required = TRUE;

	l = g_list_append(NULL, pce);

	/* Ick. We don't want this to be *shown* but the name alone isn't sufficient
	   because they aren't unique, and there's no way to preserve it otherwise
	   when the chat is added to the buddy list. */
	pce = g_new0(struct proto_chat_entry, 1);
	pce->label = _("Room ID:");
	pce->identifier = "RoomId";
	pce->required = FALSE;

	return g_list_append(l, pce);
}

GHashTable *chime_purple_chat_info_defaults(PurpleConnection *conn, const char *name)
{
	ChimeConnection *cxn = PURPLE_CHIME_CXN(conn);
	GHashTable *hash = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);

	purple_debug_info("chime", "Chat info defaults for '%s'\n", name);
	if (name) {
		ChimeRoom *room = chime_connection_room_by_id(cxn, name);
		if (!room)
			room = chime_connection_room_by_name(cxn, name);

		if (room) {
			g_hash_table_insert(hash, (char *)"Name", g_strdup(chime_room_get_name(room)));
			g_hash_table_insert(hash, (char *)"RoomId", g_strdup(chime_room_get_id(room)));
		}
	}
	return hash;
}

char *chime_purple_get_chat_name(GHashTable *components)
{
	const gchar *name = g_hash_table_lookup(components, (char *)"Name");

	purple_debug_info("chime", "Chat name: %s\n", name);

	return g_strdup(name);
}
