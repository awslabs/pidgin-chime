
SUBDIRS = po pixmaps fs-app-transmitter gst-chime

if BUILD_EVOPLUGIN
SUBDIRS += evolution-plugin
endif

AM_CPPFLAGS = @WFLAGS@

plugin_LTLIBRARIES = libchime.la

WEBSOCKET_SRCS = chime-websocket-connection.c chime-websocket-connection.h chime-websocket.c
PROTOBUF_SRCS = protobuf/auth_message.pb-c.c protobuf/auth_message.pb-c.h \
		protobuf/data_message.pb-c.c protobuf/data_message.pb-c.h \
		protobuf/rt_message.pb-c.c protobuf/rt_message.pb-c.h
PRPL_SRCS = chime.h chime.c buddy.c rooms.c chat.c messages.c conversations.c meeting.c attachments.c
LOGIN_SRCS = login.c login-amazon.c login-warpdrive.c login-private.h

libchime_la_SOURCES = \
	chime-connection.c chime-connection.h \
	chime-contact.c chime-contact.h \
	chime-room.c chime-room.h \
	chime-conversation.c chime-conversation.h \
	chime-object.c chime-object.h chime-props.h \
	chime-call.c chime-call.h chime-call-audio.c \
	chime-call-transport.c \
	chime-juggernaut.c \
	chime-meeting.c chime-meeting.h \
	$(WEBSOCKET_SRCS) $(PROTOBUF_SRCS) $(PRPL_SRCS) $(LOGIN_SRCS)

libchime_la_CFLAGS = $(PURPLE_CFLAGS) $(SOUP_CFLAGS) $(JSON_CFLAGS) $(LIBXML_CFLAGS) $(PROTOBUF_CFLAGS) $(OPUS_CFLAGS) $(FARSTREAM_CFLAGS) $(MARKDOWN_CFLAGS) $(GSTREAMER_CFLAGS)
libchime_la_LIBADD = $(PURPLE_LIBS) $(SOUP_LIBS) $(JSON_LIBS) $(LIBXML_LIBS) $(PROTOBUF_LIBS) $(OPUS_LIBS) $(FARSTREAM_LIBS) $(MARKDOWN_LIBS) $(GSTREAMER_LIBS)
libchime_la_LDFLAGS = -module -avoid-version -no-undefined

POTFILES = $(libchime_la_SOURCES)

po/$(PACKAGE).pot: $(POTFILES)
	@echo "Regenerating $@" ; rm -f $@ && \
	xgettext --directory=$(top_srcdir) --from-code=UTF-8 \
	  --sort-by-file --add-comments --keyword=_ --keyword=N_ \
	  --package-name="@PACKAGE@" --package-version="@VERSION@" \
	  --msgid-bugs-address=dwmw2@infradead.org \
	  -o $@ $(POTFILES)

chime-call-transport.h: protobuf/auth_message.pb-c.h
chime-call-transport.h: protobuf/rt_message.pb-c.h
chime-call-transport.h: protobuf/data_message.pb-c.h

chime-call-transport.c chime-call-audio.c chime-call.c: chime-call-transport.h

%.pb-c.c %.pb-c.h: %.proto
	$(PROTOC) $< --c_out .

EXTRA_DIST = COPYING.LGPL README

deb-%: debian/changelog
	fakeroot debian/rules $(patsubst deb-%,%,$@)

debian/changelog: debian/changelog.in
	@COMMITDESC=$$(git describe --tags | sed 's/v\(.*\)-g[0-9a-f]\+/\1/'); \
	 COMMITDATE=$$(git show -s --format=%cD); \
	 sed -e "s/%COMMITDESC%/$$COMMITDESC/" -e "s/%COMMITDATE%/$$COMMITDATE/" $< > $@

