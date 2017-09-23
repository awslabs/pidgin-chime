/*
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 *
 * Authors:
 *		Michael Zucchi <notzed@novell.com>
 *		Rodrigo Moya <rodrigo@novell.com>
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 */

/* Convert a mail message into a task */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <string.h>
#include <glib/gi18n.h>

#include <libecal/libecal.h>

#include <shell/e-shell-view.h>
#include <shell/e-shell-window-actions.h>

#include <mail/message-list.h>

#include <calendar/gui/calendar-config.h>
#include <calendar/gui/dialogs/comp-editor.h>
#include <calendar/gui/dialogs/event-editor.h>

static CompEditor *
get_component_editor (EShell *shell,
                      ECalClient *client,
                      ECalComponent *comp)
{
	ECalComponentId *id;
	CompEditorFlags flags = 0;
	CompEditor *editor = NULL;
	ESourceRegistry *registry;

	g_return_val_if_fail (E_IS_SHELL (shell), NULL);
	g_return_val_if_fail (E_IS_CAL_CLIENT (client), NULL);
	g_return_val_if_fail (E_IS_CAL_COMPONENT (comp), NULL);

	registry = e_shell_get_registry (shell);

	id = e_cal_component_get_id (comp);
	g_return_val_if_fail (id != NULL, NULL);
	g_return_val_if_fail (id->uid != NULL, NULL);

	flags |= COMP_EDITOR_NEW_ITEM | COMP_EDITOR_MEETING;

	if (itip_organizer_is_user (registry, comp, client))
		flags |= COMP_EDITOR_USER_ORG;

	editor = event_editor_new (client, shell, flags);

	if (editor) {
		event_editor_show_meeting (EVENT_EDITOR (editor));

		comp_editor_edit_comp (editor, comp);

		/* request save for new events */
		comp_editor_set_changed (editor, TRUE);
	}

	e_cal_component_free_id (id);

	return editor;
}

static void
set_attendees (ECalComponent *comp,
               CamelInternetAddress *addresses)
{
	GSList *attendees = NULL, *to_free = NULL;
	ECalComponentAttendee *ca;
	gint len, i;

	len = CAMEL_ADDRESS (addresses)->addresses->len;
	for (i = 0; i < len; i++) {
		const gchar *name, *addr;

		if (camel_internet_address_get (addresses, i, &name, &addr)) {
			gchar *temp;

			temp = g_strconcat ("mailto:", addr, NULL);

			ca = g_new0 (ECalComponentAttendee, 1);

			ca->value = temp;
			ca->cn = name;
			ca->cutype = ICAL_CUTYPE_INDIVIDUAL;
			ca->status = ICAL_PARTSTAT_NEEDSACTION;
			ca->role = ICAL_ROLE_REQPARTICIPANT;

			to_free = g_slist_prepend (to_free, temp);

			attendees = g_slist_append (attendees, ca);
		}
	}

	e_cal_component_set_attendee_list (comp, attendees);

	g_slist_foreach (attendees, (GFunc) g_free, NULL);
	g_slist_foreach (to_free, (GFunc) g_free, NULL);

	g_slist_free (to_free);
	g_slist_free (attendees);
}


struct _report_error
{
	gchar *format;
	gchar *param;
};

static gboolean
do_report_error (struct _report_error *err)
{
	if (err) {
		e_notice (NULL, GTK_MESSAGE_ERROR, err->format, err->param);
		g_free (err->format);
		g_free (err->param);
		g_free (err);
	}

	return FALSE;
}

static void
report_error_idle (const gchar *format,
                   const gchar *param)
{
	struct _report_error *err = g_new (struct _report_error, 1);

	err->format = g_strdup (format);
	if (param)
		err->param = g_strdup (param);
	else err->param = NULL;

	g_usleep (250);
	g_idle_add ((GSourceFunc) do_report_error, err);
}


typedef struct {
	ECalComponent *comp;
}AsyncData;

static void got_client_cb(GObject *object, GAsyncResult *res, gpointer user_data)
{
	AsyncData *data = user_data;
	GError *error = NULL;
	EClient *client = e_client_cache_get_client_finish(E_CLIENT_CACHE(object), res, &error);

	if (!client) {
		report_error_idle (_("Cannot open calendar. %s"), error->message);
		g_error_free(error);
		return;
	}


	CompEditor *editor = get_component_editor (e_shell_get_default(),
						   E_CAL_CLIENT(client), data->comp);

	if (editor)
		gtk_window_present(GTK_WINDOW(editor));
	else
		report_error_idle (_("Cannot create calendar editor"), NULL);

	g_object_unref (client);

	g_object_unref (data->comp);
	g_free (data);
	data = NULL;
}

