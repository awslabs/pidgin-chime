gstplugin_LTLIBRARIES = libgstchime.la

libgstchime_la_CFLAGS = $(GSTREAMER_CFLAGS)
libgstchime_la_LIBADD = $(GSTREAMER_LIBS)
libgstchime_la_LDFLAGS = -module -avoid-version -no-undefined

libgstchime_la_SOURCES =	\
	gstchime.c		\
	gstrtpchimepay.c	\
	gstrtpchimedepay.c

noinst_HEADERS =		\
	gstrtpchimepay.h	\
	gstrtpchimedepay.h

EXTRA_DIST = README
