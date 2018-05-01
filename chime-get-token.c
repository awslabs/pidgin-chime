/*
 * Obtain a login token through the command line.
 */
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>

#include "chime/chime-connection.h"

static GMainLoop *loop;
static int status;

static gchar *read_string(const char *prompt, gboolean echo)
{
	#define MAX_LEN 128
	struct termios conf, save;
	char buf[MAX_LEN], *ret;

	fputs(prompt, stdout);
	fflush(stdout);
	if (!echo) {
		if (tcgetattr(STDIN_FILENO, &conf) == -1)
			return NULL;
		save = conf;
		conf.c_lflag &= ~ECHO;
		if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &conf) == -1)
			return NULL;
	}
	ret = fgets(buf, MAX_LEN, stdin);
	if (!echo) {
		tcsetattr(STDIN_FILENO, TCSANOW, &save);
		fputs("\n", stdout);
	}
	return g_strdup(g_strchomp(ret));
}

static void authenticate(ChimeConnection *conn, gpointer state, gboolean user_required)
{
	gchar *user = NULL, *password;

	if (user_required) {
		user = read_string("Username: ", TRUE);
	}
	password = read_string("Password: ", FALSE);
	chime_connection_authenticate(state, user, password);
	g_free(user);
	g_free(password);
}

static void token_acquired(ChimeConnection *conn, GParamSpec *pspec, gpointer ignored)
{
	printf("Session token:\t%s\n", chime_connection_get_session_token(conn));
	g_main_loop_quit(loop);
}

static void disconnected(ChimeConnection *conn, GError *error)
{
	if (error) {
		fprintf(stderr, "ERROR: %s\n", error->message);
		status = EXIT_FAILURE;
	} else {
		status = EXIT_SUCCESS;
	}
	if (g_main_loop_is_running(loop))
		g_main_loop_quit(loop);
}

static void connected(ChimeConnection *conn, gpointer display_name, gpointer ignored)
{
	printf("Disconnecting...\n");
	chime_connection_disconnect(conn);
}

int main(int argc, char *argv[])
{
	ChimeConnection *conn;
	gchar *account;

	if (argc < 2)
		account = read_string("Account e-mail: ", TRUE);
	else
		account = argv[1];

	loop = g_main_loop_new(NULL, FALSE);
	status = EXIT_SUCCESS;

	conn = chime_connection_new(account, NULL, "foo", NULL);

	g_signal_connect(conn, "authenticate",
			 G_CALLBACK(authenticate), NULL);
	g_signal_connect(conn, "notify::session-token",
			 G_CALLBACK(token_acquired), NULL);
	g_signal_connect(conn, "disconnected",
			 G_CALLBACK(disconnected), NULL);
	g_signal_connect(conn, "connected",
			 G_CALLBACK(connected), NULL);

	chime_connection_connect(conn);
	g_main_loop_run(loop);
	chime_connection_disconnect(conn);
	g_object_unref(conn);
	g_main_loop_unref(loop);
	return status;
}
