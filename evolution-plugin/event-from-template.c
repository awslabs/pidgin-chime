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

#include <mail/e-mail-browser.h>
#include <mail/em-utils.h>
#include <mail/message-list.h>

#include <calendar/gui/dialogs/comp-editor.h>
#include <calendar/gui/dialogs/event-editor.h>
#include <calendar/gui/dialogs/memo-editor.h>
#include <calendar/gui/dialogs/task-editor.h>

gboolean	mail_browser_init		(GtkUIManager *ui_manager,
						 EMailBrowser *browser);
gboolean	mail_shell_view_init		(GtkUIManager *ui_manager,
						 EShellView *shell_view);

static CompEditor *
get_component_editor (EShell *shell,
                      ECalClient *client,
                      ECalComponent *comp,
                      gboolean is_new,
                      GError **error)
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

	if (is_new) {
		flags |= COMP_EDITOR_NEW_ITEM;
	} else {
		editor = comp_editor_find_instance (id->uid);
	}

	if (!editor) {
		if (itip_organizer_is_user (registry, comp, client))
			flags |= COMP_EDITOR_USER_ORG;

		if (e_cal_component_has_attendees (comp))
				flags |= COMP_EDITOR_MEETING;

		editor = event_editor_new (client, shell, flags);

		if (flags & COMP_EDITOR_MEETING)
			event_editor_show_meeting (EVENT_EDITOR (editor));

		if (editor) {
			comp_editor_edit_comp (editor, comp);

			/* request save for new events */
			comp_editor_set_changed (editor, is_new);
		}
	}

	e_cal_component_free_id (id);

	return editor;
}

static void
set_attendees (ECalComponent *comp,
               CamelMimeMessage *message,
               const gchar *organizer)
{
	GSList *attendees = NULL, *to_free = NULL;
	ECalComponentAttendee *ca;
	CamelInternetAddress *from = NULL, *to, *cc, *bcc, *arr[4];
	gint len, i, j;

	if (message->reply_to)
		from = message->reply_to;
	else if (message->from)
		from = message->from;

	to = camel_mime_message_get_recipients (message, CAMEL_RECIPIENT_TYPE_TO);
	cc = camel_mime_message_get_recipients (message, CAMEL_RECIPIENT_TYPE_CC);
	bcc = camel_mime_message_get_recipients (message, CAMEL_RECIPIENT_TYPE_BCC);

	arr[0] = from; arr[1] = to; arr[2] = cc; arr[3] = bcc;

	for (j = 0; j < 4; j++) {
		if (!arr[j])
			continue;

		len = CAMEL_ADDRESS (arr[j])->addresses->len;
		for (i = 0; i < len; i++) {
			const gchar *name, *addr;

			if (camel_internet_address_get (arr[j], i, &name, &addr)) {
				gchar *temp;

				temp = g_strconcat ("mailto:", addr, NULL);
				if (organizer && g_ascii_strcasecmp (temp, organizer) == 0) {
					/* do not add organizer twice */
					g_free (temp);
					continue;
				}

				ca = g_new0 (ECalComponentAttendee, 1);

				ca->value = temp;
				ca->cn = name;
				ca->cutype = ICAL_CUTYPE_INDIVIDUAL;
				ca->status = ICAL_PARTSTAT_NEEDSACTION;
				if (j == 0) {
					/* From */
					ca->role = ICAL_ROLE_CHAIR;
				} else if (j == 2) {
					/* BCC  */
					ca->role = ICAL_ROLE_OPTPARTICIPANT;
				} else {
					/* all other */
					ca->role = ICAL_ROLE_REQPARTICIPANT;
				}

				to_free = g_slist_prepend (to_free, temp);

				attendees = g_slist_append (attendees, ca);
			}
		}
	}

	e_cal_component_set_attendee_list (comp, attendees);

	g_slist_foreach (attendees, (GFunc) g_free, NULL);
	g_slist_foreach (to_free, (GFunc) g_free, NULL);

	g_slist_free (to_free);
	g_slist_free (attendees);
}

