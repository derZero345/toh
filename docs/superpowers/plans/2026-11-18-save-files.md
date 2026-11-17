# Save Files (`.pfsave`) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Save and load the whole table to compact, tamper-evident binary `.pfsave` files, using `:save` and `:load` and an nvim-style unsaved-changes guard on `:q` and `:load`.

**Architecture:** `src/savefmt.c` is a self-contained codec. It turns a plain `SaveData` struct into a file and back, and does all validation. It doesn't use SDL, Vulkan or `App`, so a future server can reuse it. The pieces:

- The state is packed with varints and compressed with zstd.
- Each image is stored once as WebP.
- A keyed BLAKE2b tag over the file is checked before anything is parsed.
- `src/save.c` connects the codec to the game: it gathers `SaveData` from `gh` and applies a decoded one, and implements the commands, dialogs, atomic writes and the content-fingerprint guard.
- Textures keep a CPU copy of user images (pixels, or cached WebP data) so that they can be saved.

**Tech Stack:** C11, SDL3 3.4 (dialogs), Vulkan (existing renderer), libzstd, libwebp, libsodium, libFuzzer (clang), ImageMagick (e2e test).

**Spec:** `docs/superpowers/specs/2026-11-18-save-files-design.md`

**Notes for the implementer:**
- Every code block in this plan was compiled and run on a scratch copy of the project. That run gave: `make test`
  71/71 checks pass (also under ASan/UBSan with `-Werror`); `make fuzz` ran 758k inputs with no crash; `make e2e`
  passed (also with an ASan/UBSan build of the game).
- The project is not a git repository yet. Run `git init && git add -A && git commit -m "Initial import"` once
  before Task 1 if you want the commit steps; otherwise skip them.
- Build commands are run from the project root. `make` builds `./pathfinder`; `src/*.c` is picked up by a wildcard,
  so new source files need no Makefile entry.
- Patches are given as unified diffs against the current sources. Apply each with `patch -p0` as shown, or make
  the same edits by hand.

## File structure

| File | Status | Responsibility |
|---|---|---|
| `src/savefmt.h` | new | Format constants, limits, `SaveData` types, codec API |
| `src/savefmt.c` | new | Encode, decode, validation, fingerprint, tag. Pure C plus zstd, WebP and libsodium |
| `src/save.c` | new | Game ↔ `SaveData`, `:save` / `:load` / `:q` logic, atomic write, dialogs, fingerprint state |
| `tests/save_test.c` | new | Unit tests for `savefmt.c` (`make test`) |
| `tests/fuzz_savefmt.c` | new | libFuzzer target (`make fuzz`) |
| `tests/e2e_save.sh` | new | End-to-end check in a real window (`make e2e`) |
| `Makefile` | modify | New libraries; `test`, `fuzz` and `e2e` targets |
| `src/vk_renderer.[ch]` | modify | Texture keeps pixels / encoded blob, unique id |
| `src/game.c` | modify | `game_restore_initiative` |
| `src/app.h` | modify | `ACTION_SAVE` / `ACTION_LOAD`, declarations for save.c and game.c |
| `src/main.c` | modify | `save_init()` after `gfx_init`; comment |
| `src/graphics.c` | modify | User images keep their pixels; save/load dialogs |
| `src/console.c` | modify | `:save`, `:load[!]`, `:q!`, help, completion |
| `README.md` | modify | Dependencies, tests, "Saving" section |

---

### Task 1: Build setup

**Files:**
- Modify: `Makefile`

- [ ] **Step 1: Check that the libraries are installed**

Run: `pkg-config --modversion libwebp libzstd libsodium && which clang magick`
Expected: three version numbers (e.g. `1.6.0`, `1.5.7`, `1.0.22`) and two paths. If any are missing, install them
(Arch: `pacman -S libwebp zstd libsodium clang imagemagick`).

- [ ] **Step 2: Patch `Makefile`**

Save the diff below as `/tmp/Makefile.patch` and apply it from the project root with
`patch -p0 < /tmp/Makefile.patch` (or make the same edits by hand):

```diff
--- Makefile
+++ Makefile
@@ -1,10 +1,12 @@
 # Pathfinder - C / Vulkan port
 #
 # Dependencies: SDL3 (>= 3.4, for SDL_LoadPNG), Vulkan loader + headers,
-#               FreeType 2, fontconfig, glslc (shaderc) for shader compilation.
+#               FreeType 2, fontconfig, glslc (shaderc) for shader compilation,
+#               libwebp, libzstd and libsodium for save files.
 
 CC      ?= cc
-PKGS    := sdl3 vulkan freetype2 fontconfig
+SAVE_PKGS := libwebp libzstd libsodium
+PKGS    := sdl3 vulkan freetype2 fontconfig $(SAVE_PKGS)
 # CFLAGS / LDFLAGS may be overridden, e.g. make CFLAGS="-O1 -g -fsanitize=address"
 CFLAGS  ?= -O2 -g
 LDFLAGS ?=
@@ -40,4 +42,31 @@
 run: $(BIN)
 	./$(BIN)
 
-.PHONY: all clean run
+# ---- tests --------------------------------------------------------------------------
+# The save format tests and the fuzzer only need src/savefmt.c (no SDL / Vulkan).
+TEST_CFLAGS := -std=c11 -Wall -Wextra -D_POSIX_C_SOURCE=200809L -Isrc $(shell pkg-config --cflags $(SAVE_PKGS))
+TEST_LIBS   := $(shell pkg-config --libs $(SAVE_PKGS)) -lm
+FUZZ_CC     ?= clang
+
+build/save_test: tests/save_test.c src/savefmt.c src/savefmt.h | build
+	$(CC) $(TEST_CFLAGS) $(CFLAGS) -o $@ tests/save_test.c src/savefmt.c $(TEST_LIBS)
+
+test: build/save_test
+	./build/save_test
+
+build/fuzz_savefmt: tests/fuzz_savefmt.c src/savefmt.c src/savefmt.h | build
+	$(FUZZ_CC) $(TEST_CFLAGS) -DSAVEFMT_FUZZING -g -O1 -fsanitize=fuzzer,address,undefined \
+		-o $@ tests/fuzz_savefmt.c src/savefmt.c $(TEST_LIBS)
+
+# FUZZ_TIME seconds of fuzzing, seeded with a sample save written by save_test
+FUZZ_TIME ?= 60
+fuzz: build/fuzz_savefmt build/save_test
+	mkdir -p build/fuzz-corpus
+	./build/save_test build/fuzz-corpus/sample.pfsave
+	./build/fuzz_savefmt -max_total_time=$(FUZZ_TIME) build/fuzz-corpus
+
+# end-to-end :save / :load check; needs a display and ImageMagick
+e2e: $(BIN)
+	./tests/e2e_save.sh
+
+.PHONY: all clean run test fuzz e2e
```

- [ ] **Step 3: Check that the game still builds**

Run: `make 2>&1 | grep -E "warning|error"; ls -l pathfinder`
Expected: no output from grep, and a fresh `pathfinder` binary.

- [ ] **Step 4: Commit**

```bash
git add Makefile
git commit -m "build: add libwebp, libzstd, libsodium and test targets"
```

---

### Task 2: Save format API (`savefmt.h`)

**Files:**
- Create: `src/savefmt.h`

- [ ] **Step 1: Create `src/savefmt.h`**

```c
#ifndef PF_SAVEFMT_H
#define PF_SAVEFMT_H

/*
 * .pfsave file format, version 1: encoding, decoding and validation.
 *
 * Plain C11 on top of libzstd, libwebp and libsodium. It knows nothing about SDL,
 * Vulkan or the App struct, so tools or a server can reuse it to validate files.
 * Design: docs/superpowers/specs/2026-11-18-save-files-design.md
 *
 * Layout (little endian):
 *   "PFSV" | u16 version | u16 flags | u32 state_len | zstd(state) |
 *   u32 image_count | { u8 kind, u16 w, u16 h, u32 len, WebP }... | 32-byte keyed BLAKE2b tag
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SAVEFMT_VERSION     1
#define SAVEFMT_TAG_SIZE    32
#define SAVEFMT_HEADER_SIZE 12 /* magic, version, flags, state_len */

/* Limits for untrusted input. The encoder enforces the same rules. */
#define SAVEFMT_MAX_FILE        (256u << 20)
#define SAVEFMT_MAX_STATE       (16u << 20)
#define SAVEFMT_MAX_IMAGES      4096u
#define SAVEFMT_MAX_DIM         16383 /* WebP's limit */
#define SAVEFMT_MAX_PIXEL_BYTES (1ull << 30) /* decoded RGBA of all images together */
#define SAVEFMT_MAX_ENTITIES    100000u
#define SAVEFMT_MAX_WAYPOINTS   10000u
#define SAVEFMT_MAX_STR         255u
#define SAVEFMT_NAME_MAX        32 /* waypoint names are shorter than this (= WAYPOINT_NAME_MAX) */
#define SAVEFMT_MAX_SPEED       1000 /* = MAX_SPEED */
#define SAVEFMT_MAX_COORD       1000000
#define SAVEFMT_MAX_OFFSET      1000000000
#define SAVEFMT_MIN_HEX         5
#define SAVEFMT_MAX_HEX         400
#define SAVEFMT_MAX_GRID        10000

enum { SAVE_IMG_LOSSLESS = 0, SAVE_IMG_LOSSY = 1 };
enum { SAVE_ENT_CHARACTER = 0, SAVE_ENT_WALL = 1 };

typedef enum {
    SAVE_OK = 0,
    SAVE_ERR_NOT_SAVE,  /* wrong magic */
    SAVE_ERR_NEWER,     /* version from the future */
    SAVE_ERR_CORRUPT,   /* bad tag, structure or values */
    SAVE_ERR_TOO_LARGE, /* over one of the size limits */
    SAVE_ERR_INVALID,   /* encoder: the data breaks a rule and would not load again */
    SAVE_ERR_ENCODE,    /* encoder: WebP or zstd failed */
    SAVE_ERR_INTERNAL,  /* out of memory, libsodium init failed */
} SaveError;

typedef struct {
    uint8_t kind;        /* SAVE_IMG_* */
    int w, h;            /* size of rgba; when webp is set: size of the WebP image */
    const uint8_t *rgba; /* w*h*4 bytes; may be NULL when webp is set */
    const uint8_t *webp; /* already encoded data, used as is when set */
    size_t webp_len;
    uint64_t id;         /* identity for savefmt_fingerprint only; never written to files */
} SaveImage;

typedef struct {
    uint8_t type;   /* SAVE_ENT_* */
    uint32_t image; /* index into SaveData.images */
    int32_t row, col;
    /* characters only (not stored for walls) */
    int32_t size, max_health, health, attack, ac, speed, initiative;
} SaveEntity;

typedef struct {
    uint32_t entity; /* index into SaveData.entities */
    double value;
} SaveInit;

typedef struct {
    char name[SAVEFMT_NAME_MAX];
    int32_t row, col;
} SaveWaypoint;

typedef struct {
    bool is_flat, has_background, init_markers_visible, turn_started;
    uint32_t background; /* image index, when has_background */
    uint32_t bg_rows, bg_cols;
    int32_t offset_x, offset_y;
    uint32_t hex_size;
    char theme[SAVEFMT_MAX_STR + 1];

    SaveImage *images;
    uint32_t nimages;
    SaveEntity *entities;
    uint32_t nentities;
    SaveInit *init;
    uint32_t ninit;
    uint32_t current_turn;
    SaveWaypoint *waypoints;
    uint32_t nwaypoints;

    /* Set by savefmt_decode: savefmt_free releases the arrays and image buffers.
     * Data built by the caller leaves it false and manages its own memory. */
    bool owns_memory;
} SaveData;

/* A WebP image produced while encoding. Free webp with free(). */
typedef struct {
    uint8_t *webp;
    size_t len;
    int w, h; /* can be smaller than the source: images over SAVEFMT_MAX_DIM are scaled down */
} SaveBlob;

/* Encodes d into a new buffer (*out, free with free()).
 * blobs_out (optional, d->nimages entries): receives the WebP data of every image that
 * had to be encoded (webp == NULL on input); other entries are zeroed. Ownership passes
 * to the caller, also on failure (use savefmt_blobs_free). */
SaveError savefmt_encode(const SaveData *d, SaveBlob *blobs_out, uint8_t **out, size_t *out_len);

/* Verifies and decodes a whole file. Images come back with both rgba and webp set.
 * On failure *out is left empty (savefmt_free is still safe to call). */
SaveError savefmt_decode(const uint8_t *buf, size_t len, SaveData *out);

void savefmt_free(SaveData *d);
void savefmt_blobs_free(SaveBlob *blobs, uint32_t n);

/* Hash of the table content, ignoring camera and theme; images by their id.
 * Returns false when out of memory. */
bool savefmt_fingerprint(const SaveData *d, uint8_t out[SAVEFMT_TAG_SIZE]);

/* Integrity tag of buf[0..len). Exposed for tests. */
void savefmt_compute_tag(const uint8_t *buf, size_t len, uint8_t out[SAVEFMT_TAG_SIZE]);

const char *savefmt_strerror(SaveError e);

#endif
```

- [ ] **Step 2: Check that the header compiles on its own**

Run: `echo '#include "savefmt.h"' | cc -std=c11 -Wall -Wextra -Isrc -fsyntax-only -x c -`
Expected: no output.

- [ ] **Step 3: Commit**

```bash
git add src/savefmt.h
git commit -m "savefmt: add save file format API"
```

---

### Task 3: Unit tests for the format (written first)

**Files:**
- Create: `tests/save_test.c`

What the tests cover:

- **Round trip**, including an empty table.
- **Image handling:** WebP data is handed back and re-saving reproduces the file exactly; oversized images are
  scaled down.
- **Tamper detection:** every byte flip and every truncation is rejected; header errors (magic, version, flags)
  give the right messages.
- **A strict parser:** hand-built files carry a *valid* tag but break a rule, and must still be rejected.
- **Encoder:** it validates its input.
- **Fingerprint:** it ignores camera and theme, and notices moved entities and changed images.

- [ ] **Step 1: Create `tests/save_test.c`**

