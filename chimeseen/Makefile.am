# @PIDGIN_PLUGIN_RULE@

pidgin_plugin_LTLIBRARIES = chimeseen.la

chimeseen_la_CFLAGS = $(PIDGIN_CFLAGS) $(JSON_CFLAGS)

chimeseen_la_SOURCES = chimeseen.c

chimeseen_la_LDFLAGS = -module -avoid-version -no-undefined

chimeseen_la_LIBADD = $(PIDGIN_LIBS) $(JSON_LIBS)

