/*
 * Copyright (C) 2015 Red Hat, Inc.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.";
 */

#include <string.h>

#include "chime-util.h"

#include <glib.h>

/**
 * chime_util_get_basenick:
 * @nick: (transfer none): the original nick
 *
 * Returns: (transfer full): the "base nick" of @nick, which can be used to
 *   group nicks that likely belong to the same person (e.g. "nick-away" or
 *   "nick|bbl")
 */
char *
chime_util_get_basenick (const char *nick)
{
  int len;

  for (len = 0; g_ascii_isalnum(nick[len]); len++)
    ;

  if (len > 0)
    return g_utf8_casefold (nick, len);
  else
    return g_utf8_casefold (nick, -1);
}

#ifdef HAVE_STRCASESTR
#  define FOLDFUNC(text) ((char *)(text))
#  define MATCHFUNC(haystick,needle) strcasestr (haystick, needle)
#else
#  define FOLDFUNC(text) g_utf8_casefold (text, -1)
#  define MATCHFUNC(haystick,needle) strstr (haystick, needle)
#endif

gboolean
chime_util_match_nick (const char *text,
                        const char *nick)
{
  g_autofree char *folded_text = NULL;
  g_autofree char *folded_nick = NULL;
  char *match;
  gboolean result = FALSE;
  int len;

  len = strlen (nick);
  if (len == 0)
    return FALSE;

  folded_text = FOLDFUNC (text);
  folded_nick = FOLDFUNC (nick);

  match = MATCHFUNC (folded_text, folded_nick);

  while (match != NULL)
    {
      gboolean starts_word, ends_word;

      /* assume ASCII nicknames, so no complex pango-style breaks */
      starts_word = (match == folded_text || !g_ascii_isalnum (*(match - 1)));
      ends_word = !g_ascii_isalnum (*(match + len));

      result = starts_word && ends_word;
      if (result)
        break;
      match = MATCHFUNC (match + len, folded_nick);
    }

  return result;
}

/**
 * chime_util_match_identify_message:
 * @message: a text message
 * @command: (optional) (out): the parsed command if the @message is an
 *                             identify command
 * @username: (optional) (out): the parsed name if the @message is an
 *                              identify command
 * @password: (optional) (out): the parsed password if the @message is an
 *                              identify command
 *
 * Returns: %TRUE if @message is an identify command
 */
gboolean
chime_util_match_identify_message (const char  *message,
                                    char       **command,
                                    char       **username,
                                    char       **password)
{
  static GRegex *identify_message_regex = NULL;
  g_autoptr(GMatchInfo) match = NULL;
  g_autofree char *text = NULL;
  char *stripped_text;
  gboolean matched;

  text = g_strdup (message);
  stripped_text = g_strstrip (text);

  if (G_UNLIKELY (identify_message_regex == NULL))
    identify_message_regex = g_regex_new ("^(identify|login) (?:(\\S+) )?(\\S+)$",
                                          G_REGEX_OPTIMIZE | G_REGEX_CASELESS,
                                          0, NULL);

  matched = g_regex_match (identify_message_regex, stripped_text, 0, &match);
  if (matched)
    {
      if (command)
        *command = g_match_info_fetch (match, 1);
      if (username)
        *username = g_match_info_fetch (match, 2);
      if (password)
        *password = g_match_info_fetch (match, 3);
    }

  return matched;
}

/* Hm, doesn't GLib have something that'll do this for us? */
static void get_machine_id(unsigned char *id, int len)
{
	int i = 0;

	memset(id, 0, len);

	gchar *machine_id;
	if (g_file_get_contents("/etc/machine-id", &machine_id, NULL, NULL)) {
		while (i < len * 2 && g_ascii_isxdigit(machine_id[i]) &&
		       g_ascii_isxdigit(machine_id[i+1])) {
			id[i / 2] = (g_ascii_xdigit_value(machine_id[i]) << 4) + g_ascii_xdigit_value(machine_id[i+1]);
			i += 2;
		}

		g_free(machine_id);
		return;
	}
#ifdef _WIN32
	/* XXX: On Windows, try HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Cryptography\MachineGuid */
#endif
	/* XXX: We could actually try to cobble one together from things like
	 * the FSID of the root file system (see how OpenConnect does that). */
	g_warning("No /etc/machine-id; faking");
	for (i = 0; i < len; i++)
		id[i] = g_random_int_range(0, 256);
}

/**
 * chime_util_generate_dev_token:
 * username: a username
 *
 * Returns: (transfer full): a newly allocated string with the dev token
 */
char *chime_util_generate_dev_token(const char *username)
{
	unsigned char machine_id[16];
	get_machine_id(machine_id, sizeof(machine_id));

	GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA1);
	g_checksum_update(sum, machine_id, sizeof(machine_id));

	g_checksum_update(sum, (void *)username, strlen(username));

	char *dev_token = g_strdup(g_checksum_get_string(sum));
	g_checksum_free(sum);

	return dev_token;
}
