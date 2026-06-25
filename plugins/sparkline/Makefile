TARGET = sparkline
INSTALL_DIR = $(HOME)/.nsr/plugins

all: $(TARGET)

$(TARGET): sparkline.py
	cp sparkline.py $(TARGET)
	chmod +x $(TARGET)

install: $(TARGET)
	@mkdir -p $(INSTALL_DIR)
	cp $(TARGET) $(INSTALL_DIR)/

clean:
	rm -f $(TARGET)

.PHONY: all install clean