```c
/* Unit tests for src/savefmt.c. Run with `make test`.
 * `build/save_test <file>` additionally writes a sample save (used as fuzzing seed). */
#include "savefmt.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <webp/encode.h>
#include <zstd.h>

static int failures, checks;

#define CHECK(cond) do {                                                       \
        checks++;                                                              \
        if (!(cond)) {                                                         \
            failures++;                                                        \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                      \
    } while (0)

#define CHECK_ERR(expr, want) do {                                             \
        SaveError got_ = (expr);                                               \
        checks++;                                                              \
        if (got_ != (want)) {                                                  \
            failures++;                                                        \
            fprintf(stderr, "%s:%d: %s = \"%s\", expected \"%s\"\n", __FILE__, __LINE__, #expr, \
                    savefmt_strerror(got_), savefmt_strerror(want));           \
        }                                                                      \
    } while (0)

/* ---- sample data ---------------------------------------------------------------------- */

/* Deterministic RGBA pattern with varied alpha, including transparent pixels with colour. */
static uint8_t *pattern(int w, int h, int seed) {
    uint8_t *p = malloc((size_t)w * h * 4);
    for (int i = 0; i < w * h; i++) {
        p[i * 4 + 0] = (uint8_t)(i * 7 + seed);
        p[i * 4 + 1] = (uint8_t)(i * 13 + seed * 3);
        p[i * 4 + 2] = (uint8_t)(i * 29);
        p[i * 4 + 3] = (uint8_t)((i % 5) * 60);
    }
    return p;
}

typedef struct {
    SaveData d;
    SaveImage images[2];
    SaveEntity entities[3];
    SaveInit init[2];
    SaveWaypoint waypoints[2];
    uint8_t *icon, *bg;
} Sample;

/* A table with a background, two characters sharing an icon, a wall, initiative and waypoints. */
static void sample_init(Sample *s) {
    memset(s, 0, sizeof(*s));
    s->icon = pattern(8, 8, 1);
    s->bg = pattern(64, 48, 2);
    s->images[0] = (SaveImage){ .kind = SAVE_IMG_LOSSY, .w = 64, .h = 48, .rgba = s->bg, .id = 101 };
    s->images[1] = (SaveImage){ .kind = SAVE_IMG_LOSSLESS, .w = 8, .h = 8, .rgba = s->icon, .id = 102 };
    s->entities[0] = (SaveEntity){ SAVE_ENT_CHARACTER, 1, 3, -4, 1, 30, 25, -2, 15, 6, 12 };
    s->entities[1] = (SaveEntity){ SAVE_ENT_CHARACTER, 1, -100, 250, -2, 7, 7, 0, 10, 1000, -3 };
    s->entities[2] = (SaveEntity){ .type = SAVE_ENT_WALL, .image = 1, .row = 5, .col = 5 };
    s->init[0] = (SaveInit){ 1, 0.875 };
    s->init[1] = (SaveInit){ 0, 0.125 };
    snprintf(s->waypoints[0].name, SAVEFMT_NAME_MAX, "camp");
    s->waypoints[0].row = 2;
    s->waypoints[0].col = 3;
    snprintf(s->waypoints[1].name, SAVEFMT_NAME_MAX, "Höhle");
    s->waypoints[1].row = -7;
    s->waypoints[1].col = 0;

    SaveData *d = &s->d;
    d->is_flat = true;
    d->has_background = true;
    d->background = 0;
    d->init_markers_visible = true;
    d->turn_started = true;
    d->bg_rows = 20;
    d->bg_cols = 20;
    d->offset_x = -120;
    d->offset_y = 45;
    d->hex_size = 40;
    snprintf(d->theme, sizeof(d->theme), "dark");
    d->images = s->images;
    d->nimages = 2;
    d->entities = s->entities;
    d->nentities = 3;
    d->init = s->init;
    d->ninit = 2;
    d->current_turn = 1;
    d->waypoints = s->waypoints;
    d->nwaypoints = 2;
}

static void sample_free(Sample *s) {
    free(s->icon);
    free(s->bg);
}

static uint8_t *encode_sample(Sample *s, size_t *len) {
    uint8_t *file = NULL;
    CHECK_ERR(savefmt_encode(&s->d, NULL, &file, len), SAVE_OK);
    return file;
}

/* ---- tests ----------------------------------------------------------------------------- */

static void test_tag(void) {
    uint8_t a[SAVEFMT_TAG_SIZE], b[SAVEFMT_TAG_SIZE];
    uint8_t data[16] = "pathfinder";
    savefmt_compute_tag(data, sizeof(data), a);
    savefmt_compute_tag(data, sizeof(data), b);
    CHECK(memcmp(a, b, sizeof(a)) == 0);
    data[3] ^= 1;
    savefmt_compute_tag(data, sizeof(data), b);
    CHECK(memcmp(a, b, sizeof(a)) != 0);
}

static void test_roundtrip(void) {
    Sample s;
    sample_init(&s);
    size_t len;
    uint8_t *file = encode_sample(&s, &len);
    CHECK(file && len > 4 && memcmp(file, "PFSV", 4) == 0);

    SaveData d;
    CHECK_ERR(savefmt_decode(file, len, &d), SAVE_OK);
    const SaveData *o = &s.d;
    CHECK(d.is_flat == o->is_flat && d.has_background && d.background == 0);
    CHECK(d.init_markers_visible && d.turn_started);
    CHECK(d.bg_rows == 20 && d.bg_cols == 20 && d.offset_x == -120 && d.offset_y == 45 && d.hex_size == 40);
    CHECK(strcmp(d.theme, "dark") == 0);
    CHECK(d.nentities == 3 && memcmp(d.entities, o->entities, sizeof(s.entities)) == 0);
    CHECK(d.ninit == 2 && d.init[0].entity == 1 && d.init[0].value == 0.875 && d.init[1].value == 0.125);
    CHECK(d.current_turn == 1);
    CHECK(d.nwaypoints == 2 && strcmp(d.waypoints[1].name, "Höhle") == 0 && d.waypoints[1].row == -7);
    CHECK(d.nimages == 2);
    /* lossless icon: pixel exact, lossy background: right size */
    CHECK(d.images[1].kind == SAVE_IMG_LOSSLESS && d.images[1].w == 8 && d.images[1].h == 8);
    CHECK(memcmp(d.images[1].rgba, s.icon, 8 * 8 * 4) == 0);
    CHECK(d.images[0].kind == SAVE_IMG_LOSSY && d.images[0].w == 64 && d.images[0].h == 48);
    CHECK(d.images[0].rgba && d.images[0].webp && d.images[0].webp_len > 0);
    savefmt_free(&d);
    free(file);
    sample_free(&s);
}

static void test_empty_table(void) {
    SaveData d = { .bg_rows = 20, .bg_cols = 20, .hex_size = 40, .init_markers_visible = true };
    snprintf(d.theme, sizeof(d.theme), "light");
    uint8_t *file;
    size_t len;
    CHECK_ERR(savefmt_encode(&d, NULL, &file, &len), SAVE_OK);
    SaveData o;
    CHECK_ERR(savefmt_decode(file, len, &o), SAVE_OK);
    CHECK(o.nentities == 0 && o.nimages == 0 && !o.has_background);
    savefmt_free(&o);
    free(file);
}

/* Images encoded once are handed back; re-saving with them reproduces the file exactly. */
static void test_blobs(void) {
    Sample s;
    sample_init(&s);
    SaveBlob blobs[2];
    uint8_t *a, *b;
    size_t alen, blen;
    CHECK_ERR(savefmt_encode(&s.d, blobs, &a, &alen), SAVE_OK);
    CHECK(blobs[0].webp && blobs[0].w == 64 && blobs[0].h == 48);
    CHECK(blobs[1].webp && blobs[1].w == 8 && blobs[1].h == 8);
    for (int i = 0; i < 2; i++) {
        s.images[i].rgba = NULL;
        s.images[i].webp = blobs[i].webp;
        s.images[i].webp_len = blobs[i].len;
    }
    SaveBlob again[2];
    CHECK_ERR(savefmt_encode(&s.d, again, &b, &blen), SAVE_OK);
    CHECK(again[0].webp == NULL && again[1].webp == NULL);
    CHECK(alen == blen && memcmp(a, b, alen) == 0);
    savefmt_blobs_free(blobs, 2);
    free(a);
    free(b);
    sample_free(&s);
}

/* Images over WebP's 16383 px limit are scaled down. */
static void test_scale_down(void) {
    SaveData d = { .bg_rows = 20, .bg_cols = 20, .hex_size = 40, .has_background = true };
    snprintf(d.theme, sizeof(d.theme), "light");
    uint8_t *px = pattern(16400, 2, 3);
    SaveImage im = { .kind = SAVE_IMG_LOSSY, .w = 16400, .h = 2, .rgba = px };
    d.images = &im;
    d.nimages = 1;
    SaveBlob blob;
    uint8_t *file;
    size_t len;
    CHECK_ERR(savefmt_encode(&d, &blob, &file, &len), SAVE_OK);
    CHECK(blob.w == 16383 && blob.h == 1);
    SaveData o;
    CHECK_ERR(savefmt_decode(file, len, &o), SAVE_OK);
    CHECK(o.nimages == 1 && o.images[0].w == 16383 && o.images[0].h == 1);
    savefmt_free(&o);
    savefmt_blobs_free(&blob, 1);
    free(file);
    free(px);
}

static void test_every_byte_flip_rejected(void) {
    Sample s;
    sample_init(&s);
    size_t len;
    uint8_t *file = encode_sample(&s, &len);
    int accepted = 0, wrong_magic_error = 0;
    for (size_t i = 0; i < len; i++) {
        for (int bit = 0; bit < 8; bit += 3) {
            file[i] ^= (uint8_t)(1 << bit);
            SaveData d;
            SaveError e = savefmt_decode(file, len, &d);
            if (e == SAVE_OK) accepted++;
            if (i < 4 && e != SAVE_ERR_NOT_SAVE) wrong_magic_error++;
            savefmt_free(&d);
            file[i] ^= (uint8_t)(1 << bit);
        }
    }
    CHECK(accepted == 0);
    CHECK(wrong_magic_error == 0);
    free(file);
    sample_free(&s);
}

static void test_every_truncation_rejected(void) {
    Sample s;
    sample_init(&s);
    size_t len;
    uint8_t *file = encode_sample(&s, &len);
    int accepted = 0;
    for (size_t n = 0; n < len; n++) {
        SaveData d;
        if (savefmt_decode(file, n, &d) == SAVE_OK) accepted++;
        savefmt_free(&d);
    }
    CHECK(accepted == 0);
    /* appended bytes too */
    uint8_t *longer = malloc(len + 1);
    memcpy(longer, file, len);
    longer[len] = 0;
    SaveData d;
    CHECK_ERR(savefmt_decode(longer, len + 1, &d), SAVE_ERR_CORRUPT);
    free(longer);
    free(file);
    sample_free(&s);
}

static void retag(uint8_t *file, size_t len) {
    savefmt_compute_tag(file, len - SAVEFMT_TAG_SIZE, file + len - SAVEFMT_TAG_SIZE);
}

static void test_header_errors(void) {
    Sample s;
    sample_init(&s);
    size_t len;
    uint8_t *file = encode_sample(&s, &len);
    SaveData d;

    CHECK_ERR(savefmt_decode((const uint8_t *)"PNG\x89 not a save", 16, &d), SAVE_ERR_NOT_SAVE);
    CHECK_ERR(savefmt_decode(file, 2, &d), SAVE_ERR_NOT_SAVE);

    file[4] = 2; /* version 2 */
    retag(file, len);
    CHECK_ERR(savefmt_decode(file, len, &d), SAVE_ERR_NEWER);
    file[4] = 0; /* version 0 */
    retag(file, len);
    CHECK_ERR(savefmt_decode(file, len, &d), SAVE_ERR_CORRUPT);
    file[4] = 1;
    file[6] = 1; /* signature flag: reserved in v1 */
    retag(file, len);
    CHECK_ERR(savefmt_decode(file, len, &d), SAVE_ERR_CORRUPT);
    file[6] = 0;
    retag(file, len);
    CHECK_ERR(savefmt_decode(file, len, &d), SAVE_OK);
    savefmt_free(&d);
    free(file);
    sample_free(&s);
}

/* ---- hand-built files with a valid tag (the parser must not trust the tag) ----------------- */

typedef struct { uint8_t b[8192]; size_t n; } W;

static void w8(W *w, uint8_t v) { w->b[w->n++] = v; }
static void w32(W *w, uint32_t v) { for (int i = 0; i < 4; i++) w8(w, (uint8_t)(v >> (8 * i))); }
static void wuv(W *w, uint32_t v) {
    do {
        uint8_t c = v & 0x7f;
        v >>= 7;
        w8(w, (uint8_t)(c | (v ? 0x80 : 0)));
    } while (v);
}
static void wsv(W *w, int32_t v) { wuv(w, ((uint32_t)v << 1) ^ (v < 0 ? 0xFFFFFFFFu : 0)); }
static void wf64(W *w, double v) { uint64_t u; memcpy(&u, &v, 8); w32(w, (uint32_t)u); w32(w, (uint32_t)(u >> 32)); }
static void wbytes(W *w, const void *p, size_t n) { memcpy(w->b + w->n, p, n); w->n += n; }
static void wstr(W *w, const char *s) { wuv(w, (uint32_t)strlen(s)); wbytes(w, s, strlen(s)); }

/* world section of a valid table: pointy grid, no background */
static void wworld(W *w, uint8_t flags) {
    w8(w, flags);
    wuv(w, 20);
    wuv(w, 20);
    wsv(w, 0);
    wsv(w, 0);
    wuv(w, 40);
    wstr(w, "light");
}

static void wwall(W *w, uint32_t image) { w8(w, SAVE_ENT_WALL); wuv(w, image); wsv(w, 1); wsv(w, 2); }

/* Wraps a raw state section and raw image section (count + records) into a tagged file. */
static uint8_t *wrap(const W *state, const W *images, size_t *len, int content_size_flag) {
    ZSTD_CCtx *cc = ZSTD_createCCtx();
    ZSTD_CCtx_setParameter(cc, ZSTD_c_contentSizeFlag, content_size_flag);
    size_t bound = ZSTD_compressBound(state->n);
    uint8_t *z = malloc(bound);
    size_t zn = ZSTD_compress2(cc, z, bound, state->b, state->n);
    ZSTD_freeCCtx(cc);
    size_t total = SAVEFMT_HEADER_SIZE + zn + images->n + SAVEFMT_TAG_SIZE;
    uint8_t *f = malloc(total);
    memcpy(f, "PFSV\x01\x00\x00\x00", 8);
    for (int i = 0; i < 4; i++) f[8 + i] = (uint8_t)(zn >> (8 * i));
    memcpy(f + 12, z, zn);
    memcpy(f + 12 + zn, images->b, images->n);
    retag(f, total);
    free(z);
    *len = total;
    return f;
}

static SaveError decode_raw(const W *state, const W *images, int content_size_flag) {
    size_t len;
    uint8_t *f = wrap(state, images, &len, content_size_flag);
    SaveData d;
    SaveError e = savefmt_decode(f, len, &d);
    savefmt_free(&d);
    free(f);
    return e;
}

/* image section with one lossless 8x8 image, declared as w x h */
static void wimage(W *w, int decl_w, int decl_h) {
    uint8_t *px = pattern(8, 8, 9);
    uint8_t *webp = NULL;
    size_t n = WebPEncodeLosslessRGBA(px, 8, 8, 32, &webp);
    w32(w, 1);
    w8(w, SAVE_IMG_LOSSLESS);
    w8(w, (uint8_t)decl_w); w8(w, 0);
    w8(w, (uint8_t)decl_h); w8(w, 0);
    w32(w, (uint32_t)n);
    wbytes(w, webp, n);
    WebPFree(webp);
    free(px);
}

static void test_semantic_validation(void) {
    W st, im;

    /* baseline: one wall using image 0, one initiative entry, one waypoint -> OK */
    st.n = 0; im.n = 0;
    wworld(&st, 4);
    wuv(&st, 1); wwall(&st, 0);
    wuv(&st, 1); wuv(&st, 0); wf64(&st, 0.5);
    wuv(&st, 0);
    wuv(&st, 1); wstr(&st, "camp"); wsv(&st, 0); wsv(&st, 0);
    wimage(&im, 8, 8);
    CHECK_ERR(decode_raw(&st, &im, 1), SAVE_OK);

    /* zstd frame without a content size */
    CHECK_ERR(decode_raw(&st, &im, 0), SAVE_ERR_CORRUPT);

    /* declared image size differs from the WebP data */
    W im2 = { .n = 0 };
    wimage(&im2, 9, 8);
    CHECK_ERR(decode_raw(&st, &im2, 1), SAVE_ERR_CORRUPT);

    /* trailing byte in the state section */
    W st2 = st;
    w8(&st2, 0);
    CHECK_ERR(decode_raw(&st2, &im, 1), SAVE_ERR_CORRUPT);

    W noimg = { .n = 0 };
    w32(&noimg, 0);

    /* entity refers to a missing image */
    st.n = 0;
    wworld(&st, 0);
    wuv(&st, 1); wwall(&st, 0);
    wuv(&st, 0); wuv(&st, 0); wuv(&st, 0);
    CHECK_ERR(decode_raw(&st, &noimg, 1), SAVE_ERR_CORRUPT);

    /* initiative refers to a missing entity */
    st.n = 0;
    wworld(&st, 0);
    wuv(&st, 1); wwall(&st, 0);
    wuv(&st, 1); wuv(&st, 5); wf64(&st, 0.5);
    wuv(&st, 0); wuv(&st, 0);
    CHECK_ERR(decode_raw(&st, &im, 1), SAVE_ERR_CORRUPT);

    /* same entity twice in the initiative */
    st.n = 0;
    wworld(&st, 0);
    wuv(&st, 2); wwall(&st, 0); wwall(&st, 0);
    wuv(&st, 2); wuv(&st, 1); wf64(&st, 0.5); wuv(&st, 1); wf64(&st, 0.25);
    wuv(&st, 0); wuv(&st, 0);
    CHECK_ERR(decode_raw(&st, &im, 1), SAVE_ERR_CORRUPT);

    /* initiative value NaN */
    st.n = 0;
    wworld(&st, 0);
    wuv(&st, 1); wwall(&st, 0);
    wuv(&st, 1); wuv(&st, 0); wf64(&st, NAN);
    wuv(&st, 0); wuv(&st, 0);
    CHECK_ERR(decode_raw(&st, &im, 1), SAVE_ERR_CORRUPT);

    /* current turn out of range */
    st.n = 0;
    wworld(&st, 8);
    wuv(&st, 1); wwall(&st, 0);
    wuv(&st, 1); wuv(&st, 0); wf64(&st, 0.5);
    wuv(&st, 1); wuv(&st, 0);
    CHECK_ERR(decode_raw(&st, &im, 1), SAVE_ERR_CORRUPT);

    /* absurd entity count without the data */
    st.n = 0;
    wworld(&st, 0);
    wuv(&st, 100001);
    CHECK_ERR(decode_raw(&st, &noimg, 1), SAVE_ERR_CORRUPT);

    /* waypoint name that is not UTF-8 */
    st.n = 0;
    wworld(&st, 0);
    wuv(&st, 0); wuv(&st, 0); wuv(&st, 0);
    wuv(&st, 1); wstr(&st, "\xC3\x28"); wsv(&st, 0); wsv(&st, 0);
    CHECK_ERR(decode_raw(&st, &noimg, 1), SAVE_ERR_CORRUPT);

    /* waypoint coordinates out of range */
    st.n = 0;
    wworld(&st, 0);
    wuv(&st, 0); wuv(&st, 0); wuv(&st, 0);
    wuv(&st, 1); wstr(&st, "far"); wsv(&st, SAVEFMT_MAX_COORD + 1); wsv(&st, 0);
    CHECK_ERR(decode_raw(&st, &noimg, 1), SAVE_ERR_CORRUPT);

    /* hexSize 0 */
    st.n = 0;
    w8(&st, 0); wuv(&st, 20); wuv(&st, 20); wsv(&st, 0); wsv(&st, 0); wuv(&st, 0); wstr(&st, "light");
    wuv(&st, 0); wuv(&st, 0); wuv(&st, 0); wuv(&st, 0);
    CHECK_ERR(decode_raw(&st, &noimg, 1), SAVE_ERR_CORRUPT);

    /* unknown world flag bit */
    st.n = 0;
    wworld(&st, 0x10);
    wuv(&st, 0); wuv(&st, 0); wuv(&st, 0); wuv(&st, 0);
    CHECK_ERR(decode_raw(&st, &noimg, 1), SAVE_ERR_CORRUPT);

    /* varint longer than 32 bits */
    st.n = 0;
    wworld(&st, 0);
    w8(&st, 0xFF); w8(&st, 0xFF); w8(&st, 0xFF); w8(&st, 0xFF); w8(&st, 0x7F);
    CHECK_ERR(decode_raw(&st, &noimg, 1), SAVE_ERR_CORRUPT);

    /* image section claims more data than there is */
    st.n = 0;
    wworld(&st, 0);
    wuv(&st, 0); wuv(&st, 0); wuv(&st, 0); wuv(&st, 0);
    W bigimg = { .n = 0 };
    w32(&bigimg, 1); w8(&bigimg, 0); w8(&bigimg, 8); w8(&bigimg, 0); w8(&bigimg, 8); w8(&bigimg, 0);
    w32(&bigimg, 0x7FFFFFFF);
    CHECK_ERR(decode_raw(&st, &bigimg, 1), SAVE_ERR_CORRUPT);
}

static void test_encoder_validates(void) {
    Sample s;
    sample_init(&s);
    uint8_t *file;
    size_t len;
    s.waypoints[0].row = SAVEFMT_MAX_COORD + 1;
    CHECK_ERR(savefmt_encode(&s.d, NULL, &file, &len), SAVE_ERR_INVALID);
    CHECK(file == NULL);
    s.waypoints[0].row = 0;
    s.entities[0].image = 7;
    CHECK_ERR(savefmt_encode(&s.d, NULL, &file, &len), SAVE_ERR_INVALID);
    s.entities[0].image = 1;
    s.images[1].rgba = NULL; /* no pixels and no WebP data */
    CHECK_ERR(savefmt_encode(&s.d, NULL, &file, &len), SAVE_ERR_INVALID);
    sample_free(&s);
}

static void test_fingerprint(void) {
    Sample s;
    sample_init(&s);
    uint8_t a[SAVEFMT_TAG_SIZE], b[SAVEFMT_TAG_SIZE];
    CHECK(savefmt_fingerprint(&s.d, a));

    /* camera and theme don't count */
    s.d.offset_x += 500;
    s.d.hex_size = 80;
    snprintf(s.d.theme, sizeof(s.d.theme), "red");
    CHECK(savefmt_fingerprint(&s.d, b));
    CHECK(memcmp(a, b, sizeof(a)) == 0);

    /* moving an entity does */
    s.entities[2].row++;
    CHECK(savefmt_fingerprint(&s.d, b));
    CHECK(memcmp(a, b, sizeof(a)) != 0);
    s.entities[2].row--;

    /* so does a different image (new texture id) */
    s.images[0].id = 999;
    CHECK(savefmt_fingerprint(&s.d, b));
    CHECK(memcmp(a, b, sizeof(a)) != 0);
    sample_free(&s);
}

static int write_sample(const char *path) {
    Sample s;
    sample_init(&s);
    size_t len;
    uint8_t *file = encode_sample(&s, &len);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(file, 1, len, f) != len) {
        perror(path);
        return 1;
    }
    fclose(f);
    free(file);
    sample_free(&s);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2) return write_sample(argv[1]);
    test_tag();
    test_roundtrip();
    test_empty_table();
    test_blobs();
    test_scale_down();
    test_every_byte_flip_rejected();
    test_every_truncation_rejected();
    test_header_errors();
    test_semantic_validation();
    test_encoder_validates();
    test_fingerprint();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
```

