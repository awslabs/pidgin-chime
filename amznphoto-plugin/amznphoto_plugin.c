#define PURPLE_PLUGINS
#include <glib.h>
#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <debug.h>
#include <version.h>
#include <request.h>
#include <stdlib.h>
#include <errno.h>

/** Plugin id : type-author-name (to guarantee uniqueness) */
#define SIMPLE_PLUGIN_ID "core-glotzer-amznphoto"
#define BADGE_PHOTO_URL_ROOT "https://internal-cdn.amazon.com/badgephotos.amazon.com/?uid="

/*
 * Caller must free returned string.
 */
static gchar*
amznphoto_get_amazon_id_from_email(const gchar* email) {
	gchar **tokenized_email;
	gchar *amazon_id;

	tokenized_email = g_strsplit(email, "@", 2);
	amazon_id = g_strdup(tokenized_email[0]);
	g_strfreev(tokenized_email);

	return (amazon_id);
}

static void
amznphoto_fetch_url_cb(PurpleUtilFetchUrlData *url_data, gpointer user_data,
		       const gchar *url_text, gsize len, const gchar *error_message) {
	/*
	 * Very Important:
	 * The PurpleContact* is the PARENT of the buddy and this is needed
	 * to pass to the set_custom_icon api (cast as a PurpleBlistNode*).
	 */
	PurpleBuddy* buddy = (PurpleBuddy*)user_data;
	const gchar* email = purple_buddy_get_name(buddy);
	gchar* amazon_id = amznphoto_get_amazon_id_from_email(email);
	PurpleContact* contact = purple_buddy_get_contact(buddy);
	if (!(contact && PURPLE_BLIST_NODE_IS_CONTACT((PurpleBlistNode*)contact))) {
		g_free(amazon_id);
		return;
        }
	gchar* filename = g_strconcat(purple_user_dir(), "/icons/", "amazon-", amazon_id, ".jpg", NULL);
	if (!purple_util_write_data_to_file_absolute(filename, url_text, len)) {
		purple_debug(PURPLE_DEBUG_ERROR, "amznphoto", "Could not write URL %s data to file %s\n", url_text, filename);
		goto err_out;
	}
	if (!purple_buddy_icons_node_set_custom_icon_from_file((PurpleBlistNode*)contact, filename)) {
		purple_debug(PURPLE_DEBUG_ERROR, "amznphoto", "Could not set custom icon from file %s\n", filename);
	}

	err_out:
	g_free(amazon_id);
	g_free(filename);
}

/*
 * Given an account and a buddy fetch the custom url if it is provided
 * else fetch the canonical amazon badge photo. Frees the supplied URL
 * (freeing NULL pointer is OK too).
 */
static void
amznphoto_fetch_photo(PurpleBuddy *buddy, PurpleAccount *acct, gchar *custom_url)
{
	const gchar *email;
	gchar *amazon_id;
	gchar *photo_url;
	PurpleUtilFetchUrlData *url_data;

	email = purple_buddy_get_name(buddy);
	if (!email) {
		purple_debug(PURPLE_DEBUG_ERROR, "amznphoto", "Could not get name for buddy.\n");
		return;
	}
	amazon_id = amznphoto_get_amazon_id_from_email(email);
	if (!amazon_id) {
		purple_debug(PURPLE_DEBUG_ERROR, "amznphoto", "Could not derive Amazon ID.\n");
		return;
	}

	/*
	 * Depending on the case photo_url is either allocated here or allocated by caller.
	 * In either case it will be freed on exit from this function.
	 */
	if (!custom_url) {
		photo_url = g_strconcat(BADGE_PHOTO_URL_ROOT, amazon_id, NULL);
	} else {
		photo_url = custom_url;
	}

	url_data = purple_util_fetch_url_len(photo_url, TRUE, NULL, TRUE, 100*1024, amznphoto_fetch_url_cb, buddy);
	if (!url_data) {
		purple_debug(PURPLE_DEBUG_ERROR, "amznphoto", "Error fetching URL data for %s %s\n",
			     amazon_id, photo_url);
	}
	g_free(photo_url);
	g_free(amazon_id);
}

