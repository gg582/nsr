CC = gcc

CFLAGS = \
    -I./include \
    -I./lib/libttak/include \
    -Wall \
    -Wextra \
    -O3 \
    -march=native \
    -mtune=native \
    -flto=auto \
    -fomit-frame-pointer \
    -fno-plt \
    -fvisibility=hidden \
    -ffunction-sections \
    -fdata-sections \
    -fstrict-aliasing \
    -fno-math-errno \
    -fno-asynchronous-unwind-tables \
    -std=c2x \
    -D_XOPEN_SOURCE_EXTENDED \
	-D_GNU_SOURCE
LDFLAGS = \
    -L./lib/libttak/lib \
    -lttak \
    -lstdc++ \
    -lpthread \
    -lncursesw \
    -flto \
    -Wl,--gc-sections \
    -Wl,--as-needed \
    -Wl,--strip-all \
    -Wl,-O3

CORE_SRC = \
    src/core/main.c \
    src/io/gatekeeper.c \
    src/io/logic.c \
    src/ui/tui.c \
    src/ui/key_slots.c \
    src/state/topology.c \
    src/util/stubs.c \
    src/plugin/plugin.c \
    src/json/json.c \
    src/json/json_rpc.c

CORE_OBJ = $(CORE_SRC:.c=.o)

BENCH_SRC = bench_nsr.c

TARGET = nsr
BENCH_TARGET = nsr_bench
BENCH_E2E_TARGET = nsr_bench_e2e

TTAK_DIR = lib/libttak
TTAK_LIB = $(TTAK_DIR)/lib/libttak.a

all: $(TARGET) $(BENCH_TARGET) $(BENCH_E2E_TARGET) nsr-plugged-build

nsr-plugged-build:
	$(MAKE) -C nsr-plugged

$(TTAK_LIB):
	$(MAKE) -C $(TTAK_DIR) USE_CUDA=0

$(TARGET): $(CORE_SRC) $(TTAK_LIB)
	$(CC) $(CFLAGS) $(CORE_SRC) -o $@ $(LDFLAGS)

$(BENCH_TARGET): bench_nsr.c $(TTAK_LIB)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

$(BENCH_E2E_TARGET): bench_nsr_e2e.c \
    src/io/gatekeeper.o src/io/logic.o src/util/stubs.o \
    src/ui/tui.o src/state/topology.o src/plugin/plugin.o \
    src/json/json.o src/json/json_rpc.o $(TTAK_LIB)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

install: nsr-plugged-install plugins-install
	cp $(TARGET) /usr/local/bin/nsr

nsr-plugged-install:
	$(MAKE) -C nsr-plugged install

plugins-install:
	$(MAKE) -C plugins install

clean:
	rm -f src/*.o *.o plugins/*/*.o $(TARGET) $(BENCH_TARGET) $(BENCH_E2E_TARGET)
	$(MAKE) -C plugins clean
	$(MAKE) -C nsr-plugged clean
	$(MAKE) -C $(TTAK_DIR) clean

.PHONY: all clean install plugins-install nsr-plugged-build nsr-plugged-install