- [ ] **Step 2: Run the tests and watch them fail**

Run: `make test`
Expected: FAIL. Make stops with `No rule to make target 'src/savefmt.c', needed by 'build/save_test'`.

- [ ] **Step 3: Commit**

```bash
git add tests/save_test.c
git commit -m "test: add save format unit tests"
```

---

### Task 4: Format implementation (`savefmt.c`)

**Files:**
- Create: `src/savefmt.c`

Key points (see the spec for the reasons behind them):

- **Decode order:**
  1. magic, size, version and flags;
  2. the tag (constant-time compare);
  3. the zstd frame: its content size must be known and ≤ 16 MiB, and it must be exactly one frame;
  4. the state, with bounds-checked readers (the `Rd` struct with a sticky `bad` flag);
  5. the images, with dimensions checked against the WebP header and a total pixel budget;
  6. finally `validate()` checks all cross-references.
- **The encoder runs the same `validate()`**, so the game can never write a file it would reject.
- **`SAVEFMT_FUZZING`** skips the tag check so the fuzzer can reach the parser.

- [ ] **Step 1: Create `src/savefmt.c`**

```c
/* .pfsave encoding, decoding and validation. See savefmt.h. */
#include "savefmt.h"

#include <math.h>
#include <sodium.h>
#include <stdlib.h>
#include <string.h>
#include <webp/decode.h>
#include <webp/encode.h>
#include <zstd.h>

static const uint8_t MAGIC[4] = { 'P', 'F', 'S', 'V' };

/* Key of the integrity tag. It ships with the game, so the tag detects corruption and
 * casual edits but does not prove who made a file (see the spec, "Integrity tag"). */
static const uint8_t TAG_KEY[32] = {
    0x3d, 0x56, 0xbc, 0xc9, 0xde, 0x4f, 0x53, 0x34, 0xb4, 0x6e, 0xd8, 0x90, 0x58, 0x20, 0x42, 0x86,
    0xc3, 0x9b, 0x4e, 0xa0, 0xb6, 0xd1, 0xcf, 0x91, 0xa5, 0x1b, 0x09, 0x85, 0x67, 0xb0, 0x27, 0x94,
};

#define ZSTD_LEVEL 19
#define WEBP_LOSSY_QUALITY 90

/* ---- growable output buffer --------------------------------------------------------- */

typedef struct {
    uint8_t *p;
    size_t len, cap;
    bool oom;
} Buf;

static void buf_reserve(Buf *b, size_t extra) {
    if (b->oom) return;
    if (extra > SIZE_MAX - b->len) { b->oom = true; return; }
    size_t need = b->len + extra;
    if (need <= b->cap) return;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < need) cap = cap > SIZE_MAX / 2 ? need : cap * 2;
    uint8_t *np = realloc(b->p, cap);
    if (!np) { b->oom = true; return; }
    b->p = np;
    b->cap = cap;
}

static void put_bytes(Buf *b, const void *src, size_t n) {
    buf_reserve(b, n);
    if (b->oom) return;
    if (n) memcpy(b->p + b->len, src, n);
    b->len += n;
}

static void put_u8(Buf *b, uint8_t v) { put_bytes(b, &v, 1); }

static void put_u16(Buf *b, uint16_t v) {
    uint8_t t[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    put_bytes(b, t, 2);
}

static void put_u32(Buf *b, uint32_t v) {
    uint8_t t[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    put_bytes(b, t, 4);
}

/* unsigned LEB128 */
static void put_uv(Buf *b, uint32_t v) {
    uint8_t t[5];
    int n = 0;
    do {
        uint8_t c = v & 0x7f;
        v >>= 7;
        if (v) c |= 0x80;
        t[n++] = c;
    } while (v);
    put_bytes(b, t, (size_t)n);
}

/* zig-zag: small negative numbers stay small */
static void put_sv(Buf *b, int32_t v) { put_uv(b, ((uint32_t)v << 1) ^ (v < 0 ? 0xFFFFFFFFu : 0)); }

static void put_f64(Buf *b, double v) {
    uint64_t u;
    memcpy(&u, &v, 8);
    put_u32(b, (uint32_t)u);
    put_u32(b, (uint32_t)(u >> 32));
}

static void put_str(Buf *b, const char *s) {
    size_t n = strlen(s);
    put_uv(b, (uint32_t)n);
    put_bytes(b, s, n);
}

/* ---- bounds-checked reader ------------------------------------------------------------ */

typedef struct {
    const uint8_t *p;
    size_t len, pos;
    bool bad; /* sticky: any failed read marks the whole input as bad */
} Rd;

static bool rd_need(Rd *r, size_t n) {
    if (r->bad || n > r->len - r->pos) {
        r->bad = true;
        return false;
    }
    return true;
}

static uint8_t get_u8(Rd *r) { return rd_need(r, 1) ? r->p[r->pos++] : 0; }

static uint16_t get_u16(Rd *r) {
    if (!rd_need(r, 2)) return 0;
    uint16_t v = (uint16_t)(r->p[r->pos] | r->p[r->pos + 1] << 8);
    r->pos += 2;
    return v;
}

static uint32_t get_u32(Rd *r) {
    if (!rd_need(r, 4)) return 0;
    const uint8_t *q = r->p + r->pos;
    r->pos += 4;
    return (uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24;
}

static uint32_t get_uv(Rd *r) {
    uint32_t v = 0;
    for (int i = 0; i < 5; i++) {
        uint8_t c = get_u8(r);
        if (r->bad) return 0;
        if (i == 4 && (c & 0xF0)) { /* more than 32 bits */
            r->bad = true;
            return 0;
        }
        v |= (uint32_t)(c & 0x7f) << (7 * i);
        if (!(c & 0x80)) return v;
    }
    return v; /* not reached */
}

static int32_t get_sv(Rd *r) {
    uint32_t u = get_uv(r);
    uint32_t x = (u >> 1) ^ (0u - (u & 1));
    int32_t v;
    memcpy(&v, &x, 4);
    return v;
}

static double get_f64(Rd *r) {
    uint64_t lo = get_u32(r), hi = get_u32(r);
    uint64_t u = lo | hi << 32;
    double v;
    memcpy(&v, &u, 8);
    return v;
}

static bool utf8_valid(const char *s, size_t n) {
    const uint8_t *p = (const uint8_t *)s;
    size_t i = 0;
    while (i < n) {
        uint8_t c = p[i];
        if (c == 0) return false;
        if (c < 0x80) { i++; continue; }
        size_t len;
        uint32_t cp, min;
        if ((c & 0xE0) == 0xC0)      { len = 2; cp = c & 0x1F; min = 0x80; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; min = 0x800; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; min = 0x10000; }
        else return false;
        if (len > n - i) return false;
        for (size_t k = 1; k < len; k++) {
            if ((p[i + k] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += len;
    }
    return true;
}

/* Reads a string into out[cap]; it must be shorter than cap and valid UTF-8. */
static void get_str(Rd *r, char *out, size_t cap) {
    uint32_t n = get_uv(r);
    if (r->bad || n > SAVEFMT_MAX_STR || n >= cap || !rd_need(r, n) || !utf8_valid((const char *)r->p + r->pos, n)) {
        r->bad = true;
        out[0] = '\0';
        return;
    }
    memcpy(out, r->p + r->pos, n);
    out[n] = '\0';
    r->pos += n;
}

/* ---- validation (shared by encoder and decoder) ------------------------------------------ */

static bool str_ok(const char *s, size_t cap) {
    size_t n = strnlen(s, cap);
    return n < cap && n <= SAVEFMT_MAX_STR && utf8_valid(s, n);
}

static bool coord_ok(int32_t v) { return v >= -SAVEFMT_MAX_COORD && v <= SAVEFMT_MAX_COORD; }

/* encoding: images given as rgba may exceed SAVEFMT_MAX_DIM (they get scaled down) and
 * the total pixel limit is not checked (the pixels are already in memory). */
static bool validate(const SaveData *d, bool encoding) {
    if (d->nimages > SAVEFMT_MAX_IMAGES || d->nentities > SAVEFMT_MAX_ENTITIES ||
        d->ninit > d->nentities || d->nwaypoints > SAVEFMT_MAX_WAYPOINTS)
        return false;
    if (d->has_background && d->background >= d->nimages) return false;
    if (d->bg_rows < 1 || d->bg_rows > SAVEFMT_MAX_GRID || d->bg_cols < 1 || d->bg_cols > SAVEFMT_MAX_GRID) return false;
    if (d->offset_x < -SAVEFMT_MAX_OFFSET || d->offset_x > SAVEFMT_MAX_OFFSET ||
        d->offset_y < -SAVEFMT_MAX_OFFSET || d->offset_y > SAVEFMT_MAX_OFFSET)
        return false;
    if (d->hex_size < SAVEFMT_MIN_HEX || d->hex_size > SAVEFMT_MAX_HEX) return false;
    if (!str_ok(d->theme, sizeof(d->theme))) return false;

    uint64_t pixel_bytes = 0;
    for (uint32_t i = 0; i < d->nimages; i++) {
        const SaveImage *im = &d->images[i];
        int max = (encoding && !im->webp) ? 65535 : SAVEFMT_MAX_DIM;
        if (im->kind > SAVE_IMG_LOSSY || im->w < 1 || im->h < 1 || im->w > max || im->h > max) return false;
        if (encoding && !im->rgba && !im->webp) return false;
        pixel_bytes += (uint64_t)im->w * (uint64_t)im->h * 4;
    }
    if (!encoding && pixel_bytes > SAVEFMT_MAX_PIXEL_BYTES) return false;

    for (uint32_t i = 0; i < d->nentities; i++) {
        const SaveEntity *e = &d->entities[i];
        if (e->type > SAVE_ENT_WALL || e->image >= d->nimages || !coord_ok(e->row) || !coord_ok(e->col)) return false;
        if (e->type == SAVE_ENT_CHARACTER && e->speed > SAVEFMT_MAX_SPEED) return false;
    }

    if (d->ninit > 0) {
        uint8_t *seen = calloc(d->nentities, 1);
        if (!seen) return false;
        bool ok = true;
        for (uint32_t i = 0; i < d->ninit && ok; i++) {
            uint32_t e = d->init[i].entity;
            if (e >= d->nentities || seen[e] || !isfinite(d->init[i].value)) ok = false;
            else seen[e] = 1;
        }
        free(seen);
        if (!ok) return false;
    }
    if (d->ninit == 0 ? (d->current_turn != 0 || d->turn_started) : d->current_turn >= d->ninit) return false;

    for (uint32_t i = 0; i < d->nwaypoints; i++) {
        const SaveWaypoint *w = &d->waypoints[i];
        if (w->name[0] == '\0' || !str_ok(w->name, sizeof(w->name)) || !coord_ok(w->row) || !coord_ok(w->col))
            return false;
    }
    return true;
}

/* ---- state section ----------------------------------------------------------------------- */

/* fingerprint: camera and theme are left out (they don't make the table "modified") */
static void write_state(Buf *b, const SaveData *d, bool fingerprint) {
    put_u8(b, (uint8_t)((d->is_flat ? 1 : 0) | (d->has_background ? 2 : 0) |
                        (d->init_markers_visible ? 4 : 0) | (d->turn_started ? 8 : 0)));
    if (d->has_background) put_uv(b, d->background);
    put_uv(b, d->bg_rows);
    put_uv(b, d->bg_cols);
    put_sv(b, fingerprint ? 0 : d->offset_x);
    put_sv(b, fingerprint ? 0 : d->offset_y);
    put_uv(b, fingerprint ? 0 : d->hex_size);
    put_str(b, fingerprint ? "" : d->theme);

    put_uv(b, d->nentities);
    for (uint32_t i = 0; i < d->nentities; i++) {
        const SaveEntity *e = &d->entities[i];
        put_u8(b, e->type);
        put_uv(b, e->image);
        put_sv(b, e->row);
        put_sv(b, e->col);
        if (e->type == SAVE_ENT_CHARACTER) {
            put_sv(b, e->size);
            put_sv(b, e->max_health);
            put_sv(b, e->health);
            put_sv(b, e->attack);
            put_sv(b, e->ac);
            put_sv(b, e->speed);
            put_sv(b, e->initiative);
        }
    }

    put_uv(b, d->ninit);
    for (uint32_t i = 0; i < d->ninit; i++) {
        put_uv(b, d->init[i].entity);
        put_f64(b, d->init[i].value);
    }
    put_uv(b, d->current_turn);

    put_uv(b, d->nwaypoints);
    for (uint32_t i = 0; i < d->nwaypoints; i++) {
        put_str(b, d->waypoints[i].name);
        put_sv(b, d->waypoints[i].row);
        put_sv(b, d->waypoints[i].col);
    }
}

static SaveError read_state(const uint8_t *p, size_t n, SaveData *d) {
    Rd r = { p, n, 0, false };
    uint8_t flags = get_u8(&r);
    if (flags & ~15u) return SAVE_ERR_CORRUPT;
    d->is_flat = flags & 1;
    d->has_background = flags & 2;
    d->init_markers_visible = flags & 4;
    d->turn_started = flags & 8;
    if (d->has_background) d->background = get_uv(&r);
    d->bg_rows = get_uv(&r);
    d->bg_cols = get_uv(&r);
    d->offset_x = get_sv(&r);
    d->offset_y = get_sv(&r);
    d->hex_size = get_uv(&r);
    get_str(&r, d->theme, sizeof(d->theme));

    /* counts are checked before allocating: every record takes at least one byte */
    uint32_t ne = get_uv(&r);
    if (r.bad || ne > SAVEFMT_MAX_ENTITIES || ne > n - r.pos) return SAVE_ERR_CORRUPT;
    d->entities = calloc(ne ? ne : 1, sizeof(*d->entities));
    if (!d->entities) return SAVE_ERR_INTERNAL;
    d->nentities = ne;
    for (uint32_t i = 0; i < ne && !r.bad; i++) {
        SaveEntity *e = &d->entities[i];
        e->type = get_u8(&r);
        e->image = get_uv(&r);
        e->row = get_sv(&r);
        e->col = get_sv(&r);
        if (e->type == SAVE_ENT_CHARACTER) {
            e->size = get_sv(&r);
            e->max_health = get_sv(&r);
            e->health = get_sv(&r);
            e->attack = get_sv(&r);
            e->ac = get_sv(&r);
            e->speed = get_sv(&r);
            e->initiative = get_sv(&r);
        } else if (e->type != SAVE_ENT_WALL) {
            return SAVE_ERR_CORRUPT;
        }
    }

    uint32_t ni = get_uv(&r);
    if (r.bad || ni > ne) return SAVE_ERR_CORRUPT;
    d->init = calloc(ni ? ni : 1, sizeof(*d->init));
    if (!d->init) return SAVE_ERR_INTERNAL;
    d->ninit = ni;
    for (uint32_t i = 0; i < ni && !r.bad; i++) {
        d->init[i].entity = get_uv(&r);
        d->init[i].value = get_f64(&r);
    }
    d->current_turn = get_uv(&r);

    uint32_t nw = get_uv(&r);
    if (r.bad || nw > SAVEFMT_MAX_WAYPOINTS || nw > n - r.pos) return SAVE_ERR_CORRUPT;
    d->waypoints = calloc(nw ? nw : 1, sizeof(*d->waypoints));
    if (!d->waypoints) return SAVE_ERR_INTERNAL;
    d->nwaypoints = nw;
    for (uint32_t i = 0; i < nw && !r.bad; i++) {
        get_str(&r, d->waypoints[i].name, sizeof(d->waypoints[i].name));
        d->waypoints[i].row = get_sv(&r);
        d->waypoints[i].col = get_sv(&r);
    }

    if (r.bad || r.pos != n) return SAVE_ERR_CORRUPT; /* also rejects trailing bytes */
    return SAVE_OK;
}

/* ---- images ---------------------------------------------------------------------------------- */

static bool encode_webp(const SaveImage *im, SaveBlob *out) {
    WebPConfig cfg;
    if (im->kind == SAVE_IMG_LOSSY) {
        if (!WebPConfigPreset(&cfg, WEBP_PRESET_PICTURE, WEBP_LOSSY_QUALITY)) return false;
        cfg.method = 6;
    } else {
        if (!WebPConfigInit(&cfg) || !WebPConfigLosslessPreset(&cfg, 6)) return false;
        cfg.exact = 1; /* keep the colour of transparent pixels: pixel-exact round trip */
    }
    WebPPicture pic;
    if (!WebPPictureInit(&pic)) return false;
    pic.use_argb = 1;
    pic.width = im->w;
    pic.height = im->h;
    bool ok = WebPPictureImportRGBA(&pic, im->rgba, im->w * 4);
    if (ok && (pic.width > SAVEFMT_MAX_DIM || pic.height > SAVEFMT_MAX_DIM)) {
        double s = (double)SAVEFMT_MAX_DIM / (pic.width > pic.height ? pic.width : pic.height);
        int nw = (int)(pic.width * s), nh = (int)(pic.height * s);
        ok = WebPPictureRescale(&pic, nw < 1 ? 1 : nw, nh < 1 ? 1 : nh);
    }
    WebPMemoryWriter wr;
    WebPMemoryWriterInit(&wr);
    if (ok) {
        pic.writer = WebPMemoryWrite;
        pic.custom_ptr = &wr;
        ok = WebPEncode(&cfg, &pic);
    }
    out->w = pic.width;
    out->h = pic.height;
    WebPPictureFree(&pic);
    if (ok && wr.size > 0) {
        out->webp = malloc(wr.size);
        ok = out->webp != NULL;
        if (ok) {
            memcpy(out->webp, wr.mem, wr.size);
            out->len = wr.size;
        }
    } else {
        ok = false;
    }
    WebPMemoryWriterClear(&wr);
    return ok;
}

/* ---- public API ---------------------------------------------------------------------------- */

void savefmt_compute_tag(const uint8_t *buf, size_t len, uint8_t out[SAVEFMT_TAG_SIZE]) {
    crypto_generichash(out, SAVEFMT_TAG_SIZE, buf, len, TAG_KEY, sizeof(TAG_KEY));
}

void savefmt_blobs_free(SaveBlob *blobs, uint32_t n) {
    if (!blobs) return;
    for (uint32_t i = 0; i < n; i++) {
        free(blobs[i].webp);
        blobs[i] = (SaveBlob){ 0 };
    }
}

SaveError savefmt_encode(const SaveData *d, SaveBlob *blobs_out, uint8_t **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    if (blobs_out) memset(blobs_out, 0, sizeof(*blobs_out) * d->nimages);
    if (sodium_init() < 0) return SAVE_ERR_INTERNAL;
    if (!validate(d, true)) return SAVE_ERR_INVALID;

    Buf state = { 0 };
    write_state(&state, d, false);
    if (state.oom) { free(state.p); return SAVE_ERR_INTERNAL; }
    if (state.len > SAVEFMT_MAX_STATE) { free(state.p); return SAVE_ERR_TOO_LARGE; }

    SaveError err = SAVE_OK;
    Buf f = { 0 };
    put_bytes(&f, MAGIC, 4);
    put_u16(&f, SAVEFMT_VERSION);
    put_u16(&f, 0);
    put_u32(&f, 0); /* state_len, patched below */
    size_t bound = ZSTD_compressBound(state.len);
    buf_reserve(&f, bound);
    if (f.oom) {
        err = SAVE_ERR_INTERNAL;
        goto done;
    }
    size_t clen = ZSTD_compress(f.p + f.len, bound, state.p, state.len, ZSTD_LEVEL);
    if (ZSTD_isError(clen)) {
        err = SAVE_ERR_ENCODE;
        goto done;
    }
    for (int i = 0; i < 4; i++) f.p[8 + i] = (uint8_t)(clen >> (8 * i));
    f.len += clen;

    put_u32(&f, d->nimages);
    for (uint32_t i = 0; i < d->nimages && !f.oom; i++) {
        const SaveImage *im = &d->images[i];
        SaveBlob fresh = { 0 };
        const uint8_t *data = im->webp;
        size_t len = im->webp_len;
        int w = im->w, h = im->h, iw, ih;
        if (data) {
            if (!WebPGetInfo(data, len, &iw, &ih) || iw != w || ih != h) {
                err = SAVE_ERR_INVALID;
                goto done;
            }
        } else {
            if (!encode_webp(im, &fresh)) {
                free(fresh.webp);
                err = SAVE_ERR_ENCODE;
                goto done;
            }
            data = fresh.webp;
            len = fresh.len;
            w = fresh.w;
            h = fresh.h;
        }
        if (len > SAVEFMT_MAX_FILE || f.len + len > SAVEFMT_MAX_FILE) {
            free(fresh.webp);
            err = SAVE_ERR_TOO_LARGE;
            goto done;
        }
        put_u8(&f, im->kind);
        put_u16(&f, (uint16_t)w);
        put_u16(&f, (uint16_t)h);
        put_u32(&f, (uint32_t)len);
        put_bytes(&f, data, len);
        if (fresh.webp) {
            if (blobs_out) blobs_out[i] = fresh;
            else free(fresh.webp);
        }
    }

    buf_reserve(&f, SAVEFMT_TAG_SIZE);
    if (f.oom) {
        err = SAVE_ERR_INTERNAL;
        goto done;
    }
    savefmt_compute_tag(f.p, f.len, f.p + f.len);
    f.len += SAVEFMT_TAG_SIZE;
    if (f.len > SAVEFMT_MAX_FILE) err = SAVE_ERR_TOO_LARGE;

done:
    free(state.p);
    if (err != SAVE_OK) {
        free(f.p);
        return err;
    }
    *out = f.p;
    *out_len = f.len;
    return SAVE_OK;
}

static SaveError decode_images(Rd *r, SaveData *d) {
    uint32_t n = get_u32(r);
    if (r->bad || n > SAVEFMT_MAX_IMAGES) return SAVE_ERR_CORRUPT;
    d->images = calloc(n ? n : 1, sizeof(*d->images));
    if (!d->images) return SAVE_ERR_INTERNAL;
    d->nimages = n;
    uint64_t pixel_bytes = 0;
    for (uint32_t i = 0; i < n; i++) {
        SaveImage *im = &d->images[i];
        im->kind = get_u8(r);
        im->w = get_u16(r);
        im->h = get_u16(r);
        uint32_t len = get_u32(r);
        if (r->bad || im->kind > SAVE_IMG_LOSSY || im->w < 1 || im->h < 1 || im->w > SAVEFMT_MAX_DIM ||
            im->h > SAVEFMT_MAX_DIM || !rd_need(r, len))
            return SAVE_ERR_CORRUPT;
        size_t px = (size_t)im->w * (size_t)im->h * 4;
        pixel_bytes += px;
        if (pixel_bytes > SAVEFMT_MAX_PIXEL_BYTES) return SAVE_ERR_TOO_LARGE;
        const uint8_t *data = r->p + r->pos;
        r->pos += len;
        int w, h;
        if (!WebPGetInfo(data, len, &w, &h) || w != im->w || h != im->h) return SAVE_ERR_CORRUPT;
        uint8_t *rgba = malloc(px);
        uint8_t *webp = malloc(len ? len : 1);
        im->rgba = rgba;
        im->webp = webp;
        if (!rgba || !webp) return SAVE_ERR_INTERNAL;
        memcpy(webp, data, len);
        im->webp_len = len;
        if (!WebPDecodeRGBAInto(data, len, rgba, px, im->w * 4)) return SAVE_ERR_CORRUPT;
    }
    return SAVE_OK;
}

SaveError savefmt_decode(const uint8_t *buf, size_t len, SaveData *out) {
    memset(out, 0, sizeof(*out));
    out->owns_memory = true;
    if (len < 4 || memcmp(buf, MAGIC, 4) != 0) return SAVE_ERR_NOT_SAVE;
    if (len > SAVEFMT_MAX_FILE) return SAVE_ERR_TOO_LARGE;
    if (len < SAVEFMT_HEADER_SIZE + 4 + SAVEFMT_TAG_SIZE) return SAVE_ERR_CORRUPT;
    uint16_t version = (uint16_t)(buf[4] | buf[5] << 8);
    uint16_t flags = (uint16_t)(buf[6] | buf[7] << 8);
    if (version > SAVEFMT_VERSION) return SAVE_ERR_NEWER;
    if (version == 0 || flags != 0) return SAVE_ERR_CORRUPT;
    if (sodium_init() < 0) return SAVE_ERR_INTERNAL;

#ifndef SAVEFMT_FUZZING /* the fuzzer skips the tag so that it reaches the parser */
    uint8_t tag[SAVEFMT_TAG_SIZE];
    savefmt_compute_tag(buf, len - SAVEFMT_TAG_SIZE, tag);
    if (sodium_memcmp(tag, buf + len - SAVEFMT_TAG_SIZE, SAVEFMT_TAG_SIZE) != 0) return SAVE_ERR_CORRUPT;
#endif

    Rd r = { buf, len - SAVEFMT_TAG_SIZE, 8, false };
    uint32_t state_len = get_u32(&r);
    if (r.bad || !rd_need(&r, state_len)) return SAVE_ERR_CORRUPT;
    const uint8_t *z = buf + r.pos;
    r.pos += state_len;

    unsigned long long size = ZSTD_getFrameContentSize(z, state_len);
    if (size == ZSTD_CONTENTSIZE_UNKNOWN || size == ZSTD_CONTENTSIZE_ERROR) return SAVE_ERR_CORRUPT;
    if (size > SAVEFMT_MAX_STATE) return SAVE_ERR_TOO_LARGE;
    size_t frame = ZSTD_findFrameCompressedSize(z, state_len);
    if (ZSTD_isError(frame) || frame != state_len) return SAVE_ERR_CORRUPT; /* exactly one frame */

    uint8_t *state = malloc(size ? (size_t)size : 1);
    if (!state) return SAVE_ERR_INTERNAL;
    size_t got = ZSTD_decompress(state, (size_t)size, z, state_len);
    SaveError err = (ZSTD_isError(got) || got != size) ? SAVE_ERR_CORRUPT : read_state(state, (size_t)size, out);
    free(state);
    if (err == SAVE_OK) err = decode_images(&r, out);
    if (err == SAVE_OK && r.pos != r.len) err = SAVE_ERR_CORRUPT;
    if (err == SAVE_OK && !validate(out, false)) err = SAVE_ERR_CORRUPT;
    if (err != SAVE_OK) {
        savefmt_free(out);
        out->owns_memory = true;
    }
    return err;
}

void savefmt_free(SaveData *d) {
    if (!d) return;
    if (d->owns_memory) {
        for (uint32_t i = 0; d->images && i < d->nimages; i++) {
            free((void *)d->images[i].rgba);
            free((void *)d->images[i].webp);
        }
        free(d->images);
        free(d->entities);
        free(d->init);
        free(d->waypoints);
    }
    memset(d, 0, sizeof(*d));
}

bool savefmt_fingerprint(const SaveData *d, uint8_t out[SAVEFMT_TAG_SIZE]) {
    if (sodium_init() < 0) return false;
    Buf b = { 0 };
    write_state(&b, d, true);
    put_uv(&b, d->nimages);
    for (uint32_t i = 0; i < d->nimages; i++) {
        put_u8(&b, d->images[i].kind);
        put_u32(&b, (uint32_t)d->images[i].id);
        put_u32(&b, (uint32_t)(d->images[i].id >> 32));
    }
    bool ok = !b.oom;
    if (ok) crypto_generichash(out, SAVEFMT_TAG_SIZE, b.p, b.len, NULL, 0);
    free(b.p);
    return ok;
}

const char *savefmt_strerror(SaveError e) {
    switch (e) {
    case SAVE_OK:            return "OK";
    case SAVE_ERR_NOT_SAVE:  return "Not a pathfinder save file";
    case SAVE_ERR_NEWER:     return "Save file was made by a newer version of pathfinder";
    case SAVE_ERR_CORRUPT:   return "Save file is damaged or was modified";
    case SAVE_ERR_TOO_LARGE: return "Save file is too large";
    case SAVE_ERR_INVALID:   return "The table contains values that cannot be saved";
    case SAVE_ERR_ENCODE:    return "Could not encode the save file";
    case SAVE_ERR_INTERNAL:  return "Out of memory or internal error";
    }
    return "Unknown error";
}
```

