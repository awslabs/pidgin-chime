
#include <libpurple/prpl.h>
#include <libpurple/version.h>

static gboolean chime_purple_plugin_load(PurplePlugin *plugin)
{
	return FALSE;
}

static gboolean chime_purple_plugin_unload(PurplePlugin *plugin)
{
	return TRUE;
}

void chime_purple_plugin_destroy(PurplePlugin *plugin)
{
}


static PurplePluginProtocolInfo chime_prpl_info = {
};

static PurplePluginInfo chime_plugin_info = {
	.magic = PURPLE_PLUGIN_MAGIC,
	.major_version = PURPLE_MAJOR_VERSION,
	.minor_version = PURPLE_MINOR_VERSION,
	.type = PURPLE_PLUGIN_PROTOCOL,
	.priority = PURPLE_PRIORITY_DEFAULT,
	.id = "prpl-chime",
	.name = "Amazon Chime",
	.version = PACKAGE_VERSION,
	.summary = "Amazon Chime Protocol Plugin",
	.description = "A plugin for Chime",
	.author = "David Woodhouse <dwmw2@infradead.org>",
	.load = chime_purple_plugin_load,
	.unload = chime_purple_plugin_unload,
	.destroy = chime_purple_plugin_destroy,
	.extra_info = &chime_prpl_info,
};
