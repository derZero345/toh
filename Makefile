# Tales of Honor (ToH) - C / Vulkan port
#
# Dependencies: SDL3 (>= 3.4, for SDL_LoadPNG), Vulkan loader + headers,
#               FreeType 2, fontconfig, glslc (shaderc) for shader compilation,
#               libwebp, libzstd and libsodium for save files.

CC      ?= cc
SAVE_PKGS := libwebp libzstd libsodium
PKGS    := sdl3 vulkan freetype2 fontconfig $(SAVE_PKGS)
# CFLAGS / LDFLAGS may be overridden, e.g. make CFLAGS="-O1 -g -fsanitize=address"
CFLAGS  ?= -O2 -g
LDFLAGS ?=
TOH_CFLAGS := -std=c11 -Wall -Wextra -Wno-unused-parameter -D_POSIX_C_SOURCE=200809L \
             $(shell pkg-config --cflags $(PKGS)) -Ibuild
TOH_LIBS   := $(shell pkg-config --libs $(PKGS)) -lm
GLSLC   ?= glslc

SRC     := $(wildcard src/*.c)
OBJ     := $(patsubst src/%.c,build/%.o,$(SRC))
SHADERS := build/ui.vert.inc build/ui.frag.inc
BIN     := toh

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(TOH_LIBS)

build/%.o: src/%.c $(wildcard src/*.h) | build
	$(CC) $(TOH_CFLAGS) $(CFLAGS) -c -o $@ $<

build/vk_renderer.o: $(SHADERS)

build/%.inc: shaders/% | build
	$(GLSLC) -O -mfmt=c -o $@ $<

build:
	mkdir -p build

clean:
	rm -rf build $(BIN)

run: $(BIN)
	./$(BIN)

# ---- tests --------------------------------------------------------------------------
# The save format tests and the fuzzer only need src/savefmt.c (no SDL / Vulkan).
TEST_CFLAGS := -std=c11 -Wall -Wextra -D_POSIX_C_SOURCE=200809L -Isrc $(shell pkg-config --cflags $(SAVE_PKGS))
TEST_LIBS   := $(shell pkg-config --libs $(SAVE_PKGS)) -lm
FUZZ_CC     ?= clang

# Game core tests: entity / game / grid / pathfinding / op logic without a window.
# tests/core_stubs.c replaces the renderer, console and graphics functions they call.
CORE_SRC    := src/ops.c src/entity.c src/game.c src/hex.c src/astar.c src/util.c src/savefmt.c
CORE_LIBS   := $(shell pkg-config --libs sdl3 $(SAVE_PKGS)) -lm

build/save_test: tests/save_test.c src/savefmt.c src/savefmt.h src/wire.h | build
	$(CC) $(TEST_CFLAGS) $(CFLAGS) -o $@ tests/save_test.c src/savefmt.c $(TEST_LIBS)

NETMSG_SRC  := src/netmsg.c src/savefmt.c
NETMSG_HDR  := src/netmsg.h src/savefmt.h src/wire.h

build/netmsg_test: tests/netmsg_test.c $(NETMSG_SRC) $(NETMSG_HDR) | build
	$(CC) $(TEST_CFLAGS) $(CFLAGS) -o $@ tests/netmsg_test.c $(NETMSG_SRC) $(TEST_LIBS)

build/core_test: tests/core_test.c tests/core_stubs.c $(CORE_SRC) $(wildcard src/*.h) | build
	$(CC) $(TOH_CFLAGS) $(CFLAGS) -Isrc -o $@ tests/core_test.c tests/core_stubs.c $(CORE_SRC) $(CORE_LIBS)

test: build/save_test build/core_test build/netmsg_test
	./build/save_test
	./build/core_test
	./build/netmsg_test

build/fuzz_savefmt: tests/fuzz_savefmt.c src/savefmt.c src/savefmt.h | build
	$(FUZZ_CC) $(TEST_CFLAGS) -DSAVEFMT_FUZZING -g -O1 -fsanitize=fuzzer,address,undefined \
		-o $@ tests/fuzz_savefmt.c src/savefmt.c $(TEST_LIBS)

build/fuzz_netmsg: tests/fuzz_netmsg.c $(NETMSG_SRC) $(NETMSG_HDR) | build
	$(FUZZ_CC) $(TEST_CFLAGS) -DSAVEFMT_FUZZING -g -O1 -fsanitize=fuzzer,address,undefined \
		-o $@ tests/fuzz_netmsg.c $(NETMSG_SRC) $(TEST_LIBS)

# FUZZ_TIME seconds of fuzzing per target, seeded with samples written by the unit tests
FUZZ_TIME ?= 60
fuzz: build/fuzz_savefmt build/fuzz_netmsg build/save_test build/netmsg_test
	mkdir -p build/fuzz-corpus build/fuzz-corpus-net
	./build/save_test build/fuzz-corpus/sample.tuhh
	./build/netmsg_test build/fuzz-corpus-net
	./build/fuzz_savefmt -max_total_time=$(FUZZ_TIME) build/fuzz-corpus
	./build/fuzz_netmsg -max_total_time=$(FUZZ_TIME) build/fuzz-corpus-net

# end-to-end :save / :load check; needs a display and ImageMagick
e2e: $(BIN)
	./tests/e2e_save.sh

.PHONY: all clean run test fuzz e2e
