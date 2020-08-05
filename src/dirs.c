#include <config.h>

#include "dirs.h"

gchar *
chime_dirs_get_data_dir(void)
{
    return g_strdup(DATADIR);
}

gchar *
chime_dirs_get_lib_dir(void)
{
    return g_strdup(LIBDIR);
}

/* ex:set ts=4 et: */
