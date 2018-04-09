AC_INIT(pidgin-chime, 0.02)

PKG_PROG_PKG_CONFIG
AC_LANG_C
AC_CANONICAL_HOST
AC_DISABLE_STATIC
AM_MAINTAINER_MODE([enable])
AM_INIT_AUTOMAKE([foreign tar-ustar subdir-objects])
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

plugindir=
PKG_CHECK_MODULES(PURPLE, [purple >= 2.4.0],
			  [purple_pkg=purple],
			  [PKG_CHECK_MODULES(PURPLE, purple-3,
					    [AC_MSG_ERROR([Pidgin 3.0 is not yet released. Do not use it. It has nothing you need for Lync support that I didn't already backport to Pidgin 2.11.])])])


plugindir="$($PKG_CONFIG --variable=plugindir $purple_pkg)"
purple_datadir="$($PKG_CONFIG --variable=datadir $purple_pkg)"

AC_SUBST(plugindir, $plugindir)
if test "$purple_datadir" = "" ; then
   purple_datadir="$datadir"
fi
AC_SUBST(pixmapdir, ${purple_datadir}/pixmaps/pidgin/protocols)

PKG_CHECK_MODULES(GNUTLS, [gnutls >= 3.2.0])
PKG_CHECK_MODULES(FARSTREAM, [farstream-0.2])
PKG_CHECK_MODULES(GSTREAMER, [gstreamer-1.0 gstreamer-app-1.0 gstreamer-rtp-1.0])
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

build_evoplugin=
PKG_CHECK_MODULES(EVOLUTION, [evolution-calendar-3.0 evolution-shell-3.0 evolution-data-server-1.2 libebook-1.2 libecal-1.2],
		  [build_evoplugin=yes
		   if $PKG_CONFIG --atleast-version 3.20 evolution-shell-3.0; then
		      AC_DEFINE(EVO_HAS_E_COMP_EDITOR, 1, [Since 3.20])
		   fi
		   evomoddir="$($PKG_CONFIG --variable=moduledir evolution-shell-3.0)"
		   AC_SUBST(evomoddir, ${evomoddir})],
		  [:]);
AM_CONDITIONAL(BUILD_EVOPLUGIN, [ test "$build_evoplugin" = "yes" ])

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
	pixmaps/24/Makefile
	pixmaps/32/Makefile
	pixmaps/48/Makefile
	evolution-plugin/Makefile
	fs-app-transmitter/Makefile
	gst-chime/Makefile
	])
AC_OUTPUT
