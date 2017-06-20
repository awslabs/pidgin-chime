
#ifndef __CHIME_H__
#define __CHIME_H__

#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

struct chime_msg_queue {
	SoupMessage *msg;
	SoupSessionCallback cb;
	gpointer cb_data;
};

struct chime_connection {
	PurpleConnection *prpl_conn;

	SoupSession *soup_sess;
	SoupWebsocketConnection *ws_conn;

	gchar *session_token;

	/* Messages queued for resubmission */
	GList *msg_queue;

	JsonNode *reg_node;
	const gchar *session_id;
	const gchar *profile_id;
	const gchar *profile_channel;
	const gchar *presence_channel;

	const gchar *device_id;
	const gchar *device_channel;

	const gchar *presence_url;
	const gchar *websocket_url;
	const gchar *reachability_url;
	const gchar *profile_url;
	const gchar *contacts_url;
	const gchar *messaging_url;
	const gchar *conference_url;
};


extern GQuark pidgin_chime_error_quark(void);
#define CHIME_ERROR pidgin_chime_error_quark()

enum {
	CHIME_ERROR_REQUEST_FAILED = 1,
	CHIME_ERROR_BAD_RESPONSE,
};

#endif /* __CHIME_H__ */
