/*
 * Pidgin/libpurple Chime client plugin
 *
 * Copyright © 2017 Amazon.com, Inc. or its affiliates.
 *
 * Authors: David Woodhouse <dwmw2@infradead.org>
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

#include "chime-connection.h"
#include "chime-call.h"
#include "chime-connection-private.h"

#include <libsoup/soup.h>

#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>

struct _ChimeCallScreen {
	ChimeCall *call;
	GCancellable *cancel;
	ChimeScreenState state;
	GMutex transport_lock;

	GstAppSrc *screen_src;
	gboolean appsrc_need_data;

	GstAppSink *screen_sink;

	SoupWebsocketConnection *ws;
};

/* Called from ChimeMeeting */
ChimeCallScreen *chime_call_screen_open(ChimeConnection *cxn, ChimeCall *call);
void chime_call_screen_close(ChimeCallScreen *screen);
void chime_call_screen_view(ChimeCallScreen *screen);
void chime_call_screen_unview(ChimeCallScreen *screen);

void chime_call_screen_install_appsrc(ChimeCallScreen *screen, GstAppSrc *appsrc);
void chime_call_screen_install_appsink(ChimeCallScreen *screen, GstAppSink *appsink);