- [ ] **Step 2: Run the tests and watch them pass**

Run: `make test`
Expected: `71 checks, 0 failed`

- [ ] **Step 3: Run them under the sanitizers with warnings as errors**

Run: `rm -f build/save_test && make test CFLAGS="-O1 -g -fsanitize=address,undefined -Werror" && rm -f build/save_test`
Expected: `71 checks, 0 failed`, with no sanitizer reports and no warnings.

- [ ] **Step 4: Commit**

```bash
git add src/savefmt.c
git commit -m "savefmt: implement .pfsave encoding, decoding and validation"
```

---

### Task 5: Fuzz target

**Files:**
- Create: `tests/fuzz_savefmt.c`

- [ ] **Step 1: Create `tests/fuzz_savefmt.c`**

```c
/* libFuzzer target for the save file decoder. Built by `make fuzz` with -DSAVEFMT_FUZZING,
 * which skips the integrity tag so that mutated inputs reach the parser. */
#include "savefmt.h"

#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    SaveData d;
    if (savefmt_decode(data, size, &d) == SAVE_OK) {
        /* everything the decoder accepts must be writable again */
        uint8_t *out;
        size_t len;
        if (savefmt_encode(&d, NULL, &out, &len) != SAVE_OK) abort();
        free(out);
    }
    savefmt_free(&d);
    return 0;
}
```

