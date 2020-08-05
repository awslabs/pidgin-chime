/*
 * Copyright © 2020 Amazon.com, Inc. or its affiliates.
 *
 * Authors: Ignacio Casal Quinteiro <icq@gnome.org>
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

#include "application.h"
#include "dirs.h"

#include <locale.h>
#include <libintl.h>

static gchar *
get_locale_dir(void)
{
    gchar *locale_dir;
    gchar *data_dir;

    data_dir = chime_dirs_get_data_dir();
    locale_dir = g_build_filename(data_dir, "locale", NULL);
    g_free(data_dir);

    return locale_dir;
}

static int
run_application(int    argc,
                char **argv)
{
    ChimeApplication *app;
    int result;
    gchar *dir;

    /* Setup locale/gettext */
    setlocale(LC_ALL, "");

    dir = get_locale_dir();
    bindtextdomain(GETTEXT_PACKAGE, dir);
    g_free(dir);

    bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
    textdomain(GETTEXT_PACKAGE);

    app = chime_application_new();

    result = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);

    return result;
}

int
main(int    argc,
     char **argv)
{
    return run_application(argc, argv);
}

/* ex:set ts=4 et: */
