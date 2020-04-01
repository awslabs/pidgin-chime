#include "markdown.h"
#include <debug.h>
#include <string.h>
#include <stdlib.h>
#include <prpl.h>

/*
 * Uses libmarkdown to convert md to html.
 * Caller must g_free the returned string.
 */
int
do_markdown (const gchar *message, gchar **outbound) {
	MMIOT *doc;
	int flags = MKD_NOTABLES | MKD_NOIMAGE | MKD_NOTABLES; /* Disable unsupported tags */
	int nbytes, rc;
	gchar *res;

	/* make a mkd doc */
	doc = mkd_string(message, strlen(message), flags);
	if (!doc) {
		purple_debug(PURPLE_DEBUG_ERROR, "chime", "mkd_string() failed.\n");
		return -1;
	}

	/* compile the mkd doc */
	rc = mkd_compile(doc, flags);
	if (rc == EOF) {
		purple_debug(PURPLE_DEBUG_ERROR, "chime", "mkd_compile failed.\n");
		mkd_cleanup(doc);
		return -1;
	}

	/* render the html output */
	nbytes = mkd_document(doc, &res);
	if (nbytes <= 0) {
		purple_debug(PURPLE_DEBUG_ERROR, "chime", "mkd_document() failed.\n");
		mkd_cleanup(doc);
		return -1;
	}

	/* Since mkd_cleanup also frees res make a copy before cleaning up. */
	*outbound = g_strdup(res);

	/* Do a sweep to replace some HTML unsupported by Pango markup with best-effort alternatives */
	/* TODO It'd be nice to improve this, e.g. by switching the UI to something that can render HTML or by replacing
	 *      the renderer with something that can produce properly formatted output for a GtkTextBuffer directly.
	 */
	for (char *p = *outbound; *p; p++) {
		/* Code tags are not supported, replace with documented <tt> */
		if (!strncmp(p, "<code>", 6)) {
			memcpy(p, "  <tt>", 6);
			p += 5;
		}
		if (!strncmp(p, "</code>", 7)) {
			memcpy(p, "  </tt>", 7);
			p += 6;
		}
		/* Lists aren't rendered, replace with asterisks */
		if (!strncmp(p, "<li>", 4)) {
			memcpy(p, "  * ", 4);
			p += 3;
		}
		if (!strncmp(p, "</li>", 5)) {
			memcpy(p, "     ", 5);
			p += 4;
		}
	}

	mkd_cleanup(doc);

	return 0;
}
