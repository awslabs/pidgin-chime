# For COPR builds directly from git.
#
# https://docs.pagure.org/copr.copr/user_documentation.html#make-srpm
#
WTF := $(shell dnf install git)

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

srpm: .copr/pidgin-chime.spec .copr/pidgin-chime-$(TARNAME).tar.gz
	rpmbuild -bs .copr/pidgin-chime.spec --define "_sourcedir .copr" --define "_srcrpmdir $(outdir)"

.copr/pidgin-chime.spec: pidgin-chime.spec.in .copr/Makefile
	sed -e "s/@ISSNAP@/$(ISSNAP)/" \
	    -e "s/@VERSION@/$(GITTAG)/" \
	    -e "s/@SNAPCOMMIT@/$(GITCOMMIT)/" \
	    -e "s/@SNAPCOUNT@/$(GITCOUNT)/" \
	    $< > $@


.copr/pidgin-chime-$(TARNAME).tar.gz ./copr/Makefile:
	git archive --prefix=pidgin-chime-$(TARPREFIX)/ $(GITCOMMIT) -o $@
