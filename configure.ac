AC_INIT(pidgin-chime, 0.01)

PKG_PROG_PKG_CONFIG
AC_LANG_C
AC_CANONICAL_HOST
AC_DISABLE_STATIC
AM_MAINTAINER_MODE([enable])
AM_INIT_AUTOMAKE([foreign tar-ustar])
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
PKG_CHECK_MODULES(PURPLE, [purple-3], [plugindir="$($PKG_CONFIG --variable=plugindir purple-3)"],
		  [PKG_CHECK_MODULES(PURPLE, [purple >= 2.4.0])
		   plugindir="$($PKG_CONFIG --variable=plugindir purple)"
		   purple_datadir="$($PKG_CONFIG --variable=datadir purple)"
		   ])

AC_SUBST(plugindir, $plugindir)
if [ "$purple_datadir" = "" ]; then
   purple_datadir = "$datadir"
fi
AC_SUBST(pixmapdir, ${purple_datadir}/pixmaps/pidgin/protocols)
PKG_CHECK_MODULES(SOUP, [libsoup-2.4])
PKG_CHECK_MODULES(JSON, [json-glib-1.0])

AC_PATH_PROG(MSGFMT, msgfmt)
if test "$MSGFMT" = ""; then
   AC_ERROR([msgfmt could not be found. Try configuring with --disable-nls])
fi

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
	])
AC_OUTPUT
