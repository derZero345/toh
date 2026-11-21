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

build/save_test: tests/save_test.c src/savefmt.c src/savefmt.h | build
	$(CC) $(TEST_CFLAGS) $(CFLAGS) -o $@ tests/save_test.c src/savefmt.c $(TEST_LIBS)

test: build/save_test
	./build/save_test

build/fuzz_savefmt: tests/fuzz_savefmt.c src/savefmt.c src/savefmt.h | build
	$(FUZZ_CC) $(TEST_CFLAGS) -DSAVEFMT_FUZZING -g -O1 -fsanitize=fuzzer,address,undefined \
		-o $@ tests/fuzz_savefmt.c src/savefmt.c $(TEST_LIBS)

# FUZZ_TIME seconds of fuzzing, seeded with a sample save written by save_test
FUZZ_TIME ?= 60
fuzz: build/fuzz_savefmt build/save_test
	mkdir -p build/fuzz-corpus
	./build/save_test build/fuzz-corpus/sample.tuhh
	./build/fuzz_savefmt -max_total_time=$(FUZZ_TIME) build/fuzz-corpus

# end-to-end :save / :load check; needs a display and ImageMagick
e2e: $(BIN)
	./tests/e2e_save.sh

.PHONY: all clean run test fuzz e2e
