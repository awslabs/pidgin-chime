
SUBDIRS = po pixmaps

if BUILD_EVOPLUGIN
SUBDIRS += evolution-plugin
endif

AM_CPPFLAGS = @WFLAGS@

plugin_LTLIBRARIES = libchime.la

libchime_la_SOURCES = chime.c buddy.c rooms.c chat.c messages.c conversations.c \
	chime-connection.c chime-connection.h chime-contact.c chime-contact.h \
	chime-room.h chime-room.c chime-conversation.h chime-conversation.c \
	chime-juggernaut.c chime-object.h chime-object.c chime.h \
	login.c login-amazon.c login-warpdrive.c login-private.h \
	chime-websocket-connection.c chime-websocket-connection.h chime-websocket.c \
	chime-meeting.c chime-meeting.h meeting.c chime-props.h \
	chime-call.c chime-call.h chime-call-audio.c attachments.c \
	protobuf/auth_message.pb-c.c protobuf/auth_message.pb-c.h \
	protobuf/data_message.pb-c.c protobuf/data_message.pb-c.h \
	protobuf/rt_message.pb-c.c protobuf/rt_message.pb-c.h

libchime_la_CFLAGS = $(PURPLE_CFLAGS) $(SOUP_CFLAGS) $(JSON_CFLAGS) $(LIBXML_CFLAGS) $(PROTOBUF_CFLAGS) $(OPUS_CFLAGS)
libchime_la_LIBADD = $(PURPLE_LIBS) $(SOUP_LIBS) $(JSON_LIBS) $(LIBXML_LIBS) $(PROTOBUF_LIBS) $(OPUS_LIBS)
libchime_la_LDFLAGS = -module -avoid-version -no-undefined

POTFILES = $(libchime_la_SOURCES)

po/$(PACKAGE).pot: $(POTFILES)
	@echo "Regenerating $@" ; rm -f $@ && \
	xgettext --directory=$(top_srcdir) --from-code=UTF-8 \
	  --sort-by-file --add-comments --keyword=_ --keyword=N_ \
	  --package-name="@PACKAGE@" --package-version="@VERSION@" \
	  --msgid-bugs-address=dwmw2@infradead.org \
	  -o $@ $(POTFILES)

chime-call-audio.c: protobuf/auth_message.pb-c.h
chime-call-audio.c: protobuf/rt_message.pb-c.h
chime-call-audio.c: protobuf/data_message.pb-c.h

%.pb-c.c %.pb-c.h: %.proto
	$(PROTOC) $< --c_out .

EXTRA_DIST = COPYING.LGPL README
