CC = gcc

CFLAGS = \
    -I./include \
    -I./lib/libttak/include \
    -Wall \
    -Wextra \
    -Ofast \
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
    -D_XOPEN_SOURCE_EXTENDED
LDFLAGS = \
    -L./lib/libttak \
    -lttak \
    -lpthread \
    -lncursesw \
    -flto \
    -Wl,--gc-sections \
    -Wl,--as-needed \
    -Wl,--strip-all \
    -Wl,-Ofast

OMNI_OBJ = $(OMNI_SRC:.c=.o)
OMNI_SRC = src/main.c src/omni_gatekeeper.c src/omni_logic.c src/tui.c src/stubs.c
SINGULAR_SRC = src/singular_broker.c src/singular_sender.c src/singular_receiver.c
BENCH_SRC = bench_nsr.c

OMNI_CORE_OBJ = src/omni_gatekeeper.o src/omni_logic.o src/stubs.o src/tui.o

TARGET = nsr_omni_bin
BENCH_TARGET = nsr_bench
BENCH_E2E_TARGET = nsr_bench_e2e

all: $(TARGET) $(BENCH_TARGET) $(BENCH_E2E_TARGET)

$(TARGET): $(OMNI_OBJ)
	$(CC) $(OMNI_OBJ) -o $@ $(LDFLAGS)

$(BENCH_TARGET): bench_nsr.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

$(BENCH_E2E_TARGET): bench_nsr_e2e.c $(OMNI_CORE_OBJ)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
install:
	cp nsr_omni_bin /usr/local/bin/nsr

clean:
	rm -f src/*.o *.o $(TARGET) $(BENCH_TARGET)

.PHONY: all clean
