/*
 * Pidgin/libpurple Chime client plugin
 *
 * Copyright © 2017 Amazon.com, Inc. or its affiliates.
 *
 * Author: Nicola Girardi <nicola@aloc.in>
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

#include <errno.h>
#include <libgen.h>
#include <glib/gi18n.h>
#include <debug.h>
#include "chime.h"
#include "chime-connection-private.h"

// According to http://docs.aws.amazon.com/chime/latest/ug/chime-ug.pdf this is the maximum allowed size for attachments.
// (The default limit for purple_util_fetch_url() is 512 kB.)
#define ATTACHMENT_MAX_SIZE (50*1000*1000)

/*
 * Writes to the IM conversation handling the case where the user sent message
 * from other client.
 */
static void write_conversation_message(const char *from, const char *im_email,
		PurpleConnection *conn, const gchar *msg, PurpleMessageFlags flags, time_t when)
{
	if (!strcmp(from, im_email)) {
		serv_got_im(conn, im_email, msg, flags | PURPLE_MESSAGE_RECV, when);
	} else {
		PurpleAccount *account = conn->account;
		PurpleConversation *pconv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM,
										  im_email, account);
		if (!pconv) {
			pconv = purple_conversation_new(PURPLE_CONV_TYPE_IM, account, im_email);
			if (!pconv) {
				purple_debug_error("chime", "NO CONV FOR %s\n", im_email);
				return;
			}
		}
		/* Just inject a message from ourselves, avoiding any notifications */
		purple_conversation_write(pconv, NULL, msg, flags | PURPLE_MESSAGE_SEND, when);
	}
}

static void img_message(AttachmentContext *ctx, int image_id)
{
	PurpleMessageFlags flags = PURPLE_MESSAGE_IMAGES;
	gchar *msg = g_strdup_printf("<br><img id=\"%u\">", image_id);
	if (ctx->chat_id != -1) {
		serv_got_chat_in(ctx->conn, ctx->chat_id, ctx->from, flags, msg, ctx->when);
	} else {
		write_conversation_message(ctx->from, ctx->im_email, ctx->conn, msg, flags, ctx->when);
	}
	g_free(msg);
}

static void sys_message(AttachmentContext *ctx, const gchar *msg, PurpleMessageFlags flags)
{
	flags |= PURPLE_MESSAGE_SYSTEM;
	if (ctx->chat_id != -1) {
		serv_got_chat_in(ctx->conn, ctx->chat_id, "", flags, msg, time(NULL));
	} else {
		write_conversation_message(ctx->from, ctx->im_email, ctx->conn, msg, flags, time(NULL));
	}
}

static void insert_image_from_file(AttachmentContext *ctx, const gchar *path)
{
	gchar *contents;
	gsize size;
	GError *err = NULL;

	if (!g_file_get_contents(path, &contents, &size, &err)) {
		sys_message(ctx, err->message, PURPLE_MESSAGE_ERROR);
		g_error_free(err);
		return;
	}

	/* The imgstore will take ownership of the contents. */
	int img_id = purple_imgstore_add_with_id(contents, size, path);
	if (img_id == 0) {
		gchar *msg = g_strdup_printf(_("Could not make purple image from %s"), path);
		sys_message(ctx, msg, PURPLE_MESSAGE_ERROR);
		g_free(msg);
		return;
	}
	img_message(ctx, img_id);
}

typedef struct _DownloadCallbackData {
	ChimeAttachment *att;
	AttachmentContext *ctx;
	gchar *path;
} DownloadCallbackData;

static void deep_free_download_data(DownloadCallbackData *data)
{
	g_free(data->att->message_id);
	g_free(data->att->filename);
	g_free(data->att->url);
	g_free(data->att->content_type);
	g_free(data->att);
	g_free(data->ctx);
	g_free(data->path);
	g_free(data);
}

