
#include <unistd.h>
#include <getopt.h>
#include <stdio.h>

#include "chime-connection.h"
#include "chime-contact.h"
#include "chime-room.h"
#include "chime-meeting.h"
#include "chime-call.h"

static const struct option long_options[] = {
	{ "devtoken", required_argument, NULL, 'd' },
	{ "email", required_argument, NULL, 'e' },
	{ "token", required_argument, NULL, 't' },
	{ NULL, 0, NULL, 0 }
};


typedef struct {
	char *meeting;
	ChimeMeeting *mtg;
	ChimeCall *call;
} StreamCtx;

static void on_chime_authenticate(ChimeConnection *cxn, const gchar *uri, StreamCtx *sctx)
{
	fprintf(stderr, "Error: Chime asked to authenticate at %s\n", uri);
	exit(1);
}


static void on_chime_connected(ChimeConnection *cxn, const gchar *display_name, StreamCtx *sctx)
{
	printf("Chime connected as %s\n", display_name);
}

static void on_chime_disconnected(ChimeConnection *cxn, GError *error, StreamCtx *sctx)
{
	printf("Chime disconnected (%s)\n", error ? error->message : "no error");
}

static void on_chime_progress(ChimeConnection *cxn, int percent, const gchar *msg, StreamCtx *sctx)
{
	printf("Chime progress: %s\n", msg);
}

static void on_chime_log_message(ChimeConnection *cxn, ChimeLogLevel lvl, const gchar *str,
				 StreamCtx *sctx)
{
	printf("Chime log: %s\n", str);
}

static void on_audio_state(ChimeCall *call, ChimeAudioState audio_state,
			   const gchar *message, StreamCtx *sctx)
{
	printf("Audio state %d: %s\n", audio_state, message);
}

static void join_mtg_done(GObject *source, GAsyncResult *result, gpointer _sctx)
{
	StreamCtx *sctx = _sctx;
	GError *error = NULL;
	sctx->mtg = chime_connection_join_meeting_finish(CHIME_CONNECTION(source), result, &error);

	if (!sctx->mtg) {
		fprintf(stderr, "Failed to join meeting: %s\n", error->message);
		exit(1);
	}
	sctx->call = chime_meeting_get_call(sctx->mtg);
	if (!sctx->call) {
		fprintf(stderr, "ChimeMeeting has no call!\n");
		exit(1);
	}
	g_signal_connect(sctx->call, "audio-state", G_CALLBACK(on_audio_state), sctx);
}

static void on_chime_new_meeting(ChimeConnection *cxn, ChimeMeeting *mtg, StreamCtx *sctx)
{
	printf("Chime meeting discovered: %s (%s)\n", chime_meeting_get_name(mtg), chime_meeting_get_id(mtg));

	if (!g_strcmp0(sctx->meeting, chime_meeting_get_name(mtg)) ||
	    !g_strcmp0(sctx->meeting, chime_meeting_get_id(mtg))) {
		chime_connection_join_meeting_async(cxn, mtg, TRUE, NULL, join_mtg_done, sctx);
	}
}

int main(int argc, char **argv)
{
	StreamCtx sctx = { 0 };
	char *devtoken = NULL;
	char *email = NULL;
	char *token = NULL;
	int opt;

	while ((opt = getopt_long(argc, argv, "d:e:t:", long_options, NULL)) != -1) {
		switch (opt) {
		case 'd':
			devtoken = optarg;
			break;
		case 'e':
			email = optarg;
			break;
		case 't':
			token = optarg;
			break;
		}
	}

	if (optind != argc - 1 || !devtoken || !email || !token) {
		fprintf(stderr, "Usage: stream-meeting -d <DEVTOKEM> -e <EMAIL> -t <TOKEN> <MEETING>\n");
		exit(1);
	}

	sctx.meeting = argv[optind];


	GMainLoop *loop = g_main_loop_new(NULL, FALSE);

	ChimeConnection *cxn = chime_connection_new(email, NULL, devtoken, token);
	g_signal_connect(cxn, "authenticate",
			 G_CALLBACK(on_chime_authenticate), &sctx);
	g_signal_connect(cxn, "connected",
			 G_CALLBACK(on_chime_connected), &sctx);
	g_signal_connect(cxn, "disconnected",
			 G_CALLBACK(on_chime_disconnected), &sctx);
	g_signal_connect(cxn, "progress",
			 G_CALLBACK(on_chime_progress), &sctx);
	g_signal_connect(cxn, "new-meeting",
			 G_CALLBACK(on_chime_new_meeting), &sctx);
	g_signal_connect(cxn, "log-message",
			 G_CALLBACK(on_chime_log_message), &sctx);

	chime_connection_connect(cxn);
	g_main_loop_run(loop);

	exit(0);
}
