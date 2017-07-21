
SUBDIRS = po pixmaps

AM_CPPFLAGS = @WFLAGS@

plugin_LTLIBRARIES = libchime.la

libchime_la_SOURCES = chime.c buddy.c rooms.c chat.c messages.c conversations.c login.c \
	chime-connection.c chime-connection.h chime-contact.c chime-contact.h chime-room.c \
	chime-juggernaut.c chime.h
libchime_la_CFLAGS = $(PURPLE_CFLAGS) $(SOUP_CFLAGS) $(JSON_CFLAGS) $(LIBXML_CFLAGS)
libchime_la_LIBADD = $(PURPLE_LIBS) $(SOUP_LIBS) $(JSON_LIBS) $(LIBXML_LIBS)
libchime_la_LDFLAGS = -module -avoid-version -no-undefined

POTFILES = $(libchime_la_SOURCES)

po/$(PACKAGE).pot: $(POTFILES)
	@echo "Regenerating $@" ; rm -f $@ && \
	xgettext --directory=$(top_srcdir) --from-code=UTF-8 \
	  --sort-by-file --add-comments --keyword=_ --keyword=N_ \
	  --package-name="@PACKAGE@" --package-version="@VERSION@" \
	  --msgid-bugs-address=dwmw2@infradead.org \
	  -o $@ $(POTFILES)


EXTRA_DIST = COPYING.LGPL README
