.PHONY: install clean

OUTDIR  = build
SRCDIR  = src
WRENDIR = external/wren

build: $(OUTDIR)/mod_wren.la

install: $(OUTDIR)/mod_wren.la
	apxs -a -i -n wren $(OUTDIR)/mod_wren.la

$(OUTDIR)/mod_wren.la: $(WRENDIR)/wren $(SRCDIR)/mod_wren.c Makefile
	apxs -I$(WRENDIR)/src/include \
		-c $(WRENDIR)/lib/libwren.a $(SRCDIR)/mod_wren.c \
		-o $(OUTDIR)/mod_wren.la 
	@mv -f $(SRCDIR)/mod_wren.slo $(OUTDIR)
	@mv -f $(SRCDIR)/mod_wren.lo $(OUTDIR)
	@rm -rf $(SRCDIR)/.libs

##
# Wren isn't exactly versioned at present (except for 0.1.0, in 2016), so
# we're going to grab the latest and hope it goes well.
#
$(WRENDIR):
	git clone https://github.com/munificent/wren $(WRENDIR)

$(WRENDIR)/wren: $(WRENDIR)
	cd $(WRENDIR) && \
		git checkout 40c927f4402bb6ff74fe8aa257bf8042eeff6544 && \
		git apply ../../wren_patches/map_api.diff &&   \
		git apply ../../wren_patches/unload_modules.diff && \
		make

clean:
	@rm -rf $(OUTDIR)
	@rm -rf external