- [ ] **Step 2: Fuzz for a minute**

Run: `make fuzz FUZZ_TIME=60 2>&1 | grep -E "Done|ERROR|SUMMARY"`
Expected: `Done <N> runs in 6x second(s)`, with no `ERROR` or `SUMMARY` lines. If a crash is found, libFuzzer writes a
`crash-*` file: reproduce it with `./build/fuzz_savefmt crash-*`, fix `savefmt.c`, and add the input as a case to
`test_semantic_validation`.

- [ ] **Step 3: Commit**

```bash
git add tests/fuzz_savefmt.c
git commit -m "test: add libFuzzer target for the save decoder"
```

---

### Task 6: Textures keep their source data

**Files:**
- Modify: `src/vk_renderer.h`, `src/vk_renderer.c`

What the patch adds:

- **Unique id:** each texture gets a serial id that is never reused, which the fingerprint uses.
- **Pixels:** an optional owned RGBA copy.
- **Blob:** an optional owned encoded copy. Setting a blob frees the pixel copy, so a big background costs CPU
  memory only until its first save.
- **Cleanup:** both copies are freed together with the texture.

- [ ] **Step 1: Patch `src/vk_renderer.h`**

Save the diff below as `/tmp/src_vk_renderer.h.patch` and apply it from the project root with
`patch -p0 < /tmp/src_vk_renderer.h.patch` (or make the same edits by hand):

```diff
--- src/vk_renderer.h
+++ src/vk_renderer.h
@@ -39,6 +39,17 @@
 int  vkr_texture_width(const Texture *t);
 int  vkr_texture_height(const Texture *t);
 
+/* CPU-side source data for save files. A texture keeps its RGBA pixels and/or an
+ * encoded copy (the "blob", e.g. WebP); setting a blob frees the pixels. */
+/* Like vkr_texture_create (RGBA), but takes ownership of `pixels` (malloc'd) and keeps them. */
+Texture *vkr_texture_create_kept(int w, int h, uint8_t *pixels);
+const uint8_t *vkr_texture_pixels(const Texture *t); /* NULL when not kept */
+/* Takes ownership of blob (malloc'd); kind, w and h describe the encoded image. */
+void vkr_texture_set_blob(Texture *t, uint8_t *blob, size_t len, int kind, int w, int h);
+const uint8_t *vkr_texture_blob(const Texture *t, size_t *len, int *kind, int *w, int *h); /* NULL if none */
+/* Unique serial number; never reused within a run. */
+uint64_t vkr_texture_id(const Texture *t);
+
 /* Renders and presents one frame. `clear` is RGBA in [0,1].
  * If screenshot_path is non-NULL the presented image is also saved as PNG. */
 void vkr_render(const Vertex *verts, int nverts, const DrawCmd *cmds, int ncmds,
```

- [ ] **Step 2: Patch `src/vk_renderer.c`**

