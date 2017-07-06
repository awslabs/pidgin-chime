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

#ifndef __CHIME_ROOM_H__
#define __CHIME_ROOM_H__

#include <glib-object.h>

#include <json-glib/json-glib.h>

#include "chime-connection.h"

G_BEGIN_DECLS

#define CHIME_TYPE_ROOM (chime_room_get_type ())
G_DECLARE_FINAL_TYPE (ChimeRoom, chime_room, CHIME, ROOM, GObject)

#define CHIME_ENUM_VALUE(val, nick) { val, #val, nick },
#define CHIME_DEFINE_ENUM_TYPE(TypeName, type_name, values)		\
	GType type_name ## _get_type(void) {				\
		static volatile gsize chime_define_id__volatile = 0;	\
		if (g_once_init_enter(&chime_define_id__volatile)) {	\
			static const GEnumValue v[] = {			\
				values					\
				{ 0, NULL, NULL },			\
			};						\
			GType chime_define_id = g_enum_register_static(g_intern_static_string(#TypeName), v); \
			g_once_init_leave(&chime_define_id__volatile, chime_define_id); \
		}							\
		return chime_define_id__volatile;			\
	}

typedef enum {
	CHIME_ROOM_TYPE_STANDARD,
	CHIME_ROOM_TYPE_MEETING,
	CHIME_ROOM_TYPE_ORGANIZATION
} ChimeRoomType;

#define CHIME_TYPE_ROOM_TYPE (chime_room_type_get_type ())
GType chime_room_type_get_type (void) G_GNUC_CONST;

typedef enum {
	CHIME_ROOM_NOTIFY_ALWAYS,
	CHIME_ROOM_NOTIFY_DIRECT_ONLY,
	CHIME_ROOM_NOTIFY_NEVER
} ChimeRoomNotifyPref;

#define CHIME_TYPE_ROOM_NOTIFY_PREF (chime_room_notify_pref_get_type ())
GType chime_room_notify_pref_get_type (void) G_GNUC_CONST;

ChimeRoom *chime_connection_room_by_name(ChimeConnection *cxn,
					 const gchar *name);
ChimeRoom *chime_connection_room_by_id(ChimeConnection *cxn,
				       const gchar *id);

/* Designed to match the NEW_ROOM signal handler */
typedef void (*ChimeRoomCB) (ChimeConnection *, ChimeRoom *, gpointer);
void chime_connection_foreach_room(ChimeConnection *cxn, ChimeRoomCB cb,
				      gpointer cbdata);

G_END_DECLS

#endif /* __CHIME_ROOM_H__ */