static ECalComponent *generate_comp(const gchar *organizer, const gchar *location,
				    const gchar *summary, const gchar *description,
				    GSList *attendees)
{
	ECalComponentDateTime dt, dt2;
	struct icaltimetype tt, tt2;
	icaltimezone *tz = calendar_config_get_icaltimezone();

	tt = icaltime_current_time_with_zone (tz);
	/* Round up to the next half hour */
	icaltime_adjust (&tt, 0, 0, 30 - (tt.minute % 30), -tt.second);

	dt.value = &tt;
	dt.tzid = icaltimezone_get_tzid(tz);

	tt2 = tt;
	icaltime_adjust (&tt2, 0, 0, 30, 0);
	dt2.value = &tt2;
	dt2.tzid = icaltimezone_get_tzid(tz);

	ECalComponent *comp;
	ECalComponentText text;
	icalproperty *icalprop;
	icalcomponent *icalcomp;

	comp = e_cal_component_new ();

	e_cal_component_set_new_vtype (comp, E_CAL_COMPONENT_EVENT);
	e_cal_component_set_dtstart (comp, &dt);
	e_cal_component_set_dtend (comp, &dt2);

	/* set the summary */
	if (summary) {
		text.value = summary;
		text.altrep = NULL;
		e_cal_component_set_summary (comp, &text);
	}

	if (location) {
		text.value = location;
		text.altrep = NULL;
		e_cal_component_set_location (comp, location);
	}

	if (description) {
		text.value = description;
		GSList *sl = g_slist_append (NULL, &text);
		e_cal_component_set_description_list (comp, sl);
		g_slist_free(sl);
	}

	if (organizer) {
		ECalComponentOrganizer e_organizer = {NULL, NULL, NULL, NULL};
		gchar *mailto = g_strconcat ("mailto:", organizer, NULL);
		e_organizer.value = mailto;
		e_organizer.cn = NULL;
		e_cal_component_set_organizer (comp, &e_organizer);
		g_free(mailto);
	}

	CamelInternetAddress *addresses = camel_internet_address_new();
	if (organizer)
		camel_internet_address_add(addresses, NULL, organizer);
	GSList *l = attendees;
	while (l) {
		camel_internet_address_add(addresses, NULL, l->data);
		l = l->next;
	}
	set_attendees (comp, addresses);
	g_object_unref(addresses);

	/* no need to increment a sequence number, this is a new component */
	e_cal_component_abort_sequence (comp);

	icalcomp = e_cal_component_get_icalcomponent (comp);

	icalprop = icalproperty_new_x ("1");
	icalproperty_set_x_name (icalprop, "X-EVOLUTION-MOVE-CALENDAR");
	icalcomponent_add_property (icalcomp, icalprop);

	return comp;
}

enum goodness {
	MATCH_NONE,
	MATCH_DEFAULT,
	MATCH_PARENT_RO,
	MATCH_PARENT_RW,
	MATCH_SOURCE_RO,
	MATCH_SOURCE_RW
};

