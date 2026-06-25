# Makefile for nsr-plugged (NSR Plugin Manager)

TARGET = nsr-plugged
INSTALL_DIR = $(HOME)/.local/bin

all: $(TARGET)
	chmod +x $(TARGET)

install: all
	@mkdir -p $(INSTALL_DIR)
	cp $(TARGET) $(INSTALL_DIR)/
	@echo "nsr-plugged installed to $(INSTALL_DIR)."
	@echo "Ensure $(INSTALL_DIR) is in your PATH."

clean:
	@echo "Nothing to clean for nsr-plugged."

.PHONY: all install clean