static const gchar *
prepend_from (CamelMimeMessage *message,
              gchar **text)
{
	gchar *res, *tmp, *addr = NULL;
	const gchar *name = NULL, *eml = NULL;
	CamelInternetAddress *from = NULL;

	g_return_val_if_fail (message != NULL, NULL);
	g_return_val_if_fail (text != NULL, NULL);

	if (message->reply_to)
		from = message->reply_to;
	else if (message->from)
		from = message->from;

	if (from && camel_internet_address_get (from, 0, &name, &eml))
		addr = camel_internet_address_format_address (name, eml);

	/* To Translators: The full sentence looks like: "Created from a mail by John Doe <john.doe@myco.example>" */
	tmp = g_strdup_printf (_("Created from a mail by %s"), addr ? addr : "");

	res = g_strconcat (tmp, "\n", *text, NULL);

	g_free (tmp);
	g_free (*text);

	*text = res;

	return res;
}
static gchar *
set_organizer (ECalComponent *comp,
               CamelFolder *folder)
{
	EShell *shell;
	ESource *source = NULL;
	ESourceRegistry *registry;
	ESourceMailIdentity *extension;
	const gchar *extension_name;
	const gchar *address, *name;
	ECalComponentOrganizer organizer = {NULL, NULL, NULL, NULL};
	gchar *mailto = NULL;

	shell = e_shell_get_default ();
	registry = e_shell_get_registry (shell);

	if (folder != NULL) {
		CamelStore *store;

		store = camel_folder_get_parent_store (folder);
		source = em_utils_ref_mail_identity_for_store (registry, store);
	}

	if (source == NULL)
		source = e_source_registry_ref_default_mail_identity (registry);

	g_return_val_if_fail (source != NULL, NULL);

	extension_name = E_SOURCE_EXTENSION_MAIL_IDENTITY;
	extension = e_source_get_extension (source, extension_name);

	name = e_source_mail_identity_get_name (extension);
	address = e_source_mail_identity_get_address (extension);

	if (name != NULL && address != NULL) {
		mailto = g_strconcat ("mailto:", address, NULL);
		organizer.value = mailto;
		organizer.cn = name;
		e_cal_component_set_organizer (comp, &organizer);
	}

	g_object_unref (source);

	return mailto;
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
	err->param = g_strdup (param);

	g_usleep (250);
	g_idle_add ((GSourceFunc) do_report_error, err);
}

struct _manage_comp
{
	ECalClient *client;
	ECalComponent *comp;
	GCond cond;
	GMutex mutex;
	gint mails_count;
	gint mails_done;
	gchar *editor_title;
};

static void
free_manage_comp_struct (struct _manage_comp *mc)
{
	g_return_if_fail (mc != NULL);

	g_object_unref (mc->comp);
	g_object_unref (mc->client);
	g_mutex_clear (&mc->mutex);
	g_cond_clear (&mc->cond);
	if (mc->editor_title)
		g_free (mc->editor_title);

	g_free (mc);
}

static void
comp_editor_closed (CompEditor *editor,
                    gboolean accepted,
                    struct _manage_comp *mc)
{
	if (!mc)
		return;

	/* Signal the do_mail_to_event thread that editor was closed and editor
	 * for next event can be displayed (if any) */
	g_cond_signal (&mc->cond);
}

/*
 * This handler takes title of the editor window and
 * inserts information about number of processed mails and
 * number of all mails to process, so the window title
 * will look like "Appointment (3/10) - An appoitment name"
 */
static void
comp_editor_title_changed (GtkWidget *widget,
                           GParamSpec *pspec,
                           struct _manage_comp *mc)
{
	GtkWindow *editor = GTK_WINDOW (widget);
	const gchar *title = gtk_window_get_title (editor);
	gchar *new_title;
	gchar *splitter;
	gchar *comp_name, *task_name;