static void
amznphoto_blist_node_added_cb(PurpleBlistNode *node) {
	PurpleBuddy *buddy;
	/* Make sure we're really a buddy */
	if (!(node && PURPLE_BLIST_NODE_IS_BUDDY(node))) {
		return;
	}
	buddy = (PurpleBuddy*)node;
	amznphoto_fetch_photo(buddy, buddy->account, NULL);
}

static void
amznphoto_download_badge_photo_ok(gpointer data, PurpleRequestFields *fields)
{
	gchar* username;
	PurpleAccount *account;
	PurpleBuddy *buddy;
	GSList *buddies, *cur;
	account = purple_request_fields_get_account(fields, "account");
	username = g_strdup(purple_normalize(account, purple_request_fields_get_string(fields, "screenname")));

	if (username && *username && account) {
		buddies = purple_find_buddies(account, username);
		if (!buddies) {
			purple_debug(PURPLE_DEBUG_ERROR, "amznphoto", "Could not get buddy list for account and %s\n", username);
		}
		for (cur = buddies; cur != NULL; cur = cur->next) {
			PurpleBlistNode *node = cur->data;
			/* Make sure we're really a buddy */
			if (node && (PURPLE_BLIST_NODE_IS_BUDDY(node))) {
				buddy = (PurpleBuddy*)node;
				amznphoto_fetch_photo(buddy, account, NULL);
			}
		}
		g_slist_free(buddies);
	} else {
		purple_debug(PURPLE_DEBUG_ERROR, "amznphoto", "Problem with username or account in download custom photo.\n");
	}

	g_free(username);
}

static void
amznphoto_download_badge_photo(PurplePluginAction *action)
{
	PurpleRequestFields *fields;
	PurpleRequestFieldGroup *group;
	PurpleRequestField *field;

	fields = purple_request_fields_new();
	group = purple_request_field_group_new(NULL);
	purple_request_fields_add_group(fields, group);

	field = purple_request_field_string_new("screenname", _("_Name"), NULL, FALSE);
	purple_request_field_set_type_hint(field, "screenname-all");
	purple_request_field_set_required(field, TRUE);
	purple_request_field_group_add_field(group, field);

	field = purple_request_field_account_new("account", _("_Account"), NULL);
	purple_request_field_account_set_show_all(field, TRUE);
	purple_request_field_set_visible(field,
		(purple_accounts_get_all() != NULL &&
		 purple_accounts_get_all()->next != NULL));
	purple_request_field_set_required(field, TRUE);
	purple_request_field_group_add_field(group, field);

	purple_request_fields(purple_get_blist(), _("Download Badge Photo"),
	                                            NULL,
						    _("Please enter the username or alias of the person "
						    "whose badge photo you want."),
						    fields,
						    _("OK"), G_CALLBACK(amznphoto_download_badge_photo_ok),
						    _("Cancel"), NULL,
						    NULL, NULL, NULL,
						    NULL);
}

/*
 * Called function amznphoto_fetch_photo must free url.
 */
static void
amznphoto_download_custom_photo_ok(gpointer data, PurpleRequestFields *fields)
{
	gchar *username, *url;
	PurpleAccount *account;
	PurpleBuddy *buddy;
	GSList *buddies, *cur;
	account = purple_request_fields_get_account(fields, "account");
	username = g_strdup(purple_normalize(account, purple_request_fields_get_string(fields, "screenname")));
	url = g_strdup(purple_normalize(account, purple_request_fields_get_string(fields, "url")));

	if (username && *username && account) {
		buddies = purple_find_buddies(account, username);
		if (!buddies) {
			purple_debug(PURPLE_DEBUG_ERROR, "amznphoto", "Could not get buddy list for account and %s\n", username);
		}
		for (cur = buddies; cur != NULL; cur = cur->next) {
			PurpleBlistNode *node = cur->data;
			/* Make sure we're really a buddy */
			if (node && (PURPLE_BLIST_NODE_IS_BUDDY(node))) {
				buddy = (PurpleBuddy*)node;
				amznphoto_fetch_photo(buddy, account, url);
			}
		}
		g_slist_free(buddies);
	} else {
		purple_debug(PURPLE_DEBUG_ERROR, "amznphoto", "Problem with username or account in download custom photo.\n");
	}

	g_free(username);
}

