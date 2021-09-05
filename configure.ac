AC_INIT(pidgin-chime, 1.4.1)

PKG_PROG_PKG_CONFIG
AC_LANG_C
AC_CANONICAL_HOST
AC_DISABLE_STATIC
AM_MAINTAINER_MODE([enable])
AM_INIT_AUTOMAKE([foreign tar-pax subdir-objects])
m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])
LT_INIT

AC_PREREQ([2.62], [], [AC_SUBST([localedir], ['$(datadir)/locale'])])

AS_COMPILER_FLAGS(WFLAGS,
        "-Wall
         -Wextra
         -Wno-missing-field-initializers
         -Wno-sign-compare
         -Wno-unused-parameter
         -Werror=pointer-to-int-cast
         -Wdeclaration-after-statement
         -Werror-implicit-function-declaration
         -Wformat-nonliteral
         -Wformat-security
         -Winit-self
         -Wmissing-declarations
         -Wmissing-include-dirs
         -Wnested-externs
         -Wpointer-arith
         -Wwrite-strings
	 -Wno-declaration-after-statement")
AC_SUBST(WFLAGS, [$WFLAGS])

AC_ARG_WITH([certsdir],
	[AS_HELP_STRING([--with-certsdir],
		[install directory for Amazon CA certs])],
		[], [with_certsdir='$(pkgdatadir)'])
AC_SUBST([certsdir], [${with_certsdir}])

purple_plugindir=
PKG_CHECK_MODULES(PURPLE, [purple >= 2.8.0], [purple_pkg=purple])

purple_plugindir="$($PKG_CONFIG --variable=plugindir $purple_pkg)"
AC_SUBST(purple_plugindir, $purple_plugindir)

PKG_CHECK_MODULES(PIDGIN, [pidgin], [pidgin_pkg=pidgin])

pidgin_datadir="$($PKG_CONFIG --variable=datadir $pidgin_pkg)"
pidgin_plugindir="$($PKG_CONFIG --variable=plugindir $pidgin_pkg)"
if test "$purple_datadir" = "" ; then
   purple_datadir="$datadir"
fi
AC_SUBST(pixmapdir, ${pidgin_datadir}/pixmaps/pidgin/protocols)
AC_SUBST(pidgin_plugindir, $pidgin_plugindir)

DLOPEN_LIBS=""
AC_SEARCH_LIBS([dlopen],[dl dld],
  [test "$ac_cv_search_dlopen" == "none required" || DLOPEN_LIBS="$ac_cv_search_dlopen"],
  [AC_MSG_ERROR([Unable to find dlopen function])]
)
AC_SUBST([DLOPEN_LIBS])

PKG_CHECK_MODULES(DBUS, [dbus-1])
PKG_CHECK_MODULES(GNUTLS, [gnutls >= 3.2.0])
PKG_CHECK_MODULES(FARSTREAM, [farstream-0.2])
PKG_CHECK_MODULES(GSTREAMER, [gstreamer-1.0])
PKG_CHECK_MODULES(GSTAPP, [gstreamer-app-1.0])
PKG_CHECK_MODULES(GSTRTP, [gstreamer-rtp-1.0])
PKG_CHECK_MODULES(GSTVIDEO, [gstreamer-video-1.0])
PKG_CHECK_MODULES(OPUS, [opus])
PKG_CHECK_MODULES(PROTOBUF, [libprotobuf-c])
PKG_CHECK_MODULES(JSON, [json-glib-1.0])
PKG_CHECK_MODULES(LIBXML, [libxml-2.0])
PKG_CHECK_MODULES(SOUP, [libsoup-2.4 >= 2.50])
if $PKG_CONFIG --atleast-version 2.59 libsoup-2.4; then
   AC_DEFINE(USE_LIBSOUP_WEBSOCKETS, 1, [Use libsoup websockets])
fi
PKG_CHECK_MODULES(MARKDOWN, [libmarkdown], [],
		  [oldLIBS="$LIBS"
		   LIBS="$LIBS -lmarkdown"
		   AC_MSG_CHECKING([for libmarkdown without pkg-config])
		   AC_LINK_IFELSE([AC_LANG_PROGRAM([#include <mkdio.h>],[mkd_initialize();])],
				  [AC_MSG_RESULT(yes)
				   AC_SUBST([MARKDOWN_LIBS], ["-lmarkdown"])
				   AC_SUBST([MARKDOWN_CFLAGS], [])],
				  [AC_MSG_RESULT(no)
				   AC_ERROR([Could not build against libmarkdown])])
		   LIBS="$oldLIBS"])

PKG_CHECK_MODULES(X11, [x11], [have_x11=yes], [have_x11=no])
PKG_CHECK_MODULES(GSTBASE, [gstreamer-base-1.0], [have_gstbase=yes], [have_gstbase=no])
PKG_CHECK_MODULES(XCB, [xcb], [have_xcb=yes], [have_xcb=no]);
AM_CONDITIONAL(BUILD_GSTXCBIMAGESRC, [ test "$have_x11$have_gstbase$have_xcb" = "yesyesyes" ])

if test "$have_x11" = "yes"; then
   PKG_CHECK_MODULES(XDAMAGE, xdamage, AC_DEFINE(HAVE_XDAMAGE, 1, [xdamage]), [:])
   PKG_CHECK_MODULES(XFIXES, xfixes, AC_DEFINE(HAVE_XFIXES, 1, [xfixes]), [:])
   PKG_CHECK_MODULES(XEXT, "xext",
   			   [AC_CHECK_LIB([Xext], [XShmAttach],
			   			 AC_DEFINE(HAVE_XSHM, 1, [xshm]))], [:])
fi

LIBS="$LIBS $PURPLE_LIBS"
AC_CHECK_FUNC(purple_request_screenshare_media, [AC_DEFINE(HAVE_SCREENSHARE, 1, [Have purple_request_screenshare_media()])], [])
AC_CHECK_FUNC(serv_chat_send_file, [AC_DEFINE(HAVE_CHAT_SEND_FILE, 1, [Have serv_chat_send_file()])], [])
LIBS="$oldLIBS"

gstplugindir="$($PKG_CONFIG --variable=pluginsdir gstreamer-1.0)"
AC_SUBST(gstplugindir, ${gstplugindir})

AC_PATH_PROG(MSGFMT, msgfmt)
if test "$MSGFMT" = ""; then
   AC_MSG_ERROR([msgfmt could not be found. Try configuring with --disable-nls])
fi


AC_PATH_PROG(PROTOC, protoc-c)
if test "$PROTOC" = ""; then
   AC_MSG_ERROR([protoc-c could not be found. Try installing protobuf-c-compiler])
fi

AC_ARG_WITH([evolution],
            [AS_HELP_STRING([--without-evolution],
              [Disable evolution plugin])],
            [with_evolution=no])

AS_IF([test "x$with_evolution" != xno],
      [ if $PKG_CONFIG --atleast-version 3.33 libecal-2.0; then
           EVOLUTION_MODULES="evolution-calendar-3.0 evolution-shell-3.0 evolution-data-server-1.2 libecal-2.0 camel-1.2"
           AC_DEFINE(EDS_IS_LIBECAL_2_0, 1, [Since 3.33.2])
        else
           EVOLUTION_MODULES="evolution-calendar-3.0 evolution-shell-3.0 evolution-data-server-1.2 libecal-1.2 camel-1.2"
        fi

        PKG_CHECK_MODULES(EVOLUTION, [$EVOLUTION_MODULES],
	[with_evolution=yes
	 if $PKG_CONFIG --atleast-version 3.20 evolution-shell-3.0; then
	    AC_DEFINE(EVO_HAS_E_COMP_EDITOR, 1, [Since 3.20])
	 fi
	 evomoddir="$($PKG_CONFIG --variable=moduledir evolution-shell-3.0)"
	 AC_SUBST(evomoddir, ${evomoddir})],
	[:])])

AM_CONDITIONAL(BUILD_EVOPLUGIN, [ test "$with_evolution" = "yes" ])

fsplugindir="$($PKG_CONFIG --variable=pluginsdir farstream-0.2)"
AC_SUBST(fsplugindir, ${fsplugindir})

AC_SUBST([CONFIG_STATUS_DEPENDENCIES],
	['$(top_srcdir)/po/LINGUAS'])

RAWLINGUAS=`sed -e "/^#/d" -e "s/#.*//" "${srcdir}/po/LINGUAS"`
# Remove newlines
LINGUAS=`echo $RAWLINGUAS`
AC_SUBST(LINGUAS)

AC_CONFIG_FILES([
	Makefile
	po/Makefile
	pixmaps/Makefile
	pixmaps/scalable/Makefile
	pixmaps/16/Makefile
	pixmaps/22/Makefile
	pixmaps/48/Makefile
	evolution-plugin/Makefile
	fs-app-transmitter/Makefile
	gst-chime/Makefile
	gst-xcbimagesrc/Makefile
	chimeseen/Makefile
	])
AC_OUTPUT