	if (!mc)
		return;

	/* Recursion prevence */
	if (mc->editor_title && g_utf8_collate (mc->editor_title, title) == 0)
		return;

	splitter = strchr (title, '-');
	if (!splitter)
		return;

	comp_name = g_strndup (title, splitter - title - 1);
	task_name = g_strdup (splitter + 2);
	new_title = g_strdup_printf (
		"%s (%d/%d) - %s",
		comp_name, mc->mails_done, mc->mails_count, task_name);

	/* Remember the new title, so that when gtk_window_set_title() causes
	 * this handler to be recursively called, we can recognize that and
	 * prevent endless recursion */
	if (mc->editor_title)
		g_free (mc->editor_title);
	mc->editor_title = new_title;

	gtk_window_set_title (editor, new_title);

	g_free (comp_name);
	g_free (task_name);
}

static gboolean
do_manage_comp_idle (struct _manage_comp *mc)
{
	GError *error = NULL;
	ECalClientSourceType source_type = E_CAL_CLIENT_SOURCE_TYPE_LAST;
	ECalComponent *edit_comp = NULL;

	g_return_val_if_fail (mc, FALSE);

	source_type = e_cal_client_get_source_type (mc->client);

	if (source_type == E_CAL_CLIENT_SOURCE_TYPE_LAST) {
		free_manage_comp_struct (mc);

		g_warning ("mail-to-task: Incorrect call of %s, no data given", G_STRFUNC);
		return FALSE;
	}

	edit_comp = mc->comp;

	if (edit_comp) {
		EShell *shell;
		CompEditor *editor;

		/* FIXME Pass in the EShell instance. */
		shell = e_shell_get_default ();
		editor = get_component_editor (
			shell, mc->client, edit_comp,
			edit_comp == mc->comp, &error);

		if (editor && !error) {
			/* Force editor's title change */
			comp_editor_title_changed (GTK_WIDGET (editor), NULL, mc);

			e_signal_connect_notify (
				editor, "notify::title",
				G_CALLBACK (comp_editor_title_changed), mc);
			g_signal_connect (
				editor, "comp_closed",
				G_CALLBACK (comp_editor_closed), mc);

			gtk_window_present (GTK_WINDOW (editor));

			if (edit_comp != mc->comp)
				g_object_unref (edit_comp);
		} else {
			g_warning ("Failed to create event editor: %s", error ? error->message : "Unknown error");
			g_cond_signal (&mc->cond);
		}
	} else {
		/* User canceled editing already existing event, so
		 * treat it as if he just closed the editor window. */
		comp_editor_closed (NULL, FALSE, mc);
	}

	if (error != NULL) {
		e_notice (
			NULL, GTK_MESSAGE_ERROR,
			_("An error occurred during processing: %s"),
			error->message);
		g_clear_error (&error);
	}

	return FALSE;
}

typedef struct {
	EClientCache *client_cache;
	ESource *source;
	const gchar *extension_name;
	ECalClientSourceType source_type;
	CamelFolder *folder;
	gchar *selected_text;
	gboolean with_attendees;
}AsyncData;

