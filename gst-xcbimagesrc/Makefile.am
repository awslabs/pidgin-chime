gstplugin_LTLIBRARIES = libgstxcbimagesrc.la

libgstxcbimagesrc_la_CFLAGS = $(GSTREAMER_CFLAGS) $(GSTBASE_CFLAGS) $(GSTVIDEO_CFLAGS) $(X11_CFLAGS) $(XEXT_CFLAGS) $(XFIXES_CFLAGS) $(XDAMAGE_CFLAGS)
libgstxcbimagesrc_la_LIBADD = $(GSTREAMER_LIBS) $(GSTBASE_LIBS) $(GSTVIDEO_LIBS) $(X11_LIBS) $(XEXT_LIBS) $(XFIXES_LIBS) $(XDAMAGE_LIBS)
libgstxcbimagesrc_la_LDFLAGS = -module -avoid-version -no-undefined

libgstxcbimagesrc_la_SOURCES =	\
	gstxcbimagesrc.c	\
	xcbimageutil.c

noinst_HEADERS =		\
	gstxcbimagesrc.h	\
	xcbimageutil.h

#EXTRA_DIST = README