static void download_callback(PurpleUtilFetchUrlData *url_data, gpointer user_data, const gchar *url_text, gsize len, const gchar *error_message)
{
	DownloadCallbackData *data = user_data;

	if (error_message != NULL) {
		sys_message(data->ctx, error_message, PURPLE_MESSAGE_ERROR);
		deep_free_download_data(data);
		return;
	}

	if (len <= 0 || url_text == NULL ){
		sys_message(data->ctx, _("Downloaded empty contents."), PURPLE_MESSAGE_ERROR);
		deep_free_download_data(data);
		return;
	}

	GError *err = NULL;
	if (!g_file_set_contents(data->path, url_text, len, &err)) {
		sys_message(data->ctx, err->message, PURPLE_MESSAGE_ERROR);
		g_error_free(err);
		deep_free_download_data(data);
		return;
	}

	if (g_content_type_is_a(data->att->content_type, "image/*")) {
		insert_image_from_file(data->ctx, data->path);
	} else {
		gchar *msg = g_strdup_printf(_("%s has attached <a href=\"file://%s\">%s</a>"), data->ctx->from, data->path, data->att->filename);
		sys_message(data->ctx, msg, PURPLE_MESSAGE_SYSTEM);
		g_free(msg);
	}

	deep_free_download_data(data);
}

ChimeAttachment *extract_attachment(JsonNode *record)
{
	JsonObject *robj;
	JsonNode *node;
	const gchar *msg_id, *filename, *url, *content_type;

	g_return_val_if_fail(record != NULL, NULL);
	robj = json_node_get_object(record);
	g_return_val_if_fail(robj != NULL, NULL);
	node = json_object_get_member(robj, "Attachment");
	if (!node || json_node_is_null(node))
		return NULL;

	g_return_val_if_fail(parse_string(record, "MessageId", &msg_id), NULL);
	g_return_val_if_fail(parse_string(node, "FileName", &filename), NULL);
	g_return_val_if_fail(parse_string(node, "Url", &url), NULL);
	g_return_val_if_fail(parse_string(node, "ContentType", &content_type), NULL);

	ChimeAttachment *att = g_new0(ChimeAttachment, 1);
	att->message_id = g_strdup(msg_id);
	att->filename = g_strdup(filename);
	att->url = g_strdup(url);
	att->content_type = g_strdup(content_type);

	return att;
}

void download_attachment(ChimeConnection *cxn, ChimeAttachment *att, AttachmentContext *ctx)
{
	const gchar *username = chime_connection_get_email(cxn);
	gchar *dir = g_build_filename(purple_user_dir(), "chime", username, "downloads", NULL);
	if (g_mkdir_with_parents(dir, 0755) == -1) {
		gchar *error_msg = g_strdup_printf(_("Could not make dir %s,will not fetch file/image (errno=%d, errstr=%s)"), dir, errno, g_strerror(errno));
		sys_message(ctx, error_msg, PURPLE_MESSAGE_ERROR);
		g_free(dir);
		g_free(error_msg);
		return;
	}
	DownloadCallbackData *data = g_new0(DownloadCallbackData, 1);
	data->path = g_strdup_printf("%s/%s-%s", dir, att->message_id, att->filename);
	g_free(dir);
	data->att = att;
	data->ctx = ctx;
	purple_util_fetch_url_len(att->url, TRUE, NULL, FALSE, ATTACHMENT_MAX_SIZE, download_callback, data);
}

/*
 * Chime Attachment Upload
 *
 * Uploading a file through Chime involves many steps.
 * This is basically the currently flow:
 *
 * +--------+         +--------+     +----+   +---------+
 * | Chime  |         | Chime  |     | S3 |   |Recipient|
 * | Client |         | Server |     |    |   |Clients  |
 * +----+---+         +---+----+     +--+-+   +----+----+
 *      |  Request upload |             |          |
 *      +---------------->+             |          |
 *      |Return upload url|             |          |
 *      +<----------------+             |          |
 *      |  Put request    |             |          |
 *      +------------------------------>+          |
 *      |  Confirm upload |             |          |
 *      +---------------->+ Deliver msg |          |
 *      |                 +----------------------->+
 *      |                 |             | Download |
 *      |                 |             +<---------+
 *      |                 |             |          |
 *      |                 |             |          |
 *
 * The interaction with S3 is transparent. All the necessary parameters
 * are embedded on the url returned by the Chime Server. All we need to
 * do is to make a PUT request to that url.
 */

