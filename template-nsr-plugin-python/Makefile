# Makefile for NSR Python Plugin Template
# Adjust PLUGIN_NAME to rename your plugin.

PLUGIN_NAME = myplugin
INSTALL_DIR = $(HOME)/.nsr/plugins

all: $(PLUGIN_NAME)

$(PLUGIN_NAME): plugin.py
	cp plugin.py $(PLUGIN_NAME)
	chmod +x $(PLUGIN_NAME)

install: $(PLUGIN_NAME)
	@mkdir -p $(INSTALL_DIR)
	cp $(PLUGIN_NAME) $(INSTALL_DIR)/

clean:
	rm -f $(PLUGIN_NAME)

.PHONY: all install clean
