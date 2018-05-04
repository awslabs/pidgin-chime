
SUBDIRS = po pixmaps fs-app-transmitter gst-chime chimeseen

if BUILD_EVOPLUGIN
SUBDIRS += evolution-plugin
endif

AM_CPPFLAGS = @WFLAGS@

certs_DATA = certs/Amazon.com_InfoSec_CA_G3.pem \
	       certs/Amazon.com_Internal_Root_Certificate_Authority.pem \
	       certs/Amazon_Root_CA_1.pem \
	       certs/Amazon_Root_CA_2.pem \
	       certs/Amazon_Root_CA_3.pem \
	       certs/Amazon_Root_CA_4.pem \
	       certs/SFS_Root_CA_G2.pem

purple_plugin_LTLIBRARIES = libchimeprpl.la

PROTOBUF_SRCS = protobuf/auth_message.pb-c.c protobuf/auth_message.pb-c.h \
		protobuf/data_message.pb-c.c protobuf/data_message.pb-c.h \
		protobuf/rt_message.pb-c.c protobuf/rt_message.pb-c.h

PRPL_SRCS =	prpl/chime.h prpl/chime.c prpl/buddy.c prpl/rooms.c prpl/chat.c \
		prpl/messages.c prpl/conversations.c prpl/meeting.c prpl/attachments.c \
		prpl/authenticate.c

WEBSOCKET_SRCS = chime/chime-websocket-connection.c chime/chime-websocket-connection.h \
		chime/chime-websocket.c

CHIME_SRCS =	chime/chime-connection.c chime/chime-connection.h \
		chime/chime-connection-private.h chime/chime-certs.c \
		chime/chime-contact.c chime/chime-contact.h \
		chime/chime-room.c chime/chime-room.h \
		chime/chime-conversation.c chime/chime-conversation.h \
		chime/chime-object.c chime/chime-object.h chime/chime-props.h \
		chime/chime-call.c chime/chime-call.h \
		chime/chime-call-audio.c chime/chime-call-audio.h \
		chime/chime-call-transport.c \
		chime/chime-call-screen.c chime/chime-call-screen.h \
		chime/chime-juggernaut.c \
		chime/chime-signin.c \
		chime/chime-meeting.c chime/chime-meeting.h

EXTRA_PROGRAMS = chime-get-token
chime_get_token_SOURCES = chime-get-token.c
chime_get_token_CFLAGS = $(SOUP_CFLAGS) $(JSON_CFLAGS)
chime_get_token_LDADD = libchime.la

noinst_LTLIBRARIES = libchime.la

libchime_la_SOURCES = $(CHIME_SRCS) $(WEBSOCKET_SRCS) $(PROTOBUF_SRCS)
libchime_la_CFLAGS = $(SOUP_CFLAGS) $(JSON_CFLAGS) $(LIBXML_CFLAGS) $(PROTOBUF_CFLAGS) $(GSTREAMER_CFLAGS) $(GSTRTP_CFLAGS) $(GSTAPP_CFLAGS) $(GSTVIDEO_CFLAGS) $(GNUTLS_CFLAGS) -Ichime -DCHIME_CERTS_DIR=\"$(certsdir)\"
libchime_la_LIBADD = $(SOUP_LIBS) $(JSON_LIBS) $(LIBXML_LIBS) $(PROTOBUF_LIBS) $(GSTREAMER_LIBS) $(GSTRTP_LIBS) $(GSTAPP_LIBS) $(GSTVIDEO_LIBS) $(GNUTLS_LIBS)
libchime_la_LDFLAGS = -module -avoid-version -no-undefined

libchimeprpl_la_SOURCES = $(PRPL_SRCS) $(LOGIN_SRCS)
libchimeprpl_la_CFLAGS = $(PURPLE_CFLAGS) $(SOUP_CFLAGS) $(JSON_CFLAGS) $(LIBXML_CFLAGS) $(GSTREAMER_CFLAGS) -Ichime -Iprpl
libchimeprpl_la_LIBADD = $(PURPLE_LIBS) $(SOUP_LIBS) $(JSON_LIBS) $(LIBXML_LIBS) $(GSTREAMER_LIBS) libchime.la
libchimeprpl_la_LDFLAGS = -module -avoid-version -no-undefined

POTFILES = $(libchime_la_SOURCES) $(libchimeprpl_la_SOURCES)

po/$(PACKAGE).pot: $(POTFILES)
	@echo "Regenerating $@" ; rm -f $@ && \
	xgettext --directory=$(top_srcdir) --from-code=UTF-8 \
	  --sort-by-file --add-comments --keyword=_ --keyword=N_ \
	  --package-name="@PACKAGE@" --package-version="@VERSION@" \
	  --msgid-bugs-address=dwmw2@infradead.org \
	  -o $@ $(POTFILES)

chime/chime-call-transport.h: protobuf/auth_message.pb-c.h
chime/chime-call-transport.h: protobuf/rt_message.pb-c.h
chime/chime-call-transport.h: protobuf/data_message.pb-c.h

chime/chime-call-transport.c chime/chime-call-audio.c chime/chime-call.c: chime/chime-call-transport.h

%.pb-c.c %.pb-c.h: %.proto
	$(PROTOC) $< --c_out .

EXTRA_DIST = LICENSE README pidgin-chime.spec pidgin-chime.spec.in $(certs_DATA)

deb-%: debian/changelog
	fakeroot debian/rules $(patsubst deb-%,%,$@)

debian/changelog: debian/changelog.in
	@COMMITDESC=$$(git describe --tags | sed 's/v\(.*\)-g[0-9a-f]\+/\1/'); \
	 COMMITDATE=$$(git show -s --format=%cD); \
	 sed -e "s/%COMMITDESC%/$$COMMITDESC/" -e "s/%COMMITDATE%/$$COMMITDATE/" $< > $@

tmp-dist: uncommitted-check
	$(MAKE) $(AM_MAKEFLAGS) VERSION=$(patsubst v%,%,$(shell git describe --tags)) DISTHOOK=0 dist

uncommitted-check:
	@if ! git update-index --refresh --unmerged || \
	    ! git diff-index --name-only --exit-code HEAD; then \
		echo "*** ERROR: Uncommitted changes in above files"; exit 1; fi

DISTHOOK=1
dist-hook: uncommitted-check
	@if [ $(DISTHOOK) = 1 ]; then \
	    if ! git rev-parse --verify v$(VERSION) &> /dev/null; then \
		echo "*** ERROR: Version v$(VERSION) is not tagged"; exit 1; fi ; \
	    if ! git diff --name-only --exit-code v$(VERSION) HEAD > /dev/null; then \
		echo "*** ERROR: Git checkout not at version v$(VERSION)"; exit 1; fi ; \
	fi
	./mkspec.sh

sign-dist: dist
	@for a in $(DIST_ARCHIVES); do \
		gpg --default-key 0AC01544 --detach-sign -a $$a ; \
	done

tag: uncommitted-check
	@if git rev-parse --verify v$(VERSION) &> /dev/null; then \
		echo "*** ERROR: Version v$(VERSION) is already tagged"; exit 1; fi
	@sed 's/AC_INIT.*/AC_INIT(pidgin-chime, $(VERSION))/' -i $(srcdir)/configure.ac
	@cd $(srcdir) && git commit -s -m "Tag version $(VERSION)" configure.ac
	@git tag v$(VERSION)
	@cd $(srcdir) && ./autogen.sh
