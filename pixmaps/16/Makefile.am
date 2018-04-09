
size = 16
EXTRA_DIST = chime.png
DISTCLEANFILES = chime.png

pixmap_scalabledir = $(pixmapdir)/$(size)
pixmap_scalable_DATA = chime.png

chime.png: $(srcdir)/../scalable/chime_small.svg
	convert +antialias -trim -background transparent -resize $(size)x$(size) $< $@


