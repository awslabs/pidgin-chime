
lib_LTLIBRARIES = libchime.la

libchime_la_SOURCES = chime.c jugg.c buddy.c chime.h
libchime_la_CFLAGS = $(PURPLE_CFLAGS) $(SOUP_CFLAGS) $(JSON_CFLAGS)
libchime_la_LIBADD = $(PURPLE_LIBS) $(SOUP_LIBS) $(JSON_LIBS)
libchime_la_LDFLAGS = -module -avoid-version -no-undefined