static void
amznphoto_download_custom_photo(PurplePluginAction *action)
{
	PurpleRequestFields *fields;
	PurpleRequestFieldGroup *group;
	PurpleRequestField *field;

	fields = purple_request_fields_new();
	group = purple_request_field_group_new(NULL);
	purple_request_fields_add_group(fields, group);

	field = purple_request_field_string_new("screenname", _("_Name"), NULL, FALSE);
	purple_request_field_set_type_hint(field, "screenname-all");
	purple_request_field_set_required(field, TRUE);
	purple_request_field_group_add_field(group, field);

	field = purple_request_field_account_new("account", _("_Account"), NULL);
	purple_request_field_account_set_show_all(field, TRUE);
	purple_request_field_set_visible(field,
		(purple_accounts_get_all() != NULL &&
		 purple_accounts_get_all()->next != NULL));
	purple_request_field_set_required(field, TRUE);
	purple_request_field_group_add_field(group, field);

	field = purple_request_field_string_new("url", _("URL of custom photo"), NULL, FALSE);
	purple_request_field_set_required(field, TRUE);
	purple_request_field_group_add_field(group, field);
	purple_request_fields(purple_get_blist(), _("Download Custom Photo"),
	                                            NULL,
						    _("Please enter the username or alias of the person "
						    "whose custom photo you want."),
						    fields,
						    _("OK"), G_CALLBACK(amznphoto_download_custom_photo_ok),
						    _("Cancel"), NULL,
						    NULL, NULL, NULL,
						    NULL);
}

static void
amznphoto_download_all_photos_ok(void *ignored, PurpleRequestFields *fields)
{
	GSList *buddies, *cur;
	PurpleBuddy *buddy;
	PurpleBlistNode *node;
	PurpleAccount *acct = purple_request_fields_get_account(fields, "acct");
        int i;

	/* NULL means get em all. */
	buddies = purple_find_buddies(acct, NULL);
	if (!buddies) {
		purple_debug(PURPLE_DEBUG_ERROR, "amznphoto", "Could not get buddy list for account\n");
	}
	for (cur = buddies, i=0; cur != NULL; cur = cur->next, i++) {
		node = cur->data;
		/* Make sure we're really a buddy */
		if (node && (PURPLE_BLIST_NODE_IS_BUDDY(node))) {
			buddy = (PurpleBuddy*)node;
			amznphoto_fetch_photo(buddy, acct, NULL);
		}
	}
        if (i == 0) {
		purple_debug(PURPLE_DEBUG_WARNING, "amznphoto", "No buddies found for account\n");
        }
	g_slist_free(buddies);
}

static void
amznphoto_download_all_photos(PurplePluginAction *action)
{
	/* Use the request API */
	PurpleRequestFields *request;
	PurpleRequestFieldGroup *group;
	PurpleRequestField *field;

	group = purple_request_field_group_new(NULL);
	field = purple_request_field_account_new("acct", _("Account"), NULL);
	purple_request_field_account_set_show_all(field, TRUE);
	purple_request_field_group_add_field(group, field);
	request = purple_request_fields_new();
	purple_request_fields_add_group(request, group);
	purple_request_fields(action->plugin,
                              N_("Amazon Photo"),
			      _("Select Account"),
			      NULL,
			      request,
			      _("_Set"), G_CALLBACK(amznphoto_download_all_photos_ok),
			      _("_Cancel"), NULL,
			      NULL, NULL, NULL,
			      NULL);
}

