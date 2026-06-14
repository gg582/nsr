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
    -D_XOPEN_SOURCE_EXTENDED \
	-D_GNU_SOURCE
LDFLAGS = \
    -L./lib/libttak/lib \
    -L/usr/local/cuda/lib64 \
    -lttak \
    -lcudart \
    -lstdc++ \
    -lpthread \
    -lncursesw \
    -flto \
    -Wl,--gc-sections \
    -Wl,--as-needed \
    -Wl,--strip-all \
    -Wl,-Ofast \
    -Wl,-rpath,/usr/local/cuda/lib64

CORE_SRC = src/main.c src/gatekeeper.c src/logic.c src/tui.c src/topology.c src/stubs.c
CORE_OBJ = $(CORE_SRC:.c=.o)

BENCH_SRC = bench_nsr.c

TARGET = nsr
BENCH_TARGET = nsr_bench
BENCH_E2E_TARGET = nsr_bench_e2e

all: $(TARGET) $(BENCH_TARGET) $(BENCH_E2E_TARGET)

$(TARGET): $(CORE_OBJ)
	$(CC) $(CORE_OBJ) -o $@ $(LDFLAGS)

$(BENCH_TARGET): bench_nsr.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

$(BENCH_E2E_TARGET): bench_nsr_e2e.c src/gatekeeper.o src/logic.o src/stubs.o src/tui.o src/topology.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

install:
	cp $(TARGET) /usr/local/bin/nsr

clean:
	rm -f src/*.o *.o $(TARGET) $(BENCH_TARGET) $(BENCH_E2E_TARGET)

.PHONY: all clean