Save the diff below as `/tmp/src_vk_renderer.c.patch` and apply it from the project root with
`patch -p0 < /tmp/src_vk_renderer.c.patch` (or make the same edits by hand):

```diff
--- src/vk_renderer.c
+++ src/vk_renderer.c
@@ -32,6 +32,11 @@
     int w, h;
     bool alpha_only;
     int refs;
+    uint64_t id;      /* unique, never reused (save file fingerprint) */
+    uint8_t *pixels;  /* optional RGBA copy, see vkr_texture_create_kept */
+    uint8_t *blob;    /* optional encoded copy, see vkr_texture_set_blob */
+    size_t blob_len;
+    int blob_kind, blob_w, blob_h;
 };
 
 typedef struct {
@@ -735,6 +740,8 @@
     t->h = h;
     t->alpha_only = alpha_only;
     t->refs = 1;
+    static uint64_t next_id = 1;
+    t->id = next_id++;
     VkFormat fmt = alpha_only ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
     create_image((uint32_t)w, (uint32_t)h, fmt, VK_SAMPLE_COUNT_1_BIT,
                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
@@ -778,9 +785,42 @@
     vkDestroyImageView(R.device, t->view, NULL);
     vkDestroyImage(R.device, t->image, NULL);
     vkFreeMemory(R.device, t->memory, NULL);
+    free(t->pixels);
+    free(t->blob);
     free(t);
 }
 
+Texture *vkr_texture_create_kept(int w, int h, uint8_t *pixels) {
+    Texture *t = vkr_texture_create(w, h, pixels, false);
+    t->pixels = pixels;
+    return t;
+}
+
+const uint8_t *vkr_texture_pixels(const Texture *t) { return t->pixels; }
+
+void vkr_texture_set_blob(Texture *t, uint8_t *blob, size_t len, int kind, int w, int h) {
+    if (!blob) return;
+    free(t->blob);
+    t->blob = blob;
+    t->blob_len = len;
+    t->blob_kind = kind;
+    t->blob_w = w;
+    t->blob_h = h;
+    free(t->pixels); /* the blob is enough to save the texture again */
+    t->pixels = NULL;
+}
+
+const uint8_t *vkr_texture_blob(const Texture *t, size_t *len, int *kind, int *w, int *h) {
+    if (!t->blob) return NULL;
+    *len = t->blob_len;
+    *kind = t->blob_kind;
+    *w = t->blob_w;
+    *h = t->blob_h;
+    return t->blob;
+}
+
+uint64_t vkr_texture_id(const Texture *t) { return t->id; }
+
 int vkr_texture_width(const Texture *t)  { return t->w; }
 int vkr_texture_height(const Texture *t) { return t->h; }
 
```

- [ ] **Step 3: Build**

Run: `make 2>&1 | grep -E "warning|error"`
Expected: no output.

- [ ] **Step 4: Commit**

```bash
git add src/vk_renderer.h src/vk_renderer.c
git commit -m "renderer: textures can keep pixels or an encoded blob for saving"
```

---

### Task 7: Restoring the initiative order

**Files:**
- Modify: `src/game.c` (its declaration in `src/app.h` is added by the `app.h` patch in Task 8)

**Why a separate function:** `initiative_add` rolls a random value and the list gets sorted. A loaded table must
keep the saved values, order, current turn and whether a turn has started (`currentTurn` stays `NULL` until the
first `:i next`). So the marker-creating part is split out into `initiative_push(e, value)`.

- [ ] **Step 1: Patch `src/game.c`**

Save the diff below as `/tmp/src_game.c.patch` and apply it from the project root with
`patch -p0 < /tmp/src_game.c.patch` (or make the same edits by hand):

```diff
--- src/game.c
+++ src/game.c
@@ -32,9 +32,8 @@
 
 bool game_is_in_initiative(const Entity *e) { return init_index(e) >= 0; }
 
-static void initiative_add(Entity *e) {
-    if (!e || game_is_in_initiative(e)) return;
-    double value = rand_unit();
+/* Appends e with the given roll value and creates its marker. */
+static void initiative_push(Entity *e, double value) {
     da_push(&gh->init, ((InitEntry){ e, value }));
 
     /* visual marker above the entity */
@@ -48,6 +47,11 @@
     marker_add(m);
 }
 
+static void initiative_add(Entity *e) {
+    if (!e || game_is_in_initiative(e)) return;
+    initiative_push(e, rand_unit());
+}
+
 /* Highest initiative first; stable so equal values keep their order. */
 static void sort_initiative(void) {
     for (int i = 1; i < gh->init.count; i++) {
@@ -133,3 +137,16 @@
     gh->currentTurn = NULL;
     gh->currentTurnIndex = 0;
 }
+
+void game_restore_initiative(Entity **entities, const double *values, int n, int current, bool turn_started,
+                             bool markers_visible) {
+    game_clear_initiative();
+    for (int i = 0; i < n; i++)
+        if (entities[i] && !game_is_in_initiative(entities[i])) initiative_push(entities[i], values[i]);
+    if (gh->init.count > 0) {
+        gh->currentTurnIndex = (current >= 0 && current < gh->init.count) ? current : 0;
+        gh->currentTurn = turn_started ? gh->init.items[gh->currentTurnIndex].entity : NULL;
+    }
+    game_update_initiative_markers();
+    for (int k = 0; k < gh->initMarkers.count; k++) gh->initMarkers.items[k]->visible = markers_visible;
+}
```

- [ ] **Step 2: Compile only game.c**

Run: `cc -std=c11 -Wall -Wextra -D_POSIX_C_SOURCE=200809L $(pkg-config --cflags sdl3 vulkan) -Ibuild -c src/game.c -o /tmp/game.o`
Expected: no output.

- [ ] **Step 3: Commit**

```bash
git add src/game.c
git commit -m "game: restore a saved initiative order"
```

---

### Task 8: Game ↔ save file bridge (`save.c`)

**Files:**
- Modify: `src/app.h`, `src/main.c`
- Create: `src/save.c`

What `save.c` does:

- **`gather()`** builds `SaveData` from `gh` and borrows image data from the textures. Images are de-duplicated by
  texture, and the background is lossy.
- **`apply()`** clears the table, then installs images, settings, entities, initiative and waypoints. It runs only
  after `savefmt_decode` has succeeded, so a failed load never touches the table.
- **The guard** compares BLAKE2b fingerprints taken at the last save or load.
- **Saving** writes `path.tmp`, `fsync`s it and renames it over the target. Freshly encoded WebP data is cached on
  the textures, so the next save doesn't re-encode it.

- [ ] **Step 1: Patch `src/app.h`**

Save the diff below as `/tmp/src_app.h.patch` and apply it from the project root with
`patch -p0 < /tmp/src_app.h.patch` (or make the same edits by hand):

```diff
--- src/app.h
+++ src/app.h
@@ -280,6 +280,8 @@
     ACTION_SPAWN_CHARACTERS,
     ACTION_WALLS,
     ACTION_CHARACTER_ICON,
+    ACTION_SAVE, /* save / load dialogs (save.c) */
+    ACTION_LOAD,
 } ActionKind;
 
 typedef struct {
@@ -388,6 +390,17 @@
 void game_next_turn(void);
 void game_clear_initiative(void);
 bool game_is_in_initiative(const Entity *e);
+/* Replaces the initiative list with entities[i] / values[i] in the given order. */
+void game_restore_initiative(Entity **entities, const double *values, int n, int current, bool turn_started,
+                             bool markers_visible);
+
+/* save.c - .pfsave files (format: savefmt.h) */
+void save_init(void);              /* call once after gfx_init: the empty table counts as saved */
+bool save_is_modified(void);       /* table differs from the last save / load */
+void save_command(const char *path);              /* :save [path]; NULL or "" = current file or dialog */
+void load_command(const char *path, bool force);  /* :load[!] [path] */
+void quit_command(bool force);                    /* :q / :q! */
+void save_handle_dialog(ActionKind kind, const char *path); /* path NULL = cancelled */
 
 /* input.c */
 void io_handle_event(const SDL_Event *e);
```

- [ ] **Step 2: Patch `src/main.c`**

Save the diff below as `/tmp/src_main.c.patch` and apply it from the project root with
`patch -p0 < /tmp/src_main.c.patch` (or make the same edits by hand):

```diff
--- src/main.c
+++ src/main.c
@@ -12,7 +12,7 @@
  *   @down KEY / @up KEY                  hold or release a key
  *   @type TEXT       typed text          @shot FILE  save a screenshot
  *   @wait N          idle N frames       @quit
- * --auto-file makes every "open image" dialog return the given file.
+ * --auto-file makes every file dialog (images, :save, :load) return the given file.
  */
 #include "app.h"
 #include "font.h"
@@ -182,6 +182,7 @@
     char resdir[512];
     find_resource_dir(resdir, sizeof(resdir));
     gfx_init(&app, resdir);
+    save_init();
     SDL_StartTextInput(app.window);
 
     Uint64 last = SDL_GetTicks();
```

- [ ] **Step 3: Create `src/save.c`**

