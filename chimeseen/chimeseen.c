/*
 * Pidgin/libpurple Chime client plugin
 *
 * Copyright © 2018 Amazon.com, Inc. or its affiliates.
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

#define PURPLE_PLUGINS

#include <glib.h>
#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <debug.h>
#include <version.h>
#include <request.h>
#include <stdlib.h>
#include <errno.h>

#include "gtkplugin.h"
#include "gtkutils.h"
#include "gtkimhtml.h"

#include <json-glib/json-glib.h>

struct conv_data {
	GList *l;
};

struct msg_mark {
	gint64 created;
	GtkTextMark *mark;
};

static gboolean parse_string(JsonNode *parent, const gchar *name, const gchar **res)
{
        JsonObject *obj;
        JsonNode *node;
        const gchar *str;

        if (!parent)
                return FALSE;

        obj = json_node_get_object(parent);
        if (!obj)
                return FALSE;

        node = json_object_get_member(obj, name);
        if (!node)
                return FALSE;

        str = json_node_get_string(node);
        if (!str)
                return FALSE;

        *res = str;

        return TRUE;
}

gboolean iso8601_to_ms(const gchar *str, gint64 *ms)
{
	/* I *believe* this doesn't leak a new one every time! */
	GTimeZone *utc = g_time_zone_new_utc();
	GDateTime *dt;

	dt = g_date_time_new_from_iso8601(str, utc);
	if (!dt)
		return FALSE;

	*ms = (g_date_time_to_unix(dt) * 1000) +
		(g_date_time_get_microsecond(dt) / 1000000);

	g_date_time_unref(dt);
	return TRUE;
}

static void
conv_seen_cb(PurpleConversation *conv, JsonNode *member)
{
	const gchar *lastread;
	gint64 ms;

	if (!parse_string(member, "LastRead", &lastread) ||
	    !iso8601_to_ms(lastread, &ms))
		return;

	struct conv_data *cd = purple_conversation_get_data(conv, "chime-seen");
	if (!cd)
		return;

	while (cd->l) {
		struct msg_mark *m = cd->l->data;

		if (ms < m->created)
			break;

		cd->l = g_list_remove(cd->l, m);

		GtkIMHtml *imhtml = GTK_IMHTML(PIDGIN_CONVERSATION(conv)->imhtml);
		GtkTextIter iter;
		gtk_text_buffer_get_iter_at_mark(imhtml->text_buffer,
						 &iter, m->mark);
		gtk_text_buffer_insert(imhtml->text_buffer,
				       &iter, "  ✓", -1);
		gtk_text_buffer_delete_mark(imhtml->text_buffer, m->mark);
		g_free(m);
	}
}

static void
got_convmsg_cb(PurpleConversation *conv, gboolean outbound, JsonNode *msgnode)
{
	const gchar *created_on, *msgid;

	if (!outbound || !parse_string(msgnode, "MessageId", &msgid) ||
	    !parse_string(msgnode, "CreatedOn", &created_on))
		return;

	struct msg_mark *m = g_new0(struct msg_mark, 1);

	if (!iso8601_to_ms(created_on, &m->created)) {
		g_free(m);
		return;
	}

	GtkIMHtml *imhtml = GTK_IMHTML(PIDGIN_CONVERSATION(conv)->imhtml);
	GtkTextIter end;

	gtk_text_buffer_get_end_iter(imhtml->text_buffer, &end);

	m->mark = gtk_text_buffer_create_mark(imhtml->text_buffer,
					      msgid, &end, TRUE);

	struct conv_data *cd = purple_conversation_get_data(conv, "chime-seen");
	if (!cd) {
		cd = g_new0(struct conv_data, 1);
		purple_conversation_set_data(conv, "chime-seen", cd);
	};
	cd->l = g_list_append(cd->l, m);
}

static void
deleting_conv_cb(PurpleConversation *conv)
{
	struct conv_data *cd = purple_conversation_get_data(conv, "chime-seen");
	if (!cd)
		return;

	/* The marks themselves will die with the GtkTextBuffer */
	g_list_free_full(cd->l, g_free);
	g_free(cd);
	purple_conversation_set_data(conv, "chime-seen", NULL);
}


static gboolean
chimeseen_plugin_load(PurplePlugin *plugin)
{
	PurplePlugin *chimeprpl = purple_find_prpl("prpl-chime");
	if (!chimeprpl)
		return FALSE;

	purple_signal_connect(chimeprpl, "chime-got-convmsg", plugin,
			      PURPLE_CALLBACK(got_convmsg_cb), NULL);
	purple_signal_connect(chimeprpl, "chime-conv-membership", plugin,
			      PURPLE_CALLBACK(conv_seen_cb), NULL);
	purple_signal_connect(pidgin_conversations_get_handle(),
			      "deleting-conversation", plugin,
			      PURPLE_CALLBACK(deleting_conv_cb), NULL);
	return TRUE;
}

static gboolean
chimeseen_plugin_unload(PurplePlugin *plugin)
{
	purple_signals_disconnect_by_handle(plugin);

	GList *ims;
	for (ims = purple_get_ims(); ims; ims = ims->next)
		deleting_conv_cb(ims->data);

	return TRUE;
}

static void
chimeseen_plugin_destroy(PurplePlugin *plugin)
{
}


static PurplePluginInfo info =
{
	.magic = PURPLE_PLUGIN_MAGIC,
	.major_version = PURPLE_MAJOR_VERSION,
	.minor_version = PURPLE_MINOR_VERSION,
	.type = PURPLE_PLUGIN_STANDARD,
	.ui_requirement = PIDGIN_PLUGIN_TYPE,
	.priority = PURPLE_PRIORITY_DEFAULT,
	.id = (char *)"chimeseen",
	.name = (char *)"Chime Seen",
	.version = PACKAGE_VERSION,
	.summary = (char *)"Chime seen message handling",
	.description = (char *)"Displays when messages are seen",
	.author = (char *)"David Woodhouse <dwmw2@infradead.org>",
	.load = chimeseen_plugin_load,
	.unload = chimeseen_plugin_unload,
	.destroy = chimeseen_plugin_destroy,
};

static void
init_plugin(PurplePlugin *plugin)
{
	info.dependencies = g_list_append(NULL, "prpl-chime");
}

PURPLE_INIT_PLUGIN(chimeseen_plugin, init_plugin, info)
