CC = gcc

CFLAGS = -I../../include \
    -I../../lib/libttak/include \
    -Wall -Wextra -O2 \
    -D_XOPEN_SOURCE_EXTENDED \
    -D_GNU_SOURCE

LDFLAGS = -lpthread -lcurl

TARGET = sniffer

all: $(TARGET)

$(TARGET): sniffer.c ../../src/json/json.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

install: $(TARGET)
	@mkdir -p $(HOME)/.nsr/plugins
	@rm -f $(HOME)/.nsr/plugins/*.so
	cp $(TARGET) $(HOME)/.nsr/plugins/

clean:
	rm -f $(TARGET) *.so

.PHONY: all install clean
