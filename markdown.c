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
	int flags = 0;
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

	mkd_cleanup(doc);

	return 0;
}
