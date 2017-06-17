AC_INIT(pidgin-chime, 0.01)

PKG_PROG_PKG_CONFIG
AC_LANG_C
AC_CANONICAL_HOST
AM_MAINTAINER_MODE([enable])
AM_INIT_AUTOMAKE([foreign tar-ustar])
m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])
LT_INIT

AC_PREREQ([2.62], [], [AC_SUBST([localedir], ['$(datadir)/locale'])])

PKG_CHECK_MODULES(PURPLE, [purple-3], [],
		  [PKG_CHECK_MODULES(PURPLE, [purple >= 2.4.0])])

PKG_CHECK_MODULES(SOUP, [libsoup-2.4])
PKG_CHECK_MODULES(JSON, [json-glib-1.0])

AC_CONFIG_FILES(Makefile)
AC_OUTPUT
