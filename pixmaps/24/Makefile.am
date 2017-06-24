
size = 24
EXTRA_DIST = chime.png
DISTCLEANFILES = chime.png

pixmap_scalabledir = $(pixmapdir)/$(size)
pixmap_scalable_DATA = chime.png

chime.png: $(srcdir)/../scalable/chime.svg
	convert +antialias -background transparent -resize $(size)x$(size) $< $@