```c
/* Saving and loading the table: :save, :load, the unsaved-changes guard of :q.
 * Converts between the game state and SaveData; the file format lives in savefmt.c. */
#include "app.h"
#include "savefmt.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

_Static_assert(SAVEFMT_NAME_MAX == WAYPOINT_NAME_MAX, "waypoint name sizes differ");
_Static_assert(SAVEFMT_MAX_SPEED == MAX_SPEED, "speed limits differ");

#define SAVE_SUFFIX ".pfsave"
#define PATH_MAX_LEN 1024

static char current_file[PATH_MAX_LEN]; /* "" = none yet */
static uint8_t saved_fp[SAVEFMT_TAG_SIZE];
static bool have_fp;

/* ---- game state -> SaveData ---------------------------------------------------------------- */

/* SaveData built from the game. Image data is borrowed from the textures. */
typedef struct {
    SaveData d;
    DA(Texture *) tex; /* tex.items[i] belongs to image i */
    DA(SaveImage) images;
} Gathered;

static uint32_t image_for(Gathered *g, Texture *t, uint8_t kind) {
    for (int i = 0; i < g->tex.count; i++)
        if (g->tex.items[i] == t) return (uint32_t)i;
    SaveImage im = { .kind = kind, .w = vkr_texture_width(t), .h = vkr_texture_height(t), .id = vkr_texture_id(t) };
    size_t len;
    int bkind, bw, bh;
    const uint8_t *blob = vkr_texture_blob(t, &len, &bkind, &bw, &bh);
    if (blob) {
        im.kind = (uint8_t)bkind;
        im.webp = blob;
        im.webp_len = len;
        im.w = bw;
        im.h = bh;
    } else {
        im.rgba = vkr_texture_pixels(t); /* NULL makes savefmt_encode fail with SAVE_ERR_INVALID */
    }
    da_push(&g->tex, t);
    da_push(&g->images, im);
    return (uint32_t)(g->tex.count - 1);
}

static int entity_index(const Entity *e) {
    for (int i = 0; i < gh->entities.count; i++)
        if (gh->entities.items[i] == e) return i;
    return -1;
}

static void gather(Gathered *g) {
    memset(g, 0, sizeof(*g));
    SaveData *d = &g->d;
    d->is_flat = gh->isFlat;
    d->bg_rows = (uint32_t)gh->backgroundRows;
    d->bg_cols = (uint32_t)gh->backgroundCols;
    d->offset_x = gh->gridOffsetX;
    d->offset_y = gh->gridOffsetY;
    d->hex_size = (uint32_t)gh->hexSize;
    snprintf(d->theme, sizeof(d->theme), "%s", gfx_theme()->name);
    if (gh->backgroundImage) {
        d->has_background = true;
        d->background = image_for(g, gh->backgroundImage, SAVE_IMG_LOSSY);
    }

    int n = gh->entities.count;
    d->entities = calloc(n ? (size_t)n : 1, sizeof(*d->entities));
    if (!d->entities) abort();
    d->nentities = (uint32_t)n;
    for (int i = 0; i < n; i++) {
        const Entity *e = gh->entities.items[i];
        SaveEntity *s = &d->entities[i];
        s->type = e->type == ENTITY_WALL ? SAVE_ENT_WALL : SAVE_ENT_CHARACTER;
        s->image = image_for(g, e->image, SAVE_IMG_LOSSLESS);
        s->row = e->tile.row;
        s->col = e->tile.col;
        s->size = e->size;
        s->max_health = e->maxHealth;
        s->health = e->health;
        s->attack = e->attack;
        s->ac = e->ac;
        s->speed = e->speed;
        s->initiative = e->initiative;
    }

    d->init = calloc(gh->init.count ? (size_t)gh->init.count : 1, sizeof(*d->init));
    if (!d->init) abort();
    for (int i = 0; i < gh->init.count; i++) {
        int idx = entity_index(gh->init.items[i].entity);
        if (idx < 0) continue; /* not expected: deleted entities leave the initiative */
        d->init[d->ninit++] = (SaveInit){ (uint32_t)idx, gh->init.items[i].value };
    }
    if (d->ninit > 0) {
        d->current_turn = (uint32_t)gh->currentTurnIndex < d->ninit ? (uint32_t)gh->currentTurnIndex : 0;
        d->turn_started = gh->currentTurn != NULL;
    }
    d->init_markers_visible = gh->initMarkers.count == 0 || gh->initMarkers.items[0]->visible;

    d->waypoints = calloc(gh->waypoints.count ? (size_t)gh->waypoints.count : 1, sizeof(*d->waypoints));
    if (!d->waypoints) abort();
    d->nwaypoints = (uint32_t)gh->waypoints.count;
    for (int i = 0; i < gh->waypoints.count; i++) {
        const Waypoint *w = &gh->waypoints.items[i];
        SaveWaypoint *s = &d->waypoints[i];
        snprintf(s->name, sizeof(s->name), "%s", w->name);
        s->row = w->tile.row;
        s->col = w->tile.col;
    }

    d->images = g->images.items;
    d->nimages = (uint32_t)g->images.count;
}

static void gathered_free(Gathered *g) {
    free(g->d.entities);
    free(g->d.init);
    free(g->d.waypoints);
    da_free(&g->tex);
    da_free(&g->images);
}

/* ---- unsaved changes ---------------------------------------------------------------------- */

static bool current_fingerprint(uint8_t out[SAVEFMT_TAG_SIZE]) {
    Gathered g;
    gather(&g);
    bool ok = savefmt_fingerprint(&g.d, out);
    gathered_free(&g);
    return ok;
}

static void remember_fingerprint(void) { have_fp = current_fingerprint(saved_fp); }

void save_init(void) { remember_fingerprint(); }

bool save_is_modified(void) {
    uint8_t fp[SAVEFMT_TAG_SIZE];
    if (!have_fp || !current_fingerprint(fp)) return true;
    return memcmp(fp, saved_fp, sizeof(fp)) != 0;
}

/* ---- files -------------------------------------------------------------------------------------- */

static void format_size(size_t n, char *out, size_t cap) {
    if (n < 1024) snprintf(out, cap, "%zu B", n);
    else if (n < 1024 * 1024) snprintf(out, cap, "%.1f KiB", n / 1024.0);
    else snprintf(out, cap, "%.1f MiB", n / (1024.0 * 1024.0));
}

/* Writes path.tmp and renames it over path, so an existing save survives a failed write. */
static bool write_atomic(const char *path, const uint8_t *data, size_t len) {
    char tmp[PATH_MAX_LEN + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        console_log("Could not write \"%s\": %s", path, strerror(errno));
        return false;
    }
    bool ok = fwrite(data, 1, len, f) == len && fflush(f) == 0 && fsync(fileno(f)) == 0;
    int err = errno;
    if (fclose(f) != 0 && ok) {
        ok = false;
        err = errno;
    }
    if (ok && rename(tmp, path) != 0) {
        ok = false;
        err = errno;
    }
    if (!ok) {
        remove(tmp);
        console_log("Could not write \"%s\": %s", path, strerror(err));
    }
    return ok;
}

static bool read_file(const char *path, uint8_t **out, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        console_log("Could not read \"%s\": %s", path, strerror(errno));
        return false;
    }
    long size = -1;
    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        console_log("Could not read \"%s\": %s", path, strerror(errno));
        fclose(f);
        return false;
    }
    if ((unsigned long)size > SAVEFMT_MAX_FILE) {
        console_log("%s", savefmt_strerror(SAVE_ERR_TOO_LARGE));
        fclose(f);
        return false;
    }
    uint8_t *buf = malloc(size ? (size_t)size : 1);
    if (!buf) abort();
    if (fread(buf, 1, (size_t)size, f) != (size_t)size || ferror(f)) {
        console_log("Could not read \"%s\": %s", path, strerror(errno));
        free(buf);
        fclose(f);
        return false;
    }
    fclose(f);
    *out = buf;
    *out_len = (size_t)size;
    return true;
}

/* ---- saving ------------------------------------------------------------------------------------- */

static void save_to(const char *path_in) {
    char path[PATH_MAX_LEN];
    size_t n = strlen(path_in), sl = strlen(SAVE_SUFFIX);
    bool has_suffix = n >= sl && strcmp(path_in + n - sl, SAVE_SUFFIX) == 0;
    if (snprintf(path, sizeof(path), "%s%s", path_in, has_suffix ? "" : SAVE_SUFFIX) >= (int)sizeof(path)) {
        console_log("Path is too long");
        return;
    }

    Gathered g;
    gather(&g);
    SaveBlob *blobs = calloc(g.d.nimages ? g.d.nimages : 1, sizeof(*blobs));
    if (!blobs) abort();
    uint8_t *file = NULL;
    size_t len = 0;
    SaveError err = savefmt_encode(&g.d, blobs, &file, &len);
    /* keep freshly encoded images on their textures: the next save doesn't re-encode them */
    for (uint32_t i = 0; i < g.d.nimages; i++) {
        if (!blobs[i].webp) continue;
        Texture *t = g.tex.items[i];
        if (blobs[i].w != vkr_texture_width(t) || blobs[i].h != vkr_texture_height(t))
            console_log("An image was scaled down to %dx%d to fit the save format", blobs[i].w, blobs[i].h);
        vkr_texture_set_blob(t, blobs[i].webp, blobs[i].len, g.d.images[i].kind, blobs[i].w, blobs[i].h);
        blobs[i].webp = NULL;
    }
    savefmt_blobs_free(blobs, g.d.nimages);
    free(blobs);
    gathered_free(&g);

    if (err != SAVE_OK) {
        console_log("Could not save: %s", savefmt_strerror(err));
        return;
    }
    if (write_atomic(path, file, len)) {
        snprintf(current_file, sizeof(current_file), "%s", path);
        remember_fingerprint();
        char size[32];
        format_size(len, size, sizeof(size));
        console_log("Saved \"%s\" (%s)", path, size);
    }
    free(file);
}

/* ---- loading ------------------------------------------------------------------------------------ */

static void clear_table(void) {
    entities_delete_type(2); /* all entities; also removes them from the initiative */
    game_clear_initiative();
    da_clear(&gh->waypoints);
    vkr_texture_release(gh->backgroundImage);
    gh->backgroundImage = NULL;
    da_clear(&gh->selectedTiles);
    da_clear(&gh->selectedEntityTiles);
    da_clear(&gh->entityPreviewTiles);
    da_clear(&gh->attackTiles);
    da_clear(&gh->path);
    ts_free(&gh->entityRangeTiles);
    gh->hasSelectedEntity = false;
    da_clear(&gh->measures);
    io_update_total_length();
    gh->hasLineAttack = gh->hasAreaAttack = gh->hasConeAttack = false;
    gh->hasTileUnderMouse = false;
    gh->hasDragStart = false;
}

/* d was fully validated by savefmt_decode, so nothing here can fail. */
static void apply(SaveData *d) {
    clear_table();

    Texture **tex = calloc(d->nimages ? d->nimages : 1, sizeof(*tex));
    if (!tex) abort();
    for (uint32_t i = 0; i < d->nimages; i++) {
        SaveImage *im = &d->images[i];
        tex[i] = vkr_texture_create(im->w, im->h, im->rgba, false);
        /* the WebP data moves to the texture: saving again won't re-encode it */
        vkr_texture_set_blob(tex[i], (uint8_t *)im->webp, im->webp_len, im->kind, im->w, im->h);
        im->webp = NULL;
        im->webp_len = 0;
    }

    gh->isFlat = d->is_flat;
    gh->backgroundRows = (int)d->bg_rows;
    gh->backgroundCols = (int)d->bg_cols;
    gh->hexSize = (int)d->hex_size;
    gh->thickness = gh->hexSize / 30 > 1 ? gh->hexSize / 30 : 1;
    gh->gridOffsetX = d->offset_x;
    gh->gridOffsetY = d->offset_y;
    gfx_change_theme(d->theme); /* logs a note if the theme doesn't exist */
    if (d->has_background) {
        gh->backgroundImage = tex[d->background];
        vkr_texture_retain(gh->backgroundImage);
    }

    for (uint32_t i = 0; i < d->nentities; i++) {
        const SaveEntity *s = &d->entities[i];
        Hex tile = { s->row, s->col };
        Entity *e;
        if (s->type == SAVE_ENT_WALL) {
            e = entity_new_wall(tex[s->image], tile);
        } else {
            e = entity_new_character(tex[s->image], tile, s->size, s->max_health, s->ac, s->speed, s->initiative);
            e->health = s->health;
            e->attack = s->attack;
        }
        da_push(&gh->entities, e);
    }

    Entity **init_entities = calloc(d->ninit ? d->ninit : 1, sizeof(*init_entities));
    double *init_values = calloc(d->ninit ? d->ninit : 1, sizeof(*init_values));
    if (!init_entities || !init_values) abort();
    for (uint32_t i = 0; i < d->ninit; i++) {
        init_entities[i] = gh->entities.items[d->init[i].entity];
        init_values[i] = d->init[i].value;
    }
    game_restore_initiative(init_entities, init_values, (int)d->ninit, (int)d->current_turn, d->turn_started,
                            d->init_markers_visible);
    free(init_entities);
    free(init_values);

    for (uint32_t i = 0; i < d->nwaypoints; i++) {
        Waypoint w = { .tile = { d->waypoints[i].row, d->waypoints[i].col } };
        snprintf(w.name, sizeof(w.name), "%s", d->waypoints[i].name);
        da_push(&gh->waypoints, w);
    }

    /* entities and the background hold their own references */
    for (uint32_t i = 0; i < d->nimages; i++) vkr_texture_release(tex[i]);
    free(tex);
    gfx_out_of_bounds_correction();
}

static void load_from(const char *path) {
    uint8_t *buf;
    size_t len;
    if (!read_file(path, &buf, &len)) return;
    SaveData d;
    SaveError err = savefmt_decode(buf, len, &d);
    free(buf);
    if (err != SAVE_OK) {
        console_log("Could not load \"%s\": %s", path, savefmt_strerror(err));
        savefmt_free(&d);
        return;
    }
    apply(&d);
    savefmt_free(&d);
    snprintf(current_file, sizeof(current_file), "%s", path);
    remember_fingerprint();
    console_log("Loaded \"%s\"", path);
}

/* ---- commands ------------------------------------------------------------------------------------ */

static const char *const NO_WRITE = "No write since last change (add ! to override)";

/* "~/x" -> "$HOME/x", as in nvim. */
static const char *expand_home(const char *path, char *out, size_t cap) {
    const char *home = getenv("HOME");
    if (path && path[0] == '~' && (path[1] == '/' || path[1] == '\0') && home) {
        snprintf(out, cap, "%s%s", home, path + 1);
        return out;
    }
    return path;
}

void save_command(const char *path) {
    char buf[PATH_MAX_LEN * 2];
    path = expand_home(path, buf, sizeof(buf));
    if (path && *path) save_to(path);
    else if (current_file[0]) save_to(current_file);
    else gfx_request_file(ACTION_SAVE, NULL, 0, 0, 0, 0, 0);
}

void load_command(const char *path, bool force) {
    if (!force && save_is_modified()) {
        console_log("%s", NO_WRITE);
        return;
    }
    char buf[PATH_MAX_LEN * 2];
    path = expand_home(path, buf, sizeof(buf));
    if (path && *path) load_from(path);
    else gfx_request_file(ACTION_LOAD, NULL, 0, 0, 0, 0, 0);
}

void quit_command(bool force) {
    if (!force && save_is_modified()) {
        console_log("%s", NO_WRITE);
        return;
    }
    gh->running = false;
}

void save_handle_dialog(ActionKind kind, const char *path) {
    if (!path) {
        console_log(kind == ACTION_SAVE ? "Save canceled" : "Load canceled");
        return;
    }
    if (kind == ACTION_SAVE) save_to(path);
    else load_from(path);
}
```

- [ ] **Step 4: Build**

Run: `make 2>&1 | grep -E "warning|error"`
Expected: no output.

- [ ] **Step 5: Commit**

```bash
git add src/app.h src/main.c src/save.c
git commit -m "save: gather/apply game state, :save/:load logic, unsaved-changes guard"
```

---

### Task 9: Kept pixels for user images, save/load dialogs

**Files:**
- Modify: `src/graphics.c`

What the patch changes:

- **`texture_from_image`** now uses `vkr_texture_create_kept`. That covers every user image: background,
  characters, walls and `:c icon`.
- **`gfx_request_file`** opens a save dialog for `ACTION_SAVE` and a `*.pfsave` open dialog for `ACTION_LOAD`.
- **`gfx_handle_file_result`** passes those two kinds on to `save_handle_dialog` before the PNG handling.

- [ ] **Step 1: Patch `src/graphics.c`**

Save the diff below as `/tmp/src_graphics.c.patch` and apply it from the project root with
`patch -p0 < /tmp/src_graphics.c.patch` (or make the same edits by hand):

```diff
--- src/graphics.c
+++ src/graphics.c
@@ -221,12 +221,20 @@
         gfx_handle_file_result(gh->autoFile);
         return;
     }
+    static const SDL_DialogFileFilter save_filters[] = { { "Pathfinder saves (*.pfsave)", "pfsave" } };
     static const SDL_DialogFileFilter filters[] = { { "PNG Images (*.png)", "png" } };
-    SDL_ShowOpenFileDialog(dialog_callback, NULL, gh->window, filters, 1, NULL, false);
+    if (kind == ACTION_SAVE)
+        SDL_ShowSaveFileDialog(dialog_callback, NULL, gh->window, save_filters, 1, NULL);
+    else if (kind == ACTION_LOAD)
+        SDL_ShowOpenFileDialog(dialog_callback, NULL, gh->window, save_filters, 1, NULL, false);
+    else
+        SDL_ShowOpenFileDialog(dialog_callback, NULL, gh->window, filters, 1, NULL, false);
 }
 
+/* User images keep their pixels on the CPU: save files need them. */
 static Texture *texture_from_image(Image *img) {
-    Texture *t = vkr_texture_create(img->w, img->h, img->px, false);
+    Texture *t = vkr_texture_create_kept(img->w, img->h, img->px);
+    img->px = NULL; /* now owned by the texture */
     image_free(img);
     return t;
 }
@@ -236,6 +244,10 @@
     ActionKind kind = a->kind;
     a->kind = ACTION_NONE;
     if (kind == ACTION_NONE) return;
+    if (kind == ACTION_SAVE || kind == ACTION_LOAD) {
+        save_handle_dialog(kind, path);
+        return;
+    }
     if (!path) {
         if (kind == ACTION_SPAWN_CHARACTERS) console_log("Character summon canceled");
         else if (kind == ACTION_WALLS) console_log("Wall summon canceled");
```

- [ ] **Step 2: Build and run the existing smoke test**

Run: `make 2>&1 | grep -E "warning|error"; ./pathfinder --auto-file resources/images/toolIcons/area.png --exec "@click 300 200" --exec ":c" --exec "@shot /tmp/pf-smoke.png" --exec "@quit"`
Expected: no build output; then `Saved screenshot to /tmp/pf-smoke.png`, and the screenshot shows a character.

- [ ] **Step 3: Commit**

```bash
git add src/graphics.c
git commit -m "graphics: keep user image pixels, add save/load dialogs"
```

---

### Task 10: Console commands

**Files:**
- Modify: `src/console.c`

What the patch adds:

