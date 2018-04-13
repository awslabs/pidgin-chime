AM_CFLAGS = @WFLAGS@

fsplugin_LTLIBRARIES = libapp-transmitter.la

libapp_transmitter_la_CPPFLAGS = $(FARSTREAM_CFLAGS)

libapp_transmitter_la_SOURCES = fs-app-stream-transmitter.c fs-app-stream-transmitter.h \
					fs-app-transmitter.c  fs-app-transmitter.h

libapp_transmitter_la_LDFLAGS = -module -avoid-version -no-undefined
