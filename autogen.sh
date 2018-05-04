#!/bin/sh
# Run this to generate all the initial makefiles, etc.

test -n "$srcdir" || srcdir=`dirname "$0"`
test -n "$srcdir" || srcdir=.

OLDDIR=`pwd`
cd $srcdir

AUTORECONF=`which autoreconf`
if test -z $AUTORECONF; then
    echo "*** No autoreconf found, please install it ***"
    exit 1
fi

aclocal --install -I m4 || exit 1
autoreconf --force --install --verbose || exit 1
./mkspec.sh

cd $OLDDIR
test -n "$NOCONFIGURE" || "$srcdir/configure" "$@"