- **Commands:** `save`/`:save`, `load`/`:load`, `load!`/`:load!`, `quit!`/`:q!`; `:q` goes through `quit_command`.
- **Paths:** everything after the command word is the path, so spaces are allowed.
- **Help and completion:** help text and tab completion for `save` and `load`.

- [ ] **Step 1: Patch `src/console.c`**

Save the diff below as `/tmp/src_console.c.patch` and apply it from the project root with
`patch -p0 < /tmp/src_console.c.patch` (or make the same edits by hand):

```diff
--- src/console.c
+++ src/console.c
@@ -19,7 +19,7 @@
 static const char *const ARGS_CHARACTER[] = { "delete", "size", "maxhealth", "armorclass", "speed", "initiative", "icon", NULL };
 static const char *const ARGS_DELETE[]    = { "delete", NULL };
 static const char *const ARGS_HELP[]      = { "quit", "background", "debug", "character", "wall", "entity", "help",
-                                              "roll", "grid", "gamemaster", "init", "clear", "theme", "teleport", NULL };
+                                              "roll", "grid", "gamemaster", "init", "clear", "theme", "teleport", "save", "load", NULL };
 static const char *const ARGS_INIT[]      = { "add", "remove", "clear", "show", "next", NULL };
 static const char *const ARGS_THEME[]     = { "purple", "red", "green", "yellow", "light", "dark", "black", NULL };
 
@@ -31,6 +31,7 @@
     { "theme", ARGS_THEME },
     /* arguments depend on the position and on the existing waypoints: see teleport_suggestions */
     { "teleport", NULL },    { "tp", NULL },
+    { "save", NULL },        { "load", NULL },
 };
 #define NCOMMANDS ((int)(sizeof(COMMANDS) / sizeof(COMMANDS[0])))
 
@@ -415,7 +416,7 @@
 static void cmd_help(Args *a) {
     const char *arg = a->argc > 1 ? a->argv[1] : "";
     if (!strcmp(arg, "quit") || !strcmp(arg, ":q")) {
-        console_log(":q  or quit # Quit the application - No Arguments");
+        console_log(":q  or quit # Quit the application - :q! also quits with unsaved changes");
     } else if (!strcmp(arg, "background") || !strcmp(arg, ":b")) {
         console_log(":b  or background [file.png] # Set a background image (opens a file dialog)");
     } else if (!strcmp(arg, "debug") || !strcmp(arg, ":d")) {
@@ -450,6 +451,12 @@
         console_log("  tp waypoint # list waypoints");
         console_log("  tp waypoint create <name> [row col] # default: selected tile, else screen centre");
         console_log("  tp waypoint delete <name>");
+    } else if (!strcmp(arg, "save") || !strcmp(arg, ":save")) {
+        console_log(":save [file] # Save the table as .pfsave");
+        console_log("  without a file: save to the current file, or choose one in a dialog");
+    } else if (!strcmp(arg, "load") || !strcmp(arg, ":load")) {
+        console_log(":load [file] # Load a saved table (opens a dialog without a file)");
+        console_log("  :load! [file] discards unsaved changes");
     } else {
         console_log("Available commands:");
         console_log(":q  or quit       # Quit the application");
@@ -466,6 +473,8 @@
         console_log(":cl or clear      # Clear the command log and history");
         console_log(":t  or theme      # Show or choose a theme");
         console_log("tp  or teleport   # Jump to a tile or waypoint");
+        console_log(":save [file]      # Save the table");
+        console_log(":load[!] [file]   # Load a saved table");
     }
 }
 
@@ -665,6 +674,17 @@
     console_log("Expected coordinates as <row> <col> or a waypoint name");
 }
 
+/* Everything after the first word, without surrounding blanks: file paths may contain spaces. */
+static const char *rest_of_line(const char *input, char *out, size_t cap) {
+    const char *p = input;
+    while (isspace((unsigned char)*p)) p++;
+    while (*p && !isspace((unsigned char)*p)) p++;
+    while (isspace((unsigned char)*p)) p++;
+    snprintf(out, cap, "%s", p);
+    for (size_t n = strlen(out); n > 0 && isspace((unsigned char)out[n - 1]); n--) out[n - 1] = '\0';
+    return out;
+}
+
 static bool ends_with(const char *s, const char *suffix) {
     size_t a = strlen(s), b = strlen(suffix);
     return a >= b && strcmp(s + a - b, suffix) == 0;
@@ -701,7 +721,12 @@
     snprintf(cmd, sizeof(cmd), "%s", a.argv[0]);
     lower(cmd);
 
-    if      (!strcmp(cmd, "quit") || !strcmp(cmd, ":q"))        gh->running = false;
+    char rest[CONSOLE_TEXT_MAX];
+    if      (!strcmp(cmd, "quit") || !strcmp(cmd, ":q"))        quit_command(false);
+    else if (!strcmp(cmd, "quit!") || !strcmp(cmd, ":q!"))      quit_command(true);
+    else if (!strcmp(cmd, "save") || !strcmp(cmd, ":save"))     save_command(rest_of_line(input, rest, sizeof(rest)));
+    else if (!strcmp(cmd, "load") || !strcmp(cmd, ":load"))     load_command(rest_of_line(input, rest, sizeof(rest)), false);
+    else if (!strcmp(cmd, "load!") || !strcmp(cmd, ":load!"))   load_command(rest_of_line(input, rest, sizeof(rest)), true);
     else if (!strcmp(cmd, "background") || !strcmp(cmd, ":b"))  gfx_set_background_image(a.argc > 1 ? a.argv[1] : NULL);
     else if (!strcmp(cmd, "debug") || !strcmp(cmd, ":d"))       gh->debugMode = !gh->debugMode;
     else if (!strcmp(cmd, "character") || !strcmp(cmd, ":c"))   cmd_character(&a);
```

- [ ] **Step 2: Build**

Run: `make 2>&1 | grep -E "warning|error"`
Expected: no output.

- [ ] **Step 3: Quick manual check**

Run: `./pathfinder --auto-file resources/images/toolIcons/area.png --exec "@click 300 200" --exec ":c" --exec ":q" --exec ":save /tmp/pf-manual" --exec ":q"`
Expected: the first `:q` is refused (a character was added), and the program exits by itself after the second
`:q` (the table was just saved). `head -c4 /tmp/pf-manual.pfsave` prints `PFSV`; the file is about 11 KB.

- [ ] **Step 4: Commit**

```bash
git add src/console.c
git commit -m "console: :save, :load[!] and :q! commands"
```

---

### Task 11: End-to-end test

**Files:**
- Create: `tests/e2e_save.sh` (make it executable)

The script does the following:

1. Builds a table: background, a character, a wall, a waypoint and initiative.
2. Saves it, loads it, takes a screenshot (`loaded`).
3. Modifies the table (screenshot `modified`).
4. Checks that `:q` and `:load` are refused (screenshots `quit-refused` and `load-refused`).
5. Runs `:load!` and takes a screenshot (`reloaded`).
6. Expects the last `:q` to quit on its own; `timeout` fails the test if it doesn't.

Screenshots are compared above the console only, because the log text differs between shots. Set `KEEP=<dir>` to
keep the screenshots for inspection.

- [ ] **Step 1: Create `tests/e2e_save.sh`**

```bash
#!/usr/bin/env bash
# End-to-end check of :save, :load and the unsaved-changes guard of :q.
# Opens a window (needs Wayland or X11) and uses ImageMagick to compare screenshots.
# Usage: tests/e2e_save.sh   (or: make e2e)
set -euo pipefail
cd "$(dirname "$0")/.."

out=${KEEP:-$(mktemp -d)}
[ -n "${KEEP:-}" ] || trap 'rm -rf "$out"' EXIT
icon=resources/images/toolIcons/area.png
bg=resources/images/toolIcons/cone.png
fail() { echo "FAIL: $*" >&2; exit 1; }

# Normalized RMSE between two screenshots (0 = identical). Only the top 70% is compared:
# the console log at the bottom shows different messages in each shot.
rmse() {
    magick "$1" "$2" -gravity North -crop 100%x70%+0+0 +repage -metric RMSE -compare -format '%[distortion]' info:
}
similar() { awk -v v="$(rmse "$1" "$2")" 'BEGIN { exit !(v < 0.005) }'; }

# The final :q must quit by itself (table unchanged since :load!); otherwise timeout fails the test.
timeout 60 ./pathfinder --size 600x400 --auto-file "$icon" \
    --exec ":b $bg" \
    --exec "@click 300 200" --exec ":c" \
    --exec "@click 380 260" --exec ":w" \
    --exec "tp waypoint create camp 2 3" \
    --exec ":i" \
    --exec ":save $out/table" \
    --exec ":load $out/table.pfsave" \
    --exec "@shot $out/loaded.png" \
    --exec ":c delete" --exec ":g" \
    --exec "@shot $out/modified.png" \
    --exec ":q" \
    --exec "@shot $out/quit-refused.png" \
    --exec ":load $out/table.pfsave" \
    --exec "@shot $out/load-refused.png" \
    --exec ":load! $out/table.pfsave" \
    --exec "@shot $out/reloaded.png" \
    --exec ":q" \
    || fail "pathfinder exited with status $? (124 = the last :q did not quit)"

[ "$(head -c 4 "$out/table.pfsave")" = PFSV ] || fail "table.pfsave has no PFSV header"
[ -f "$out/quit-refused.png" ] || fail ":q quit despite unsaved changes"
similar "$out/modified.png" "$out/load-refused.png" || fail ":load replaced a modified table"
similar "$out/loaded.png" "$out/reloaded.png" || fail ":load! did not restore the saved table"
! similar "$out/loaded.png" "$out/modified.png" || fail "the modification was not visible (test is not sensitive)"

echo "e2e save/load: OK ($(stat -c %s "$out/table.pfsave") byte save file)"
```

- [ ] **Step 2: Make it executable and run it**

Run: `chmod +x tests/e2e_save.sh && make e2e 2>&1 | tail -1`
Expected: `e2e save/load: OK (<about 24000> byte save file)`

- [ ] **Step 3: Run it against a sanitizer build**

Run: `make clean && make CFLAGS="-O1 -g -fsanitize=address,undefined" && ASAN_OPTIONS=detect_leaks=0 ./tests/e2e_save.sh 2>&1 | grep -E "ERROR|runtime error|OK|FAIL"; make clean && make`
Expected: only the `e2e save/load: OK` line. Leak detection is off because the Vulkan driver reports its own
allocations.

- [ ] **Step 4: Commit**

```bash
git add tests/e2e_save.sh
git commit -m "test: end-to-end save/load/quit-guard check"
```

---

### Task 12: Documentation

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Patch `README.md`**

Save the diff below as `/tmp/README.md.patch` and apply it from the project root with
`patch -p0 < /tmp/README.md.patch` (or make the same edits by hand):

```diff
--- README.md
+++ README.md
@@ -13,6 +13,7 @@
 | Vulkan loader + headers | rendering |
 | FreeType 2 + fontconfig | text (looks up "Arial", falls back to the system sans font) |
 | `glslc` (shaderc) | compiles `shaders/*.vert/frag` to SPIR-V at build time |
+| libwebp, libzstd, libsodium | save files: image compression, state compression, integrity tag |
 
 ```sh
 make            # builds ./pathfinder
@@ -20,6 +21,9 @@
 ```
 
 Sanitizer build: `make clean && make CFLAGS="-O1 -g -fsanitize=address,undefined"`.
+
+Tests: `make test` (save format unit tests), `make fuzz` (libFuzzer on the save file decoder, needs clang;
+`FUZZ_TIME=600` fuzzes longer) and `make e2e` (saves and loads in a real window, needs a display and ImageMagick).
 Set `PATHFINDER_VK_VALIDATION=1` to enable the Khronos validation layer when it is installed.
 
 ## Controls
@@ -47,7 +51,31 @@
 Console commands: `:q` quit, `:b [file.png]` background, `:d` debug, `:c` character
 (`delete`, `size n`, `maxhealth n`, `armorclass n`, `speed n`, `initiative n`, `icon`),
 `:w [delete]` wall, `:e delete`, `:r 2d6+3` roll, `:g` grid orientation, `:gm` gamemaster,
-`:i [add|remove|clear|show|next]` initiative, `:cl` clear, `:t [name]` theme, `:h [command]` help.
+`:i [add|remove|clear|show|next]` initiative, `:cl` clear, `:t [name]` theme, `:h [command]` help,
+`:save [file]` / `:load[!] [file]` save files (see below), `:q!` quit without saving.
+
+## Saving
+
+`:save file` writes the table to `file.pfsave` (the extension is added if missing) and remembers it as the
+current file; a later `:save` without a name writes there again, or opens a save dialog if there is no current file.
+`:load file` loads a save, `:load` opens a dialog. Paths may contain spaces, and `~/` means the home directory.
+
+Like nvim, `:q` and `:load` refuse to throw away unsaved changes: `No write since last change (add ! to override)`.
+Use `:q!` / `:load!` to discard them. Camera, zoom and theme are saved but don't count as changes. The window's
+close button always quits.
+
+A save contains the background, characters (image and stats), walls, the initiative order, waypoints, grid
+orientation, camera and theme; not the console, selections, clipboard or measurements.
+
+The format (`src/savefmt.h`) is binary and compact: the game state is packed with varints and zstd-compressed, and
+every image is stored once as WebP (lossless for characters and walls, quality 90 for the background; images over
+16383 px are scaled down). Saves are written to `file.tmp` first and then renamed, and a load only replaces the
+table once the whole file has been checked.
+
+Every file ends with a keyed BLAKE2b tag, so damaged or edited files are rejected ("Save file is damaged or was
+modified"). The key is part of the program, so the tag is no proof of who made a file; that needs a signature from
+a server, for which the format reserves a flag. Because of that, the loader treats every file as untrusted: all
+sizes, counts and references are checked against fixed limits before use.
 
 ## Source layout
 
```

- [ ] **Step 2: Run all tests one last time**

Run: `make && make test && make e2e`
Expected: `71 checks, 0 failed` and `e2e save/load: OK (...)`.

- [ ] **Step 3: Commit**

```bash
git add README.md
git commit -m "docs: document saving, save format and tests"
```

