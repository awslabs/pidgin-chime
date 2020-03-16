/*
 * Pidgin/libpurple Chime client plugin
 *
 * Copyright © 2020 Amazon.com, Inc. or its affiliates.
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

#include <dbus-server.h>
#include <account.h>

extern PurpleDBusBinding chime_purple_dbus_bindings[];

/**
 * ChimeAddJoinableMeeting - Add joinable meeting using PIN
 *
 * @param account   (in) libpurple account
 * @param pin       (in) meeting PIN or URL
 */
DBUS_EXPORT void chime_add_joinable_meeting(PurpleAccount *account,
					    const gchar *pin);
