SUBDIRS = geoip percentile sniffer

all:
	@for d in $(SUBDIRS); do $(MAKE) -C $$d; done

install:
	@mkdir -p $(HOME)/.nsr/plugins
	@rm -f $(HOME)/.nsr/plugins/*.so
	@for d in $(SUBDIRS); do $(MAKE) -C $$d install; done

clean:
	@for d in $(SUBDIRS); do $(MAKE) -C $$d clean; done

.PHONY: all install clean
