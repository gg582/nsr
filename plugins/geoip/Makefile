CC = gcc

HAVE_IP2LOCATION := $(shell pkg-config --exists ip2location 2>/dev/null && echo yes || echo no)

IP2LOC_CFLAGS  =
IP2LOC_LDFLAGS =
ifeq ($(HAVE_IP2LOCATION),yes)
    IP2LOC_CFLAGS  := -DHAVE_IP2LOCATION $(shell pkg-config --cflags ip2location)
    IP2LOC_LDFLAGS := $(shell pkg-config --libs ip2location)
endif

CFLAGS = -I../../include \
    -I../../lib/libttak/include \
    $(IP2LOC_CFLAGS) \
    -Wall -Wextra -O2 \
    -D_XOPEN_SOURCE_EXTENDED \
    -D_GNU_SOURCE

LDFLAGS = -lcurl $(IP2LOC_LDFLAGS)

TARGET = geoip

all: $(TARGET)

$(TARGET): geoip.c ../../src/json/json.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

install: $(TARGET)
	@mkdir -p $(HOME)/.nsr/plugins
	@rm -f $(HOME)/.nsr/plugins/*.so
	cp $(TARGET) $(HOME)/.nsr/plugins/

clean:
	rm -f $(TARGET) *.so

.PHONY: all install clean