typedef struct _AttachmentUpload {
	ChimeConnection *conn;
	ChimeObject *obj;

	SoupSession *soup_session;
	SoupMessage *soup_message;

	gchar *content;
	gsize content_length;
	gchar *content_type;

	gchar *upload_id;
	gchar *upload_url;
} AttachmentUpload;

static void deep_free_upload_data(PurpleXfer *xfer)
{
	AttachmentUpload *data = (AttachmentUpload*)xfer->data;

	// This means an error happened, so cancel the transfer.
	if (xfer->status != PURPLE_XFER_STATUS_DONE &&
	    xfer->status != PURPLE_XFER_STATUS_CANCEL_LOCAL) {
		purple_xfer_cancel_local(xfer);
	}

	g_free(data->content);
	g_free(data->content_type);
	g_free(data->upload_id);
	g_free(data->upload_url);
	g_free(data);

	purple_xfer_unref(xfer);
}

static void send_upload_confirmation_callback(GObject *source, GAsyncResult *result, gpointer user_data)
{
	purple_debug_misc("chime", "Upload confirmation sent\n");
	ChimeConnection *cxn = CHIME_CONNECTION(source);
	GError *error = NULL;
	PurpleXfer *xfer = (PurpleXfer*)user_data;

	JsonNode *msgnode = chime_connection_send_message_finish(cxn, result, &error);
	if (msgnode) {
		const gchar *msg_id;
		if (!parse_string(msgnode, "MessageId", &msg_id)) {
			purple_xfer_conversation_write(xfer, _("Failed to send upload confirmation"), TRUE);
		} else {
			purple_xfer_set_completed(xfer, TRUE);
		}
		json_node_unref(msgnode);
	} else {
		gchar *error_msg = g_strdup_printf(_("Failed to send upload confirmation: %s"), error->message);
		purple_debug_error("chime", "%s\n", error_msg);
		purple_xfer_conversation_write(xfer, error_msg, TRUE);
		g_free(error_msg);
		g_clear_error(&error);
	}

	deep_free_upload_data(xfer);
}

static void send_upload_confirmation(PurpleXfer *xfer, const gchar *etag)
{
	purple_debug_misc("chime", "Sending upload confirmation\n");

	AttachmentUpload *data = (AttachmentUpload*)xfer->data;

	JsonBuilder *jb = json_builder_new();
	jb = json_builder_begin_object(jb);
	jb = json_builder_set_member_name(jb, "AttachUpload");

	jb = json_builder_begin_object(jb);
	jb = json_builder_set_member_name(jb, "FileName");
	jb = json_builder_add_string_value(jb, xfer->filename);
	jb = json_builder_set_member_name(jb, "UploadEtag");
	jb = json_builder_add_string_value(jb, etag);
	jb = json_builder_set_member_name(jb, "UploadId");
	jb = json_builder_add_string_value(jb, data->upload_id);
	jb = json_builder_end_object(jb);

	jb = json_builder_end_object(jb);

	JsonNode *node = json_builder_get_root(jb);
	JsonObject *obj = json_node_get_object(node);

	chime_connection_send_message_async(data->conn,
					    data->obj,
					    xfer->message,
					    NULL,
					    send_upload_confirmation_callback,
					    xfer,
					    obj);

	json_node_unref(node);
	g_object_unref(jb);
}

static void put_file_callback(SoupSession *session, SoupMessage *msg, gpointer user_data)
{
	purple_debug_misc("chime", "Put file request finished\n");
	PurpleXfer *xfer = (PurpleXfer*)user_data;
	AttachmentUpload *data = (AttachmentUpload*)xfer->data;

	// This is freed by libsoup
	data->soup_session = NULL;
	data->soup_message = NULL;

	if (purple_xfer_is_canceled(xfer))
		return deep_free_upload_data(xfer);

	if (!SOUP_STATUS_IS_SUCCESSFUL(msg->status_code)) {
		gchar *error_msg = g_strdup_printf(_("Failed to upload file: (%d) %s"),
						   msg->status_code,
						   msg->reason_phrase);
		purple_debug_misc("chime", "%s\n", error_msg);
		purple_xfer_conversation_write(xfer, error_msg, TRUE);
		g_free(error_msg);
		deep_free_upload_data(xfer);
		return;
	}

	const char *etag;
	etag = soup_message_headers_get_one(msg->response_headers, "ETag");
	purple_debug_misc("chime", "Extracted ETag: %s\n", etag);

	if (!etag) {
		purple_debug_error("chime", "Could not extract ETag value from HTTP headers\n");
		deep_free_upload_data(xfer);
		return;
	}

	// We need to send a message confirming the upload
	send_upload_confirmation(xfer, etag);
}

