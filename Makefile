CC = gcc
CFLAGS = -I./include -I./lib/libttak/include -Wall -Wextra -O2 -std=c23 -D_XOPEN_SOURCE_EXTENDED
LDFLAGS = -L./lib/libttak -lttak -lpthread

SRC = src/main.c src/omni_gatekeeper.c src/omni_logic.c src/tui.c src/stubs.c
OBJ = $(SRC:.c=.o)
TARGET = nsr_omni_bin
INSTALL_DIR=/usr/local/bin

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS) -lncursesw

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
install:
	cp $(TARGET) $(INSTALL_DIR)/nsr
clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all clean
