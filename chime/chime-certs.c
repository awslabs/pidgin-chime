/*
 * Pidgin/libpurple Chime client plugin
 *
 * Copyright © 2018 Amazon.com, Inc. or its affiliates.
 *
 * Authors: David Woodhouse <dwmw2@infradead.org>
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

#include <config.h>

#include "chime-connection.h"
#include "chime-connection-private.h"

#include <gio/gio.h>

#define NR_CERTS 7

static const char *cert_filenames[NR_CERTS] = {
	"Amazon.com_InfoSec_CA_G3.pem",
	"Amazon.com_Internal_Root_Certificate_Authority.pem",
	"Amazon_Root_CA_1.pem",
	"Amazon_Root_CA_2.pem",
	"Amazon_Root_CA_3.pem",
	"Amazon_Root_CA_4.pem",
	"SFS_Root_CA_G2.pem",
};

static GTlsCertificate *certs[NR_CERTS];

GSList *chime_cert_list(void)
{
	int i;
	GSList *ret = NULL;

	for (i=0; i < NR_CERTS; i++) {
		if (certs[i]) {
			g_object_ref(certs[i]);
		} else {
			GError *error = NULL;
			gchar *filename = g_build_filename(CHIME_CERTS_DIR, cert_filenames[i], NULL);
			certs[i] = g_tls_certificate_new_from_file(filename, &error);
			if (!certs[i]) {
				chime_debug("Failed to load %s: %s\n", cert_filenames[1], error->message);
				g_clear_error(&error);
				continue;
			}
			g_object_add_weak_pointer(G_OBJECT(certs[i]), (gpointer *)&certs[i]);
		}
		ret = g_slist_prepend(ret, certs[i]);
	}
	return ret;
}
