# Tales of Honor (ToH) - C / Vulkan port
#
# Dependencies: SDL3 (>= 3.4, for SDL_LoadPNG), Vulkan loader + headers,
#               FreeType 2, fontconfig, glslc (shaderc) for shader compilation,
#               libwebp, libzstd and libsodium for save files and multiplayer.
# Optional:     miniupnpc and libnatpmp open the host's port on the router (:host).
#               Without them the game builds and hosts, but players outside the LAN / VPN
#               need a manually forwarded port.

CC      ?= cc
SAVE_PKGS := libwebp libzstd libsodium
PKGS    := sdl3 vulkan freetype2 fontconfig $(SAVE_PKGS)
# CFLAGS / LDFLAGS may be overridden, e.g. make CFLAGS="-O1 -g -fsanitize=address"
CFLAGS  ?= -O2 -g
LDFLAGS ?=
TOH_CFLAGS := -std=c11 -Wall -Wextra -Wno-unused-parameter -D_POSIX_C_SOURCE=200809L \
             $(shell pkg-config --cflags $(PKGS)) -Ibuild
TOH_LIBS   := $(shell pkg-config --libs $(PKGS)) -lm -pthread
# Relay the game hosts through ("host[:port]"); empty = :host lets players connect directly.
# TOH_RELAY=host:port overrides it at run time. Run `make clean` after changing it.
RELAY ?= ho7oazp8e4tw4jra.myfritz.net:1520
ifneq ($(RELAY),)
TOH_CFLAGS += -DTOH_RELAY_DEFAULT='"$(RELAY)"'
endif

# Port mapping libraries: used when a test program against them links.
HAVE_PORTMAP := $(shell printf '\043include <miniupnpc/miniupnpc.h>\n\043include <natpmp.h>\nint main(void){return 0;}\n' | \
                  $(CC) -x c - -o /dev/null $$(pkg-config --cflags --libs miniupnpc 2>/dev/null || echo -lminiupnpc) -lnatpmp 2>/dev/null && echo 1)
ifeq ($(HAVE_PORTMAP),1)
TOH_CFLAGS += -DTOH_HAVE_PORTMAP $(shell pkg-config --cflags miniupnpc 2>/dev/null)
TOH_LIBS   += $(shell pkg-config --libs miniupnpc 2>/dev/null || echo -lminiupnpc) -lnatpmp
endif
GLSLC   ?= glslc

