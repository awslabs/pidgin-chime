# For COPR builds directly from git.
#
# https://docs.pagure.org/copr.copr/user_documentation.html#make-srpm
#

srpm: pidgin-chime.spec /usr/bin/git
	./mkspec.sh
	set -x;	if grep -q "global gitsnapshot 1" pidgin-chime.spec; then \
		tarprefix=$$(sed -n '/^%global snapcommit /s/.* //p' $<); \
		tarname=$${tarprefix:0:7}; \
	else \
		tarprefix=$$(sed -n '/^%global tagver /s/.* //p' $<); \
		tarname=$${tarprefix}; \
	fi; \
	git archive --prefix=pidgin-chime-$${tarprefix}/ HEAD -o .copr/pidgin-chime-$${tarname}.tar.gz
	rpmbuild -bs pidgin-chime.spec --define "_sourcedir .copr" --define "_srcrpmdir $(outdir)"

/usr/bin/git:
	dnf install git

build: srpm
	rpmbuild -bb pidgin-chime.spec --define "_sourcedir `pwd`/.copr"