static void update_progress(SoupMessage *msg, SoupBuffer *chunk, gpointer user_data)
{
	PurpleXfer *xfer = (PurpleXfer*)user_data;
	xfer->bytes_sent = xfer->bytes_sent + chunk->length;
	xfer->bytes_remaining = xfer->bytes_remaining - chunk->length;
	purple_debug_misc("chime", "Updating progress by %lu bytes. Sent=%lu, Remaining=%lu\n",
			  chunk->length, xfer->bytes_sent, xfer->bytes_remaining);
	purple_xfer_update_progress(xfer);
}

static void put_file(ChimeConnection *cxn, PurpleXfer *xfer)
{
	purple_debug_misc("chime", "Submitting put file request\n");

	AttachmentUpload *data = (AttachmentUpload*)xfer->data;
	gchar *content_length = g_strdup_printf("%lu", data->content_length);

	SoupMessage *msg;
	data->soup_message = msg = soup_message_new("PUT", data->upload_url);

	soup_message_set_request(msg, data->content_type, SOUP_MEMORY_TEMPORARY,
				 data->content, data->content_length);
	soup_message_headers_append(msg->request_headers, "Cache-Control", "no-cache");
	soup_message_headers_append(msg->request_headers, "Pragma", "no-cache");
	soup_message_headers_append(msg->request_headers, "Accept", "*/*");
	soup_message_headers_append(msg->request_headers, "Content-length", content_length);

	g_signal_connect(msg, "wrote-body-data", (GCallback)update_progress, xfer);

	data->soup_session = soup_session_new_with_options(SOUP_SESSION_ADD_FEATURE_BY_TYPE,
							   SOUP_TYPE_CONTENT_SNIFFER,
							   SOUP_SESSION_USER_AGENT,
							   "Pidgin-Chime " PACKAGE_VERSION " ",
							   NULL);

	if (getenv("CHIME_DEBUG") && atoi(getenv("CHIME_DEBUG")) > 0) {
		SoupLogger *l = soup_logger_new(SOUP_LOGGER_LOG_BODY, -1);
		soup_session_add_feature(data->soup_session, SOUP_SESSION_FEATURE(l));
		g_object_unref(l);
		g_object_set(data->soup_session, "ssl-strict", FALSE, NULL);
	}

	soup_session_queue_message(data->soup_session, msg, put_file_callback, xfer);

	g_free(content_length);
}

static void request_upload_url_callback(ChimeConnection *cxn, SoupMessage *msg,
					JsonNode *node, gpointer user_data)
{
	purple_debug_misc("chime", "Upload url requested. Parsing response.\n");
	PurpleXfer *xfer = (PurpleXfer*)user_data;
	AttachmentUpload *data = (AttachmentUpload*)xfer->data;

	if (purple_xfer_is_canceled(xfer))
		return deep_free_upload_data(xfer);

	if (SOUP_STATUS_IS_SUCCESSFUL(msg->status_code) && node) {
		const gchar *upload_id, *upload_url;
		if (parse_string(node, "UploadId", &upload_id) &&
		    parse_string(node, "UploadUrl", &upload_url)) {

			data->upload_id = g_strdup(upload_id);
			data->upload_url = g_strdup(upload_url);
			purple_xfer_start(xfer, -1, NULL, 0);
		} else {
			purple_debug_error("chime", "Could not parse UploadId and/or UploadUrl\n");
			purple_xfer_conversation_write(xfer, _("Could not get upload url"), TRUE);
			deep_free_upload_data(xfer);
		}
	} else {
		if (!SOUP_STATUS_IS_SUCCESSFUL(msg->status_code)) {
			const gchar *reason = msg->reason_phrase;

			if (node)
				parse_string(node, "Message", &reason);

			gchar *error_msg = g_strdup_printf(_("Failed to request upload: %d %s"),
							   msg->status_code,
							   reason);
			purple_xfer_conversation_write(xfer, error_msg, TRUE);
			g_free(error_msg);
		} else if (!node) {
			purple_xfer_conversation_write(xfer, _("Failed to request upload"), TRUE);
		}
		deep_free_upload_data(xfer);
	}
}

