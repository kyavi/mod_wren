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
# Wren isn't exactly versioned at present (except for 1.0.0, in 2016), so
# we're going to grab the latest and hope it goes well.
#
$(WRENDIR):
	git clone https://github.com/munificent/wren $(WRENDIR)

$(WRENDIR)/wren: $(WRENDIR)
	cd $(WRENDIR) && make

clean:
	@rm -rf $(OUTDIR)
	@rm -rf external
