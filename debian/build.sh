#!/bin/bash

set -exo pipefail

PKGNAME=pidgin-chime
COMMITDESC=$(git describe --tags | sed  -e 's/$$/-0/' -e 's/v\([^-]\+-[^-]\+\)\(-g[0-9a-f-]\+\|\)/\1/')
COMMITDATE="$(git show -s --format=%cD)"
SCRATCHDIR=$(mktemp -d)

git archive HEAD --prefix=${PKGNAME}/ \
    | xz -c \
	 > $SCRATCHDIR/${PKGNAME}.tar
cd $SCRATCHDIR
tar xvf ${PKGNAME}.tar
cd ${PKGNAME}

NOCONFIGURE=1 ./autogen.sh
for DISTRO in bionic focal ; do
    make -f debian/rules.maint clean all DISTRIB_CODENAME=$DISTRO COMMITDESC=$COMMITDESC COMMITDATE="${COMMITDATE}"
    debuild -S -d
    dput pidgin-chime ../${PKGNAME}_${COMMITDESC}~${DISTRO}+1_source.changes
done

cd /tmp
rm -r $SCRATCHDIR
