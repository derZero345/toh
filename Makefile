# Pathfinder - C / Vulkan port
#
# Dependencies: SDL3 (>= 3.4, for SDL_LoadPNG), Vulkan loader + headers,
#               FreeType 2, fontconfig, glslc (shaderc) for shader compilation.

CC      ?= cc
PKGS    := sdl3 vulkan freetype2 fontconfig
# CFLAGS / LDFLAGS may be overridden, e.g. make CFLAGS="-O1 -g -fsanitize=address"
CFLAGS  ?= -O2 -g
LDFLAGS ?=
PF_CFLAGS := -std=c11 -Wall -Wextra -Wno-unused-parameter -D_POSIX_C_SOURCE=200809L \
             $(shell pkg-config --cflags $(PKGS)) -Ibuild
PF_LIBS   := $(shell pkg-config --libs $(PKGS)) -lm
GLSLC   ?= glslc

SRC     := $(wildcard src/*.c)
OBJ     := $(patsubst src/%.c,build/%.o,$(SRC))
SHADERS := build/ui.vert.inc build/ui.frag.inc
BIN     := pathfinder

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(PF_LIBS)

build/%.o: src/%.c $(wildcard src/*.h) | build
	$(CC) $(PF_CFLAGS) $(CFLAGS) -c -o $@ $<

build/vk_renderer.o: $(SHADERS)

build/%.inc: shaders/% | build
	$(GLSLC) -O -mfmt=c -o $@ $<

build:
	mkdir -p build

clean:
	rm -rf build $(BIN)

run: $(BIN)
	./$(BIN)

.PHONY: all clean run
