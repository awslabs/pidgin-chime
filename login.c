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

#include <prpl.h>

#include <glib/gi18n.h>

#include <json-glib/json-glib.h>

#include <libsoup/soup.h>

#include "chime.h"

void chime_initial_login(ChimeConnection *cxn)
{
	purple_connection_error_reason(cxn->prpl_conn, PURPLE_CONNECTION_ERROR_OTHER_ERROR,
				       _("No authentication token"));
}