static gboolean
do_mail_to_event (AsyncData *data)
{
	EClient *client;
	GError *error = NULL;

	client = e_client_cache_get_client_sync (data->client_cache,
		data->source, data->extension_name, 30, NULL, &error);

	/* Sanity check. */
	g_return_val_if_fail (
		((client != NULL) && (error == NULL)) ||
		((client == NULL) && (error != NULL)), TRUE);

	if (error != NULL) {
		report_error_idle (_("Cannot open calendar. %s"), error->message);
	} else if (e_client_is_readonly (E_CLIENT (client))) {
		switch (data->source_type) {
		case E_CAL_CLIENT_SOURCE_TYPE_EVENTS:
			report_error_idle (_("Selected calendar is read only, thus cannot create event there. Select other calendar, please."), NULL);
			break;
		case E_CAL_CLIENT_SOURCE_TYPE_TASKS:
			report_error_idle (_("Selected task list is read only, thus cannot create task there. Select other task list, please."), NULL);
			break;
		case E_CAL_CLIENT_SOURCE_TYPE_MEMOS:
			report_error_idle (_("Selected memo list is read only, thus cannot create memo there. Select other memo list, please."), NULL);
			break;
		default:
			g_warn_if_reached ();
			break;
		}
	} else {
		gint i;
		ECalComponentDateTime dt, dt2;
		struct icaltimetype tt, tt2;
		struct _manage_comp *oldmc = NULL;

		#define cache_backend_prop(prop) { \
			gchar *val = NULL; \
			e_client_get_backend_property_sync (E_CLIENT (client), prop, &val, NULL, NULL); \
			g_free (val); \
		}

		/* precache backend properties, thus editor have them ready when needed */
		cache_backend_prop (CAL_BACKEND_PROPERTY_CAL_EMAIL_ADDRESS);
		cache_backend_prop (CAL_BACKEND_PROPERTY_ALARM_EMAIL_ADDRESS);
		cache_backend_prop (CAL_BACKEND_PROPERTY_DEFAULT_OBJECT);
		e_client_get_capabilities (E_CLIENT (client));

		#undef cache_backend_prop

		/* set start day of the event as today, without time - easier than looking for a calendar's time zone */
		tt = icaltime_today ();
		dt.value = &tt;
		dt.tzid = NULL;

		tt2 = tt;
		icaltime_adjust (&tt2, 1, 0, 0, 0);
		dt2.value = &tt2;
		dt2.tzid = NULL;

		if (1) {
			ECalComponent *comp;
			ECalComponentText text;
			icalproperty *icalprop;
			icalcomponent *icalcomp;
			struct _manage_comp *mc;

			comp = e_cal_component_new ();

			e_cal_component_set_new_vtype (comp, E_CAL_COMPONENT_EVENT);
                        e_cal_component_set_dtstart (comp, &dt);

			e_cal_component_set_dtend (comp, &dt2);

			/* set the summary */
			text.value = "test subject";
			text.altrep = NULL;
			e_cal_component_set_summary (comp, &text);

			text.value = "Chime Meeting";
			GSList *sl = g_slist_append (NULL, &text);
			e_cal_component_set_description_list (comp, sl);
			g_slist_free(sl);

			if (data->with_attendees) {
				gchar *organizer;

				/* set actual user as organizer, to be able to change event's properties */
				//organizer = set_organizer (comp, data->folder);
				//set_attendees (comp, message, organizer);
				//				g_free (organizer);
			}

			/* no need to increment a sequence number, this is a new component */
			e_cal_component_abort_sequence (comp);

			icalcomp = e_cal_component_get_icalcomponent (comp);

			icalprop = icalproperty_new_x ("1");
			icalproperty_set_x_name (icalprop, "X-EVOLUTION-MOVE-CALENDAR");
			icalcomponent_add_property (icalcomp, icalprop);

			mc = g_new0 (struct _manage_comp, 1);
			mc->client = g_object_ref (client);
			mc->comp = g_object_ref (comp);
			g_mutex_init (&mc->mutex);
			g_cond_init (&mc->cond);
			mc->mails_count = 1;
			mc->mails_done = i + 1; /* Current task */
			mc->editor_title = NULL;

			/* Prioritize ahead of GTK+ redraws. */
			g_idle_add_full (
				G_PRIORITY_HIGH_IDLE,
				(GSourceFunc) do_manage_comp_idle, mc, NULL);

			oldmc = mc;

			g_object_unref (comp);
		}
	done:
		/* Wait for the last editor and then clean up */
		if (oldmc) {
			g_mutex_lock (&oldmc->mutex);
			g_cond_wait (&oldmc->cond, &oldmc->mutex);
			g_mutex_unlock (&oldmc->mutex);
			free_manage_comp_struct (oldmc);
		}
	}

	/* free memory */
	if (client != NULL)
		g_object_unref (client);

	g_object_unref (data->client_cache);
	g_object_unref (data->source);
	g_free (data->selected_text);
	g_free (data);
	data = NULL;

	if (error != NULL)
		g_error_free (error);

	return TRUE;
}

