# For COPR builds directly from git.
#
# https://docs.pagure.org/copr.copr/user_documentation.html#make-srpm
#

GITCOMMIT := $(shell git rev-parse HEAD)
GITDESC := $(shell git describe --tags HEAD)
GITDESC_WORDS := $(subst -, ,$(GITDESC))
GITTAG := $(patsubst v%,%,$(word 1,$(GITDESC_WORDS)))
GITCOUNT := $(word 2,$(GITDESC_WORDS))

ifeq ($(GITCOUNT),)
ISSNAP := 0
TARNAME := $(GITTAG)
TARPREFIX := $(GITTAG)
else
ISSNAP := 1
TARNAME := $(shell echo $(GITCOMMIT) | cut -c1-7)
TARPREFIX := $(GITCOMMIT)
endif

srpm: pidgin-chime.spec pidgin-chime-$(TARNAME).tar.gz
	rpmbuild -bs pidgin-chime.spec --define "_sourcedir ." --define "_srcrpmdir $(outdir)"

pidgin-chime.spec: ../pidgin-chime.spec.in Makefile
	sed -e "s/@ISSNAP@/$(ISSNAP)/" \
	    -e "s/@VERSION@/$(GITTAG)/" \
	    -e "s/@SNAPCOMMIT@/$(GITCOMMIT)/" \
	    -e "s/@SNAPCOUNT@/$(GITCOUNT)/" \
	    $< > $@


pidgin-chime-$(TARNAME).tar.gz Makefile:
	cd .. && git archive --prefix=pidgin-chime-$(TARPREFIX)/ $(GITCOMMIT) -o .copr/$@