SRC     := $(wildcard src/*.c)
OBJ     := $(patsubst src/%.c,build/%.o,$(SRC))
SHADERS := build/ui.vert.inc build/ui.frag.inc
BIN     := toh

all: $(BIN)
ifneq ($(HAVE_PORTMAP),1)
	@echo "note: miniupnpc / libnatpmp not found - built without router port mapping"
endif

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
CORE_SRC    := src/ops.c src/entity.c src/game.c src/hex.c src/astar.c src/util.c src/savefmt.c src/image.c src/baranim.c src/party.c src/textwrap.c \
               src/hud_theme.c src/hud_icons.c src/hud_widgets.c src/hud_config.c src/hud.c src/hud_game.c \
               src/radial.c src/radial_widgets.c src/radial_config.c src/radial_game.c src/settings.c \
               src/moves.c src/move_ui.c src/recover.c src/attacks.c src/blocks.c src/dodges.c src/phases.c src/target_ui.c src/dice.c src/attrs.c src/charcreate.c src/hud_create.c
CORE_LIBS   := $(shell pkg-config --libs sdl3 $(SAVE_PKGS)) -lm

build/save_test: tests/save_test.c src/savefmt.c src/savefmt.h src/wire.h | build
	$(CC) $(TEST_CFLAGS) $(CFLAGS) -o $@ tests/save_test.c src/savefmt.c $(TEST_LIBS)

NETMSG_SRC  := src/netmsg.c src/savefmt.c
NETMSG_HDR  := src/netmsg.h src/savefmt.h src/wire.h
RELAY_SRC   := relay/relay.c src/relaymsg.c
RELAY_HDR   := relay/relay.h src/relaymsg.h

build/netmsg_test: tests/netmsg_test.c $(NETMSG_SRC) $(NETMSG_HDR) | build
	$(CC) $(TEST_CFLAGS) $(CFLAGS) -o $@ tests/netmsg_test.c $(NETMSG_SRC) $(TEST_LIBS)

build/net_test: tests/net_test.c src/net.c src/net.h $(NETMSG_SRC) $(NETMSG_HDR) $(RELAY_SRC) $(RELAY_HDR) | build
	$(CC) $(TEST_CFLAGS) $(CFLAGS) -Irelay -pthread -o $@ tests/net_test.c src/net.c $(NETMSG_SRC) $(RELAY_SRC) $(TEST_LIBS)

build/core_test: tests/core_test.c tests/core_stubs.c $(CORE_SRC) $(wildcard src/*.h) | build
	$(CC) $(TOH_CFLAGS) $(CFLAGS) -Isrc -o $@ tests/core_test.c tests/core_stubs.c $(CORE_SRC) $(CORE_LIBS)

build/relay_test: tests/relay_test.c $(RELAY_SRC) $(RELAY_HDR) | build
	$(CC) $(TEST_CFLAGS) $(CFLAGS) -Irelay -pthread -o $@ tests/relay_test.c $(RELAY_SRC)

# The relay for the home server: one static binary without dependencies.
build/toh-relay: relay/main.c $(RELAY_SRC) $(RELAY_HDR) | build
	$(CC) -std=c11 -Wall -Wextra -D_POSIX_C_SOURCE=200809L -Isrc -Irelay $(CFLAGS) -static -pthread \
		-o $@ relay/main.c $(RELAY_SRC)

relay: build/toh-relay

test: build/save_test build/core_test build/netmsg_test build/net_test build/relay_test
	./build/save_test
	./build/core_test
	./build/netmsg_test
	./build/net_test
	./build/relay_test

build/fuzz_savefmt: tests/fuzz_savefmt.c src/savefmt.c src/savefmt.h | build
	$(FUZZ_CC) $(TEST_CFLAGS) -DSAVEFMT_FUZZING -g -O1 -fsanitize=fuzzer,address,undefined \
		-o $@ tests/fuzz_savefmt.c src/savefmt.c $(TEST_LIBS)

build/fuzz_netmsg: tests/fuzz_netmsg.c $(NETMSG_SRC) $(NETMSG_HDR) src/relaymsg.c src/relaymsg.h | build
	$(FUZZ_CC) $(TEST_CFLAGS) -DSAVEFMT_FUZZING -g -O1 -fsanitize=fuzzer,address,undefined \
		-o $@ tests/fuzz_netmsg.c $(NETMSG_SRC) src/relaymsg.c $(TEST_LIBS)

# FUZZ_TIME seconds of fuzzing per target, seeded with samples written by the unit tests
FUZZ_TIME ?= 60
fuzz: build/fuzz_savefmt build/fuzz_netmsg build/save_test build/netmsg_test
	mkdir -p build/fuzz-corpus build/fuzz-corpus-net
	./build/save_test build/fuzz-corpus/sample.tuhh
	./build/netmsg_test build/fuzz-corpus-net
	./build/fuzz_savefmt -max_total_time=$(FUZZ_TIME) build/fuzz-corpus
	./build/fuzz_netmsg -max_total_time=$(FUZZ_TIME) build/fuzz-corpus-net

# end-to-end checks of :save / :load, the HUD, the radial menu, the settings file, move actions, character creation and a
# session (host + client on localhost);
# they need a display and ImageMagick
e2e: $(BIN) build/toh-relay
	./tests/e2e_save.sh
	./tests/e2e_hud.sh
	./tests/e2e_radial.sh
	./tests/e2e_settings.sh
	./tests/e2e_moves.sh
	./tests/e2e_attack.sh
	./tests/e2e_create.sh
	./tests/e2e_net.sh
	TOH_E2E_RELAY=1 ./tests/e2e_net.sh

.PHONY: all clean run test fuzz e2e relay