static void request_upload_url(ChimeConnection *self, const gchar *messaging_url, PurpleXfer *xfer)
{
	AttachmentUpload *data = (AttachmentUpload*)xfer->data;

	JsonBuilder *jb = json_builder_new();
	jb = json_builder_begin_object(jb);
	jb = json_builder_set_member_name(jb, "ContentType");
	jb = json_builder_add_string_value(jb, data->content_type);
	jb = json_builder_end_object(jb);

	SoupURI *uri = soup_uri_new_printf(messaging_url, "/uploads");
	JsonNode *node = json_builder_get_root(jb);
	chime_connection_queue_http_request(self, node, uri, "POST", request_upload_url_callback, xfer);

	json_node_unref(node);
	g_object_unref(jb);
}

struct FileType {
	const gchar *file_extension;
	const gchar *mime_type;
};

// Based on https://developer.mozilla.org/en-US/docs/Web/HTTP/Basics_of_HTTP/MIME_types/Complete_list_of_MIME_types
struct FileType file_types[] = {
	{".aac", "audio/aac"},
	{".avi", "video/x-msvideo"},
	{".doc", "application/msword"},
	{".docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
	{".gif", "image/gif"},
	{".htm", "text/html"},
	{".html", "text/html"},
	{".ics", "text/calendar"},
	{".jpeg", "image/jpeg"},
	{".jpg", "image/jpeg"},
	{".mid", "audio/midi"},
	{".midi", "audio/midi"},
	{".mpeg", "video/mpeg"},
	{".odp", "application/vnd.oasis.opendocument.presentation"},
	{".ods", "application/vnd.oasis.opendocument.spreadsheet"},
	{".odt", "application/vnd.oasis.opendocument.text"},
	{".oga", "audio/ogg"},
	{".ogv", "video/ogg"},
	{".ogx", "application/ogg"},
	{".png", "image/png"},
	{".pdf", "application/pdf"},
	{".ppt", "application/vnd.ms-powerpoint"},
	{".pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
	{".rar", "application/x-rar-compressed"},
	{".rtf", "application/rtf"},
	{".svg", "image/svg+xml"},
	{".tar", "application/x-tar"},
	{".tif", "image/tiff"},
	{".tiff", "image/tiff"},
	{".wav", "audio/x-wav"},
	{".weba", "audio/webm"},
	{".webm", "video/webm"},
	{".webp", "image/webp"},
	{".xhtml", "application/xhtml+xml"},
	{".xls", "application/vnd.ms-excel"},
	{".xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
	{".xml", "application/xml"},
	{".zip", "application/zip"},
	{".7z", "application/x-7z-compressed"},
};

static void get_mime_type(gchar *filename, gchar **mime_type)
{
	gchar *file_extension = g_strrstr(basename(filename), ".");
	const gchar *content_type = NULL;
	if (file_extension) {
		int i;
		purple_debug_misc("chime", "File extension: %s\n", file_extension);
		for (i = 0; i < sizeof(file_types) / sizeof(struct FileType); i++) {
			if (!g_strcmp0(file_extension, file_types[i].file_extension)) {
				content_type = file_types[i].mime_type;
				break;
			}
		}
	} else {
		purple_debug_misc("chime", "File has no extension\n");
	}

	if (!content_type) {
		content_type = "application/unknown";
	}
	purple_debug_misc("chime", "Content type: %s\n", content_type);
	*mime_type = g_strdup(content_type);
}

// TODO: This struct is duplicated on conversations.c
struct chime_im {
	struct chime_msgs m;
	ChimeContact *peer;
};

static void init_upload(PurpleXfer *xfer, struct purple_chime *pc, ChimeObject *obj)
{
	purple_debug_info("chime", "Starting to handle upload of file '%s'\n", xfer->local_filename);

	g_return_if_fail(CHIME_IS_CONNECTION(pc->cxn));
	ChimeConnectionPrivate *priv = CHIME_CONNECTION_GET_PRIVATE(pc->cxn);

	char *file_contents;
	gsize length;
	GError *error = NULL;
	if (!g_file_get_contents(xfer->local_filename, &file_contents, &length, &error)) {
		purple_xfer_conversation_write(xfer, error->message, TRUE);
		purple_debug_error("chime", _("Could not read file '%s' (errno=%d, errstr=%s)\n"),
				   xfer->local_filename, error->code, error->message);
		g_clear_error(&error);
		return;
	}
	AttachmentUpload *data = g_new0(AttachmentUpload, 1);
	data->conn = pc->cxn;
	data->obj = obj;
	data->content = file_contents;
	data->content_length = length;
	get_mime_type(xfer->local_filename, &data->content_type);

	xfer->data = data;
	purple_xfer_set_message(xfer, xfer->filename);
	purple_xfer_ref(xfer);

	request_upload_url(pc->cxn, priv->messaging_url, xfer);
}

static void chime_send_init(PurpleXfer *xfer)
{
	purple_debug_info("chime", "Starting to handle upload of file '%s'\n", xfer->local_filename);

	struct purple_chime *pc = purple_connection_get_protocol_data(xfer->account->gc);
	struct chime_im *im = g_hash_table_lookup(pc->ims_by_email, xfer->who);

	init_upload(xfer, pc, im->m.obj);
}

static void chime_send_init_chat(PurpleXfer *xfer)
{
	purple_debug_info("chime", "Starting to handle upload of file '%s'\n", xfer->local_filename);
	ChimeObject *obj = (ChimeObject*)xfer->data;
	struct purple_chime *pc = purple_connection_get_protocol_data(xfer->account->gc);

	init_upload(xfer, pc, obj);
}

static void chime_send_start(PurpleXfer *xfer)
{
	purple_debug_info("chime", "chime_send_start\n");

	AttachmentUpload *data = (AttachmentUpload*)xfer->data;
	put_file(data->conn, xfer);
}

static void chime_send_cancel(PurpleXfer *xfer)
{
	purple_debug_info("chime", "chime_send_cancel\n");
	AttachmentUpload *data = (AttachmentUpload*)xfer->data;
	if (data && data->soup_session && data->soup_message) {
		soup_session_cancel_message(data->soup_session, data->soup_message, SOUP_STATUS_CANCELLED);
		data->soup_session = NULL;
		data->soup_message = NULL;
	}
}

void chime_send_file(PurpleConnection *gc, const char *who, const char *filename)
{
	purple_debug_info("chime", "chime_send_file(who=%s, file=%s\n", who, filename);

	PurpleXfer *xfer;
	xfer = purple_xfer_new(gc->account, PURPLE_XFER_SEND, who);
	if (xfer) {
		purple_xfer_set_init_fnc(xfer, chime_send_init);
		purple_xfer_set_start_fnc(xfer, chime_send_start);
		purple_xfer_set_cancel_send_fnc(xfer, chime_send_cancel);
	}

	if (filename) {
		purple_xfer_request_accepted(xfer, filename);
	} else {
		purple_xfer_request(xfer);
	}
}

void chime_send_file_chat(PurpleConnection *gc, ChimeObject *obj, const char *who, const char *filename)
{
	purple_debug_info("chime", "chime_send_file_chat(who=%s, file=%s\n", who, filename);

	PurpleXfer *xfer;
	xfer = purple_xfer_new(gc->account, PURPLE_XFER_SEND, who);
	if (xfer) {
		purple_xfer_set_init_fnc(xfer, chime_send_init_chat);
		purple_xfer_set_start_fnc(xfer, chime_send_start);
		purple_xfer_set_cancel_send_fnc(xfer, chime_send_cancel);
	}

	xfer->data = obj;

	if (filename) {
		purple_xfer_request_accepted(xfer, filename);
	} else {
		purple_xfer_request(xfer);
	}
}