static void
amznphoto_show_about_plugin(PurplePluginAction *action)
{
	purple_notify_formatted(action->context,
				NULL, _("Amazon Photo plugin"), PACKAGE_STRING, _("Beep Beep"),
				NULL, NULL);
}

static GList*
amznphoto_plugin_actions(PurplePlugin *plugin,
			 gpointer context)
{
	PurplePluginAction *act;
	GList *acts = NULL;

	act = purple_plugin_action_new(_("About Amznphoto plugin..."),
				       amznphoto_show_about_plugin);
	acts = g_list_append(acts, act);

	act = purple_plugin_action_new(_("Download all photos"),
				       amznphoto_download_all_photos);
	acts = g_list_append(acts, act);

	act = purple_plugin_action_new(_("Download a custom photo"),
				       amznphoto_download_custom_photo);
	acts = g_list_append(acts, act);

	act = purple_plugin_action_new(_("Download a badge photo"),
				       amznphoto_download_badge_photo);
	acts = g_list_append(acts, act);

	return acts;
}

static gboolean
amznphoto_plugin_load(PurplePlugin *plugin)
{
	void *blist_handle = purple_blist_get_handle();
	purple_debug(PURPLE_DEBUG_INFO, "amznphoto", "amznphoto plugin loaded.\n");
	purple_signal_connect(blist_handle,
			      "blist-node-added", plugin,
			      PURPLE_CALLBACK(amznphoto_blist_node_added_cb), NULL);
	return TRUE;
}

static gboolean
amznphoto_plugin_unload(PurplePlugin *plugin)
{
	void *blist_handle = purple_blist_get_handle();
	purple_debug(PURPLE_DEBUG_INFO, "amznphoto", "amznphoto plugin unloaded.\n");
	purple_signal_disconnect(blist_handle,
				 "blist-node-added", plugin,
				 PURPLE_CALLBACK(amznphoto_blist_node_added_cb));
	return TRUE;
}

static void
amznphoto_plugin_destroy(PurplePlugin *plugin)
{
}

static PurplePluginInfo info =
{
	PURPLE_PLUGIN_MAGIC,
	PURPLE_MAJOR_VERSION,			   /**<major version */
	PURPLE_MINOR_VERSION,			   /**<minor version */
	PURPLE_PLUGIN_STANDARD,			   /**< type */
	NULL,					   /**< ui_requirement */
	0,					   /**< flags */
	NULL,					   /**< dependencies */
	PURPLE_PRIORITY_DEFAULT,		   /**< priority */
	"amaznphoto",				   /**< id */
	"Amazon Photo",				   /**< name */
	PACKAGE_VERSION,			   /**< version */
	"Amazon Photo.",			   /**< summary */
	"Grabs Amazon Photos ",			   /**< description */
	"John Glotzer <glotzer@amazon.com>",	   /**< author */
	"www.amazon.com",			   /**< homepage */
	amznphoto_plugin_load,			   /**< load */
	amznphoto_plugin_unload,		   /**< unload */
	amznphoto_plugin_destroy,		   /**< destroy */
	NULL,					   /**< ui_info */
	NULL,					   /**< extra_info */
	NULL,					   /**< prefs_info */
	amznphoto_plugin_actions,		   /**< actions */

	/* padding */
	NULL,
	NULL,
	NULL,
	NULL
};

static void
init_plugin(PurplePlugin *plugin)
{
	/* if .purple/icons doesn't exist create it. */
	gchar* dirname	= g_strconcat(purple_user_dir(), "/icons", NULL);
	if (!g_file_test(dirname, G_FILE_TEST_IS_DIR)) {
		if (g_mkdir(dirname, S_IRUSR | S_IWUSR | S_IXUSR) < 0) {
			purple_debug(PURPLE_DEBUG_ERROR, "amznphoto", "Unable to create directory %s: %s\n",
				     dirname, g_strerror(errno));
		}
	}
	g_free(dirname);
}
PURPLE_INIT_PLUGIN(amznphoto_plugin, init_plugin, info)