static gboolean
text_contains_nonwhitespace (const gchar *text,
                             gint len)
{
	const gchar *p;
	gunichar c = 0;

	if (!text || len <= 0)
		return FALSE;

	p = text;

	while (p && p - text < len) {
		c = g_utf8_get_char (p);
		if (!c)
			break;

		if (!g_unichar_isspace (c))
			break;

		p = g_utf8_next_char (p);
	}

	return p - text < len - 1 && c != 0;
}

/* should be freed with g_free after done with it */
static gchar *
get_selected_text (EMailReader *reader)
{
	EMailDisplay *display;
	gchar *text = NULL;

	display = e_mail_reader_get_mail_display (reader);

	if (!e_web_view_is_selection_active (E_WEB_VIEW (display)))
		return NULL;

	text = e_mail_display_get_selection_plain_text (display);

	if (text == NULL || !text_contains_nonwhitespace (text, strlen (text))) {
		g_free (text);
		return NULL;
	}

	return text;
}

static gboolean
mail_to_event (EShell *shell)
{
	EMailBackend *backend;
	ESourceRegistry *registry;
	ESource *source = NULL;
	ESource *default_source;
	GList *list, *iter;
	const gchar *extension_name;

	registry = e_shell_get_registry (shell);

	extension_name = E_SOURCE_EXTENSION_CALENDAR;
	default_source = e_source_registry_ref_default_calendar (registry);

	list = e_source_registry_list_sources (registry, extension_name);

	/* If there is only one writable source, no need to prompt the user. */
	for (iter = list; iter != NULL; iter = g_list_next (iter)) {
		ESource *candidate = E_SOURCE (iter->data);

		if (e_source_get_writable (candidate)) {
			if (source == NULL)
				source = candidate;
			else {
				source = NULL;
				break;
			}
		}
	}

	g_list_free_full (list, (GDestroyNotify) g_object_unref);

	if (source == NULL) {
		GtkWidget *dialog;
		ESourceSelector *selector;

		/* ask the user which tasks list to save to */
		dialog = e_source_selector_dialog_new (
			NULL, registry, extension_name);

		selector = e_source_selector_dialog_get_selector (
			E_SOURCE_SELECTOR_DIALOG (dialog));

		e_source_selector_set_primary_selection (
			selector, default_source);

		if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_OK)
			source = e_source_selector_dialog_peek_primary_selection (
				E_SOURCE_SELECTOR_DIALOG (dialog));

		gtk_widget_destroy (dialog);
	}

	if (source) {
		/* if a source has been selected, perform the mail2event operation */
		AsyncData *data = NULL;
		GThread *thread = NULL;
		GError *error = NULL;

		/* Fill the elements in AsynData */
		data = g_new0 (AsyncData, 1);
                data->client_cache = g_object_ref (e_shell_get_client_cache (shell));
		data->source = g_object_ref (source);
		data->extension_name = extension_name;

		thread = g_thread_try_new (
			NULL, (GThreadFunc) do_mail_to_event, data, &error);
		if (error != NULL) {
			g_warning (G_STRLOC ": %s", error->message);
			g_error_free (error);
		} else {
			g_thread_unref (thread);
		}
	}

	g_object_unref (default_source);
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
	g_timeout_add(10, (GSourceFunc)mail_to_event, event_template_handler_get_shell(extension));
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