static gboolean
mail_to_event (EShell *shell)
{
	ESourceRegistry *registry;
	ESource *source = NULL;
	const gchar *organizer = "dwmw@amazon.co.uk";
	registry = e_shell_get_registry (shell);
	int match = MATCH_DEFAULT;

	source = e_source_registry_ref_default_calendar (registry);

	GList *list, *iter;
	list = e_source_registry_list_sources (registry, E_SOURCE_EXTENSION_CALENDAR);
	for (iter = list; iter != NULL; iter = g_list_next (iter)) {
		ESource *candidate = E_SOURCE (iter->data);
		int cand_match = MATCH_NONE;
		ESource *parent = e_source_registry_ref_source(registry, e_source_get_parent(candidate));
		if (e_source_has_extension(parent, E_SOURCE_EXTENSION_COLLECTION)) {
			ESourceCollection *coll = e_source_get_extension(parent, E_SOURCE_EXTENSION_COLLECTION);
			printf("Parent idetn %s\n", e_source_collection_get_identity(coll));
		}

		if (!strcmp(e_source_get_display_name(candidate), organizer)) {
			if (e_source_get_writable(candidate))
				cand_match = MATCH_SOURCE_RW;
			else
				cand_match = MATCH_SOURCE_RO;
		} else if (match < MATCH_SOURCE_RO) {
			/* Match parent by display name or collecction identity */
			ESource *parent = e_source_registry_ref_source(registry, e_source_get_parent(candidate));
			if (!strcmp(e_source_get_display_name(parent), organizer) ||
			    (e_source_has_extension(parent, E_SOURCE_EXTENSION_COLLECTION) &&
			     strcmp(organizer,
				    e_source_collection_get_identity(
					e_source_get_extension(parent,
							       E_SOURCE_EXTENSION_COLLECTION))))) {
				if (e_source_get_writable(candidate))
					cand_match = MATCH_PARENT_RW;
				else
					cand_match = MATCH_PARENT_RO;
			}
		}
		if (cand_match > match) {
			g_object_unref(source);
			source = g_object_ref(candidate);
			match = cand_match;
			if (match == MATCH_SOURCE_RW)
				break;
		}
	}
	g_list_free_full (list, (GDestroyNotify) g_object_unref);

	AsyncData *data = g_new0 (AsyncData, 1);
	EClientCache *client_cache = e_shell_get_client_cache (shell);

	GSList *attendees = g_slist_append(NULL, (void *)"meet@chime.aws");
	attendees = g_slist_append(attendees, (void *)"pin+12345678@chime.aws");
	data->comp = generate_comp(organizer, "Chime PIN:123456788", "Something", "This is a Chime meeting blah blah blah", attendees);
	g_slist_free(attendees);

	e_client_cache_get_client(client_cache, source,
				  E_SOURCE_EXTENSION_CALENDAR, 1, NULL, got_client_cb,
				  data);

	g_object_unref (source);
	return FALSE;
}

/* Standard GObject macros */
#define E_TYPE_EVENT_TEMPLATE_HANDLER \
        (e_event_template_handler_get_type ())
#define E_EVENT_TEMPLATE_HANDLER(obj) \
        (G_TYPE_CHECK_INSTANCE_CAST \
        ((obj), E_TYPE_EVENT_TEMPLATE_HANDLER, EEventTemplateHandler))

typedef struct _EEventTemplateHandler EEventTemplateHandler;
typedef struct _EEventTemplateHandlerClass EEventTemplateHandlerClass;

struct _EEventTemplateHandler {
        EExtension parent;
};

struct _EEventTemplateHandlerClass {
        EExtensionClass parent_class;
};

/* Module Entry Points */
void e_module_load (GTypeModule *type_module);
void e_module_unload (GTypeModule *type_module);

/* Forward Declarations */
GType e_event_template_handler_get_type (void);

G_DEFINE_DYNAMIC_TYPE (EEventTemplateHandler, e_event_template_handler, E_TYPE_EXTENSION)

static EShell *
event_template_handler_get_shell (EEventTemplateHandler *extension)
{
        EExtensible *extensible;

        extensible = e_extension_get_extensible (E_EXTENSION (extension));

        return E_SHELL (extensible);
}


static void
event_template_handler_listen (EEventTemplateHandler *extension)
{
	printf("NOW LISTENING FOR EVENT TEMPLATES (not)\n");
	g_timeout_add(5000, (GSourceFunc)mail_to_event, event_template_handler_get_shell(extension));
}

static void
event_template_handler_constructed (GObject *object)
{
        EShell *shell;
        EEventTemplateHandler *extension;

        extension = E_EVENT_TEMPLATE_HANDLER (object);

        shell = event_template_handler_get_shell (extension);

        g_signal_connect_swapped (
                shell, "event::ready-to-start",
                G_CALLBACK (event_template_handler_listen), extension);

        /* Chain up to parent's constructed() method. */
        G_OBJECT_CLASS (e_event_template_handler_parent_class)->constructed (object);
}

static void
e_event_template_handler_class_init (EEventTemplateHandlerClass *class)
{
        GObjectClass *object_class;
        EExtensionClass *extension_class;

        object_class = G_OBJECT_CLASS (class);
        object_class->constructed = event_template_handler_constructed;

        extension_class = E_EXTENSION_CLASS (class);
        extension_class->extensible_type = E_TYPE_SHELL;
}

static void
e_event_template_handler_class_finalize (EEventTemplateHandlerClass *class)
{
}

static void
e_event_template_handler_init (EEventTemplateHandler *extension)
{
}

G_MODULE_EXPORT void
e_module_load (GTypeModule *type_module)
{
        e_event_template_handler_register_type (type_module);
}

G_MODULE_EXPORT void
e_module_unload (GTypeModule *type_module)
{
}
