# Multiplayer, Plan 1 of 4: Entity IDs, owners and the op layer — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every change to the table goes through one checked operation layer (`ops_submit` → `ops_check` → `ops_apply`), entities get permanent ids and an owner, and save files store owners. The game behaves exactly as before offline.

**Architecture:** A new module `src/ops.c` holds a tagged-union `Op`, a permission/rule checker that knows GM vs. player authors, and the only code that mutates table state. Input, console and dialog code build `Op`s instead of changing `App` directly. Offline the local author is always the GM, so nothing changes for the user; Plan 3 plugs the network in between `ops_check` and `ops_apply`. The game core (entity, game, hex, astar, ops) gets a unit test binary that runs without a window, using small stubs for the renderer and console.

**Tech Stack:** C11, SDL3 (types only in tests), libsodium / libzstd / libwebp (save format), make.

**Spec:** `docs/superpowers/specs/2026-11-21-multiplayer-design.md` (sections 3.1–3.4).

---

## Roadmap (all four plans)

| Plan | Delivers | Testable result |
|---|---|---|
| **1 (this one)** | entity ids + owners, save format v2, op layer, all mutations routed through ops, `:c owner` | `make test` (save + core tests), `make e2e`, the game behaves as before |
| 2 | `src/netmsg.c`: wire encoding of every message and op, join codes; fuzz target | `make test`, `make fuzz` |
| 3 | `src/net.c`, `src/portmap.c`, `src/session.c`: `:host`, `:join`, snapshot, op replication, roles, dice on host, claims | `make e2e` with a host and a client on localhost |
| 4 | presence: remote selections, names, movement previews, tools, colors | `make e2e` screenshot checks |

Plans 2–4 are written when the previous plan is merged, so they can refer to the real code.

## Before you start

The working tree has uncommitted changes from the rename "pathfinder" → "Tales of Honor (toh)". Ask the user to commit them (or allow you to) before Task 1, so this plan's commits contain only its own changes. Work on the `multiplayer` branch.

Build commands used throughout:

- `make` – builds `./toh`. Must compile **without warnings**.
- `make test` – unit tests. After Task 2 it runs `build/save_test` and `build/core_test`.
- `make e2e` – opens a window (needs Wayland/X11 and ImageMagick's `magick`).

## File structure

| File | Status | Responsibility |
|---|---|---|
| `src/savefmt.h`, `src/savefmt.c` | modify | format version 2: owner name per character, `savefmt_owner_valid` |
| `src/app.h` | modify | `Entity.id`, `Entity.owner`, `App.nextEntityId`, new prototypes |
| `src/entity.c` | modify | `entity_register`, `entity_by_id`, `entity_delete`, `entities_clear_highlights` |
| `src/game.c` | modify | `game_initiative_list` takes explicit roll values |
| `src/ops.h`, `src/ops.c` | create | `Op`, `OpAuthor`, `OpResult`, `ops_check`, `ops_apply`, `ops_submit` |
| `src/graphics.c` | modify | `gfx_set_background_texture`; file dialog results become ops |
| `src/input.c` | modify | move and paste become ops |
| `src/console.c` | modify | delete, stats, owner, initiative, waypoints and grid become ops; `:c owner` |
| `src/save.c` | modify | save/load owners, register loaded entities |
| `tests/save_test.c` | modify | owner tests, version 2 |
| `tests/core_stubs.c` | create | renderer/console/graphics stand-ins for the core tests |
| `tests/core_test.c` | create | entity id, rule and apply tests |
| `tests/e2e_save.sh` | modify | owner changes count as unsaved changes |
| `Makefile` | modify | `build/core_test`, `make test` runs it |
| `README.md` | modify | `:c owner`, save format v2 |

---

### Task 1: Save format version 2 with character owners

**Files:**
- Modify: `src/savefmt.h`
- Modify: `src/savefmt.c`
- Test: `tests/save_test.c`

- [ ] **Step 1: Write the failing tests**

In `tests/save_test.c`, in `sample_init`, directly after the line
`s->entities[2] = (SaveEntity){ .type = SAVE_ENT_WALL, .image = 1, .row = 5, .col = 5 };` add:

```c
    snprintf(s->entities[0].owner, SAVEFMT_OWNER_MAX, "Alice");
```

In `test_roundtrip`, after the line `CHECK(d.nentities == 3 && memcmp(d.entities, o->entities, sizeof(s.entities)) == 0);` add:

```c
    CHECK(strcmp(d.entities[0].owner, "Alice") == 0 && d.entities[1].owner[0] == '\0');
```

In `test_header_errors`, replace

```c
    file[4] = 2; /* version 2 */
```

with

```c
    file[4] = SAVEFMT_VERSION + 1; /* from the future */
```

and replace

```c
    file[4] = 1;
    file[6] = 1; /* signature flag: reserved in v1 */
```

with

```c
    file[4] = SAVEFMT_VERSION;
    file[6] = 1; /* signature flag: reserved */
```

Replace the whole `wrap` function with a version-aware one plus a wrapper (the existing callers keep writing version 1 files, which keeps all hand-built v1 tests meaningful):

```c
/* Wraps a raw state section and raw image section (count + records) into a tagged file. */
static uint8_t *wrap_v(const W *state, const W *images, size_t *len, int content_size_flag, uint8_t version) {
    ZSTD_CCtx *cc = ZSTD_createCCtx();
    ZSTD_CCtx_setParameter(cc, ZSTD_c_contentSizeFlag, content_size_flag);
    size_t bound = ZSTD_compressBound(state->n);
    uint8_t *z = malloc(bound);
    size_t zn = ZSTD_compress2(cc, z, bound, state->b, state->n);
    ZSTD_freeCCtx(cc);
    size_t total = SAVEFMT_HEADER_SIZE + zn + images->n + SAVEFMT_TAG_SIZE;
    uint8_t *f = malloc(total);
    memcpy(f, "TUHH\x01\x00\x00\x00", 8);
    f[4] = version;
    for (int i = 0; i < 4; i++) f[8 + i] = (uint8_t)(zn >> (8 * i));
    memcpy(f + 12, z, zn);
    memcpy(f + 12 + zn, images->b, images->n);
    retag(f, total);
    free(z);
    *len = total;
    return f;
}

static uint8_t *wrap(const W *state, const W *images, size_t *len, int content_size_flag) {
    return wrap_v(state, images, len, content_size_flag, 1);
}
```

In `test_fingerprint`, after the line `s.entities[2].row--;` add:

```c
    /* and so does a new owner */
    snprintf(s.entities[1].owner, SAVEFMT_OWNER_MAX, "Bob");
    CHECK(savefmt_fingerprint(&s.d, b));
    CHECK(memcmp(a, b, sizeof(a)) != 0);
    s.entities[1].owner[0] = '\0';
```

Add these two tests before `static int write_sample(const char *path)`:

```c
static void test_owner_names(void) {
    CHECK(savefmt_owner_valid("Alice"));
    CHECK(savefmt_owner_valid("Jörg_42"));
    CHECK(savefmt_owner_valid("abcdefghijklmnopqrstuvwx"));   /* 24 bytes */
    CHECK(!savefmt_owner_valid("abcdefghijklmnopqrstuvwxy")); /* 25 bytes */
    CHECK(!savefmt_owner_valid(""));
    CHECK(!savefmt_owner_valid("two words"));
    CHECK(!savefmt_owner_valid("tab\there"));
    CHECK(!savefmt_owner_valid("\xc3")); /* broken UTF-8 */

    Sample s;
    sample_init(&s);
    uint8_t *file = NULL;
    size_t len;
    snprintf(s.entities[0].owner, SAVEFMT_OWNER_MAX, "bad name");
    CHECK_ERR(savefmt_encode(&s.d, NULL, &file, &len), SAVE_ERR_INVALID);
    s.entities[0].owner[0] = '\0';
    snprintf(s.entities[2].owner, SAVEFMT_OWNER_MAX, "Alice"); /* walls have no owner */
    CHECK_ERR(savefmt_encode(&s.d, NULL, &file, &len), SAVE_ERR_INVALID);
    sample_free(&s);
}

/* one character record without (version 1) or with (version 2) an owner string */
static void wcharacter(W *w, const char *owner) {
    w8(w, SAVE_ENT_CHARACTER);
    wuv(w, 0);
    wsv(w, 1);
    wsv(w, 2);
    for (int i = 0; i < 7; i++) wsv(w, 0); /* size .. initiative */
    if (owner) wstr(w, owner);
}

static void test_owner_versions(void) {
    W v1 = { .n = 0 }, v2 = { .n = 0 }, bad = { .n = 0 }, im = { .n = 0 };
    wimage(&im, 8, 8);
    W *states[3] = { &v1, &v2, &bad };
    const char *owners[3] = { NULL, "Alice", "A B" };
    for (int i = 0; i < 3; i++) {
        wworld(states[i], 0);
        wuv(states[i], 1);
        wcharacter(states[i], owners[i]);
        wuv(states[i], 0); wuv(states[i], 0); /* no initiative, current turn 0 */
        wuv(states[i], 0);                    /* no waypoints */
    }
    SaveData d;
    size_t len;

    uint8_t *f = wrap_v(&v1, &im, &len, 1, 1);
    CHECK_ERR(savefmt_decode(f, len, &d), SAVE_OK);
    CHECK(d.nentities == 1 && d.entities[0].owner[0] == '\0');
    savefmt_free(&d);
    free(f);

    f = wrap_v(&v2, &im, &len, 1, 2);
    CHECK_ERR(savefmt_decode(f, len, &d), SAVE_OK);
    CHECK(d.nentities == 1 && strcmp(d.entities[0].owner, "Alice") == 0);
    savefmt_free(&d);
    free(f);

    /* a version 1 body in a version 2 file lacks the owner string */
    f = wrap_v(&v1, &im, &len, 1, 2);
    CHECK_ERR(savefmt_decode(f, len, &d), SAVE_ERR_CORRUPT);
    savefmt_free(&d);
    free(f);

    /* owner names follow savefmt_owner_valid */
    f = wrap_v(&bad, &im, &len, 1, 2);
    CHECK_ERR(savefmt_decode(f, len, &d), SAVE_ERR_CORRUPT);
    savefmt_free(&d);
    free(f);
}
```

In `main`, after `test_fingerprint();` add:

```c
    test_owner_names();
    test_owner_versions();
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `make test`
Expected: compile errors — `'SaveEntity' has no member named 'owner'`, `'SAVEFMT_OWNER_MAX' undeclared`, `implicit declaration of function 'savefmt_owner_valid'`.

- [ ] **Step 3: Implement version 2 in the header**

In `src/savefmt.h`:

In the comment at the top, replace the line

```c
 * .tuhh file format, version 1: encoding, decoding and validation.
```

with

```c
 * .tuhh file format, version 2: encoding, decoding and validation.
 * Version 2 adds an owner name after each character's stats; version 1 files still load (no owners).
```

Replace `#define SAVEFMT_VERSION     1` with `#define SAVEFMT_VERSION     2`.

After `#define SAVEFMT_NAME_MAX        32 /* waypoint names are shorter than this (= WAYPOINT_NAME_MAX) */` add:

```c
#define SAVEFMT_OWNER_MAX       25 /* owner names are shorter than this (= PLAYER_NAME_MAX) */
```

In `SaveEntity`, after `int32_t size, max_health, health, attack, ac, speed, initiative;` add:

```c
    char owner[SAVEFMT_OWNER_MAX]; /* characters only: player name, "" = no owner (version 2) */
```

Before `const char *savefmt_strerror(SaveError e);` add:

```c
/* Player names, used as character owners: 1..24 bytes of UTF-8 without spaces or control characters. */
bool savefmt_owner_valid(const char *name);
```

- [ ] **Step 4: Implement version 2 in the codec**

In `src/savefmt.c`, after the `get_str` function add:

```c
bool savefmt_owner_valid(const char *name) {
    size_t n = strnlen(name, SAVEFMT_OWNER_MAX);
    if (n == 0 || n >= SAVEFMT_OWNER_MAX || !utf8_valid(name, n)) return false;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = (uint8_t)name[i];
        if (c <= 0x20 || c == 0x7f) return false;
    }
    return true;
}
```

In `validate`, replace

```c
        if (e->type == SAVE_ENT_CHARACTER && e->speed > SAVEFMT_MAX_SPEED) return false;
```

with

```c
        if (e->type == SAVE_ENT_CHARACTER && e->speed > SAVEFMT_MAX_SPEED) return false;
        if (e->owner[0] != '\0' && (e->type != SAVE_ENT_CHARACTER || !savefmt_owner_valid(e->owner))) return false;
```

In `write_state`, replace

```c
            put_sv(b, e->initiative);
        }
```

with

```c
            put_sv(b, e->initiative);
            put_str(b, e->owner);
        }
```

Change the signature of `read_state` from
`static SaveError read_state(const uint8_t *p, size_t n, SaveData *d) {`
to
`static SaveError read_state(const uint8_t *p, size_t n, uint16_t version, SaveData *d) {`
and in it replace

```c
            e->initiative = get_sv(&r);
        } else if (e->type != SAVE_ENT_WALL) {
```

with

```c
            e->initiative = get_sv(&r);
            if (version >= 2) get_str(&r, e->owner, sizeof(e->owner));
        } else if (e->type != SAVE_ENT_WALL) {
```

In `savefmt_decode`, replace `: read_state(state, (size_t)size, out);` with `: read_state(state, (size_t)size, version, out);`.

- [ ] **Step 5: Run the tests to verify they pass**

Run: `make test`
Expected: last line `N checks, 0 failed` and exit status 0.

- [ ] **Step 6: Commit**

```bash
git add src/savefmt.h src/savefmt.c tests/save_test.c
git commit -m "Save format v2: store an owner name per character"
```

---

### Task 2: Entity ids, owners and a core test binary

**Files:**
- Modify: `src/app.h`
- Modify: `src/entity.c`
- Modify: `src/save.c`, `src/graphics.c`, `src/input.c` (only the `da_push(&gh->entities, …)` lines)
- Create: `tests/core_stubs.c`, `tests/core_test.c`
- Modify: `Makefile`

- [ ] **Step 1: Add the core test binary to the Makefile**

In `Makefile`, after the line `FUZZ_CC     ?= clang` add:

```make
# Game core tests: entity / game / grid / pathfinding / op logic without a window.
# tests/core_stubs.c replaces the renderer, console and graphics functions they call.
CORE_SRC    := src/entity.c src/game.c src/hex.c src/astar.c src/util.c src/savefmt.c
CORE_LIBS   := $(shell pkg-config --libs sdl3 $(SAVE_PKGS)) -lm
```

Replace

```make
test: build/save_test
	./build/save_test
```

with

```make
build/core_test: tests/core_test.c tests/core_stubs.c $(CORE_SRC) $(wildcard src/*.h) | build
	$(CC) $(TOH_CFLAGS) $(CFLAGS) -Isrc -o $@ tests/core_test.c tests/core_stubs.c $(CORE_SRC) $(CORE_LIBS)

test: build/save_test build/core_test
	./build/save_test
	./build/core_test
```

- [ ] **Step 2: Create the stubs**

Create `tests/core_stubs.c`:

```c
/* Stand-ins for the renderer, console and graphics functions that the game core calls,
 * so entity.c, game.c, hex.c, astar.c and ops.c can be tested without a window. */
#include "app.h"

#include <stdarg.h>
#include <stdio.h>

App *gh;

char stub_last_log[1024];
int stub_log_count;
Texture *stub_background;

void console_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(stub_last_log, sizeof(stub_last_log), fmt, ap);
    va_end(ap);
    stub_log_count++;
}

void vkr_texture_retain(Texture *t) { (void)t; }
void vkr_texture_release(Texture *t) { (void)t; }

void gfx_grid_offset(int dx, int dy) {
    (void)dx;
    (void)dy;
}
void gfx_out_of_bounds_correction(void) {}
void gfx_set_background_texture(Texture *t) { stub_background = t; }
void gfx_toggle_grid_orientation(void) { gh->isFlat = !gh->isFlat; }
```

- [ ] **Step 3: Write the failing entity id test**

Create `tests/core_test.c`:

```c
/* Tests for the game core without a window: entity ids, op rules (ops_check) and
 * op effects (ops_apply). Run with `make test`. Stubs: tests/core_stubs.c. */
#include "app.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

extern char stub_last_log[1024];
extern Texture *stub_background;

static int failures, checks;

#define CHECK(cond) do {                                                       \
        checks++;                                                              \
        if (!(cond)) {                                                         \
            failures++;                                                        \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                      \
    } while (0)

/* Textures are opaque and the stubs never touch them, so any distinct addresses do. */
static char tex_storage[2];
#define TEX_A ((Texture *)&tex_storage[0])
#define TEX_B ((Texture *)&tex_storage[1])

static App app;

static void reset_table(void) {
    gh = &app;
    entities_delete_type(2);
    game_clear_initiative();
    da_clear(&app.waypoints);
    app.hexSize = 40;
    app.isFlat = true;
}

static Entity *add_character(Hex tile, int speed, const char *owner) {
    Entity *e = entity_new_character(TEX_A, tile, SIZE_NORMAL, 10, 12, speed, 0);
    snprintf(e->owner, sizeof(e->owner), "%s", owner);
    entity_register(e);
    return e;
}

static Entity *add_wall(Hex tile) {
    Entity *e = entity_new_wall(TEX_B, tile);
    entity_register(e);
    return e;
}

/* ---- entity ids ------------------------------------------------------------------------ */

static void test_entity_ids(void) {
    reset_table();
    Entity *a = add_character((Hex){ 0, 0 }, 3, "");
    Entity *b = add_wall((Hex){ 0, 1 });
    CHECK(a->id != 0 && b->id != 0 && a->id != b->id);
    CHECK(entity_by_id(a->id) == a && entity_by_id(b->id) == b);
    CHECK(entity_by_id(0) == NULL);
    uint32_t old = b->id;
    entities_delete_type(1);
    CHECK(entity_by_id(old) == NULL && entity_by_id(a->id) == a);
    Entity *c = add_wall((Hex){ 0, 1 });
    CHECK(c->id != old); /* ids are never reused */
}

int main(void) {
    test_entity_ids();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
```

- [ ] **Step 4: Run it to verify it fails**

Run: `make build/core_test`
Expected: compile errors — `'Entity' has no member named 'owner'`, `implicit declaration of function 'entity_register'`, `'entity_by_id'`.

- [ ] **Step 5: Add ids and owners to the entity**

In `src/app.h`, directly before the line `typedef enum { ENTITY_CHARACTER, ENTITY_WALL } EntityType;`, add:

```c
/* Player names (character owners): 1..24 bytes, checked with savefmt_owner_valid. */
#define PLAYER_NAME_MAX 25
```

Replace

```c
typedef struct Entity {
    EntityType type;
```

with

```c
typedef struct Entity {
    uint32_t id;                 /* unique while the program runs (entity_register), never 0 */
    char owner[PLAYER_NAME_MAX]; /* characters: player who controls it, "" = nobody (only the GM) */
    EntityType type;
```

After the prototype `Entity *entity_at(Hex h);` add:

```c
/* Gives e the next id and appends it to gh->entities. Every entity on the table goes through this. */
void    entity_register(Entity *e);
Entity *entity_by_id(uint32_t id); /* NULL if there is none */
/* Removes e from the table and the initiative and frees it. */
void    entity_delete(Entity *e);
/* Drops selection, range, preview and path highlights (after entities were deleted). */
void    entities_clear_highlights(void);
```

In the `App` struct, after `EntityList entities;` add:

```c
    uint32_t nextEntityId; /* last id handed out by entity_register */
```

- [ ] **Step 6: Implement the entity functions**

In `src/entity.c`, replace the whole `entities_delete_type` function with:

```c
void entity_register(Entity *e) {
    e->id = ++gh->nextEntityId;
    da_push(&gh->entities, e);
}

Entity *entity_by_id(uint32_t id) {
    for (int i = 0; i < gh->entities.count; i++)
        if (gh->entities.items[i]->id == id) return gh->entities.items[i];
    return NULL;
}

void entity_delete(Entity *e) {
    if (game_is_in_initiative(e)) game_remove_from_initiative(e);
    da_remove_value(&gh->entities, e);
    entity_free(e);
}

void entities_clear_highlights(void) {
    da_clear(&gh->selectedEntityTiles);
    ts_free(&gh->entityRangeTiles);
    da_clear(&gh->entityPreviewTiles);
    da_clear(&gh->path);
    gh->hasSelectedEntity = false;
}

void entities_delete_type(int type) {
    bool deleted = false;
    for (int i = gh->entities.count - 1; i >= 0; i--) {
        Entity *e = gh->entities.items[i];
        bool match = (type == 0) ? e->type == ENTITY_CHARACTER
                   : (type == 1) ? e->type == ENTITY_WALL
                   : true;
        if (!match) continue;
        entity_delete(e);
        deleted = true;
    }
    if (deleted) entities_clear_highlights(); /* drop highlights that referred to the deleted entities */
}
```

- [ ] **Step 7: Register every entity**

Every place that adds to `gh->entities` must use `entity_register`. (Tasks 6 and 7 later replace the graphics/input ones with ops; this keeps every entity numbered in the meantime.)

In `src/graphics.c`, `gfx_handle_file_result`, replace

```c
            da_push(&gh->entities, entity_new_character(t, a->tiles.items[i], a->size, a->maxHealth,
                                                         a->ac, a->speed, a->initiative));
```

with

```c
            entity_register(entity_new_character(t, a->tiles.items[i], a->size, a->maxHealth,
                                                 a->ac, a->speed, a->initiative));
```

and replace

```c
        for (int i = 0; i < a->tiles.count; i++) da_push(&gh->entities, entity_new_wall(t, a->tiles.items[i]));
```

with

```c
        for (int i = 0; i < a->tiles.count; i++) entity_register(entity_new_wall(t, a->tiles.items[i]));
```

In `src/input.c`, `paste_entities`, replace

```c
        da_push(&gh->entities, entity_clone(en, true));
```

with

```c
        Entity *copy = entity_clone(en, true);
        copy->owner[0] = '\0'; /* pasted characters belong to nobody */
        entity_register(copy);
```

In `src/save.c`:

After `_Static_assert(SAVEFMT_MAX_SPEED == MAX_SPEED, "speed limits differ");` add:

```c
_Static_assert(SAVEFMT_OWNER_MAX == PLAYER_NAME_MAX, "owner name sizes differ");
```

In `gather`, after `s->initiative = e->initiative;` add:

```c
        if (e->type == ENTITY_CHARACTER) snprintf(s->owner, sizeof(s->owner), "%s", e->owner);
```

In `apply`, replace

```c
            e->health = s->health;
            e->attack = s->attack;
        }
        da_push(&gh->entities, e);
```

with

```c
            e->health = s->health;
            e->attack = s->attack;
            snprintf(e->owner, sizeof(e->owner), "%s", s->owner);
        }
        entity_register(e);
```

- [ ] **Step 8: Run the tests and the build**

Run: `make test && make`
Expected: both test binaries print `N checks, 0 failed`; `make` finishes with no warnings.

Run: `make e2e`
Expected: `e2e save/load: OK (… byte save file)`.

- [ ] **Step 9: Commit**

```bash
git add src/app.h src/entity.c src/save.c src/graphics.c src/input.c tests/core_stubs.c tests/core_test.c Makefile
git commit -m "Give entities permanent ids and an owner; core test binary"
```

---

### Task 3: Explicit initiative rolls

Ops must carry the initiative rolls, so the host decides them and every peer sorts the same way. `game_initiative_list` stops rolling by itself.

**Files:**
- Modify: `src/game.c`, `src/app.h`, `src/console.c`
- Test: `tests/core_test.c`

- [ ] **Step 1: Write the failing test**

In `tests/core_test.c`, before `int main(void)`, add:

```c
static void test_initiative_values(void) {
    reset_table();
    Entity *a = add_character((Hex){ 0, 0 }, 3, "");
    Entity *b = add_character((Hex){ 0, 4 }, 3, "");
    Entity *c = add_character((Hex){ 0, 8 }, 3, "");
    Entity *order[3] = { a, b, c };
    double values[3] = { 0.2, 0.9, 0.5 };
    game_initiative_list(order, values, 3);
    /* highest roll first */
    CHECK(gh->init.count == 3);
    CHECK(gh->init.items[0].entity == b && gh->init.items[1].entity == c && gh->init.items[2].entity == a);
    CHECK(gh->init.items[0].value == 0.9);
    /* entities already in the initiative keep their roll */
    double again[3] = { 0.99, 0.0, 0.0 };
    game_initiative_list(order, again, 3);
    CHECK(gh->init.count == 3 && gh->init.items[0].entity == b && gh->init.items[2].value == 0.2);
}
```

and in `main`, after `test_entity_ids();` add `test_initiative_values();`.

- [ ] **Step 2: Run it to verify it fails**

Run: `make build/core_test`
Expected: compile error — `too many arguments to function 'game_initiative_list'`.

- [ ] **Step 3: Implement**

In `src/app.h`, replace `void game_initiative_list(Entity **entities, int n);` with:

```c
/* Adds the entities that are not in the initiative yet, entities[i] with roll values[i], and sorts. */
void game_initiative_list(Entity **entities, const double *values, int n);
```

In `src/game.c`, delete the function `initiative_add` (the whole `static void initiative_add(Entity *e) { … }` block) and replace `game_initiative_list` with:

```c
void game_initiative_list(Entity **entities, const double *values, int n) {
    if (!entities) return;
    for (int i = 0; i < n; i++)
        if (entities[i] && !game_is_in_initiative(entities[i])) initiative_push(entities[i], values[i]);
    sort_initiative();
    game_update_initiative_markers();
}
```

In `src/console.c`, add this helper before `static void cmd_init(Args *a) {`:

```c
/* Rolls initiative for the entities that are not in it yet and adds them. */
static void roll_initiative(Entity **entities, int n) {
    double *values = calloc(n > 0 ? (size_t)n : 1, sizeof(*values));
    if (!values) abort();
    for (int i = 0; i < n; i++)
        if (!game_is_in_initiative(entities[i])) values[i] = rand_unit();
    game_initiative_list(entities, values, n);
    free(values);
}
```

and in `cmd_init` replace `game_initiative_list(gh->entities.items, gh->entities.count);` with
`roll_initiative(gh->entities.items, gh->entities.count);` and
`if (sel.count > 0) game_initiative_list(sel.items, sel.count);` with
`if (sel.count > 0) roll_initiative(sel.items, sel.count);`.

- [ ] **Step 4: Run the tests and the build**

Run: `make test && make`
Expected: `0 failed` twice, no compiler warnings.

- [ ] **Step 5: Commit**

```bash
git add src/app.h src/game.c src/console.c tests/core_test.c
git commit -m "Pass initiative rolls into game_initiative_list explicitly"
```

---

### Task 4: The op type and its rules (`ops_check`)

**Files:**
- Create: `src/ops.h`, `src/ops.c`
- Modify: `Makefile` (add `src/ops.c` to `CORE_SRC`)
- Test: `tests/core_test.c`

- [ ] **Step 1: Write the failing rule tests**

In `tests/core_test.c`, replace `#include "app.h"` with:

```c
#include "app.h"
#include "ops.h"
```

After the `CHECK` macro add:

```c
#define CHECK_R(expr, want) do {                                               \
        OpResult got_ = (expr);                                                \
        checks++;                                                              \
        if (got_ != (want)) {                                                  \
            failures++;                                                        \
            fprintf(stderr, "%s:%d: %s = \"%s\", expected \"%s\"\n", __FILE__, __LINE__, #expr, \
                    ops_result_text(got_), ops_result_text(want));             \
        }                                                                      \
    } while (0)
```

Before `int main(void)` add:

```c
/* ---- ops_check ------------------------------------------------------------------------- */

static const OpAuthor GM = { .gm = true };

static OpAuthor player(const char *name) {
    OpAuthor a = { .gm = false };
    snprintf(a.name, sizeof(a.name), "%s", name);
    return a;
}

/* the tile n steps from h in neighbour direction dir */
static Hex steps(Hex h, int dir, int n) {
    for (int i = 0; i < n; i++) h = hex_neighbor_raw(h, dir);
    return h;
}

static Op move_op(const Entity *e, Hex to) { return (Op){ .type = OP_MOVE, .id = e->id, .tile = to }; }

static void test_player_moves(void) {
    reset_table();
    Hex start = { 10, 10 };
    Entity *hero = add_character(start, 3, "Alice");
    Entity *orc = add_character(steps(start, 3, 4), 3, "");
    OpAuthor alice = player("Alice"), bob = player("Bob"), lower = player("alice"), nobody = player("");
    Op near = move_op(hero, steps(start, 0, 2));
    Op far = move_op(hero, steps(start, 0, 5));
    CHECK(hex_distance(start, near.tile) == 2 && hex_distance(start, far.tile) == 5);

    CHECK_R(ops_check(&near, &alice), OPR_OK);
    CHECK_R(ops_check(&near, &lower), OPR_OK); /* names compare case-insensitively */
    CHECK_R(ops_check(&near, &bob), OPR_NOT_YOURS);
    CHECK_R(ops_check(&far, &alice), OPR_OUT_OF_RANGE);
    CHECK_R(ops_check(&far, &GM), OPR_OK); /* the GM is not limited */

    Op orc_move = move_op(orc, steps(orc->tile, 0, 1));
    CHECK_R(ops_check(&orc_move, &alice), OPR_NOT_YOURS); /* unowned characters belong to the GM */
    CHECK_R(ops_check(&orc_move, &nobody), OPR_NOT_YOURS);
    CHECK_R(ops_check(&orc_move, &GM), OPR_OK);

    /* a wall blocks its tile */
    Hex blocked = steps(start, 0, 1);
    Entity *wall = add_wall(blocked);
    Op into_wall = move_op(hero, blocked);
    CHECK_R(ops_check(&into_wall, &alice), OPR_OUT_OF_RANGE);

    /* only characters move; unknown ids are rejected for everyone */
    Op wall_move = move_op(wall, start);
    CHECK_R(ops_check(&wall_move, &GM), OPR_NO_ENTITY);
    Op gone = { .type = OP_MOVE, .id = 9999, .tile = start };
    CHECK_R(ops_check(&gone, &GM), OPR_NO_ENTITY);
}

static void test_turns(void) {
    reset_table();
    Entity *hero = add_character((Hex){ 10, 10 }, 3, "Alice");
    Entity *orc = add_character((Hex){ 20, 20 }, 3, "Bob");
    OpAuthor alice = player("Alice"), bob = player("Bob");
    Op next = { .type = OP_INIT_NEXT };

    /* no initiative: free movement, but there is no turn to pass */
    Op hero_move = move_op(hero, hex_neighbor_raw(hero->tile, 0));
    CHECK_R(ops_check(&hero_move, &alice), OPR_OK);
    CHECK_R(ops_check(&next, &alice), OPR_NOT_YOUR_TURN);

    Entity *order[2] = { hero, orc };
    double values[2] = { 0.9, 0.1 };
    game_initiative_list(order, values, 2);
    /* initiative is set up, but the first turn has not started */
    CHECK_R(ops_check(&hero_move, &alice), OPR_NOT_YOUR_TURN);
    CHECK_R(ops_check(&next, &alice), OPR_NOT_YOUR_TURN);
    CHECK_R(ops_check(&next, &GM), OPR_OK);

    game_next_turn();
    Entity *cur = gh->currentTurn;
    CHECK(cur == hero || cur == orc);
    Entity *other = cur == hero ? orc : hero;
    OpAuthor cur_owner = cur == hero ? alice : bob, other_owner = cur == hero ? bob : alice;
    Op cur_move = move_op(cur, hex_neighbor_raw(cur->tile, 0));
    Op other_move = move_op(other, hex_neighbor_raw(other->tile, 0));
    CHECK_R(ops_check(&cur_move, &cur_owner), OPR_OK);
    CHECK_R(ops_check(&other_move, &other_owner), OPR_NOT_YOUR_TURN);
    CHECK_R(ops_check(&other_move, &GM), OPR_OK);
    CHECK_R(ops_check(&next, &cur_owner), OPR_OK);
    CHECK_R(ops_check(&next, &other_owner), OPR_NOT_YOUR_TURN);
}

static void test_gm_only(void) {
    reset_table();
    Entity *hero = add_character((Hex){ 10, 10 }, 3, "Alice");
    OpAuthor alice = player("Alice");
    Op ops[] = {
        { .type = OP_SPAWN },
        { .type = OP_DELETE },
        { .type = OP_SET_STATS },
        { .type = OP_SET_ICON, .image = TEX_A },
        { .type = OP_SET_BACKGROUND },
        { .type = OP_GRID_ORIENTATION },
        { .type = OP_WAYPOINT_SET, .name = "camp" },
        { .type = OP_WAYPOINT_DELETE, .name = "camp" },
        { .type = OP_INIT_ADD },
        { .type = OP_INIT_REMOVE },
        { .type = OP_INIT_CLEAR },
        { .type = OP_INIT_SHOW },
        { .type = OP_SET_OWNER, .name = "Alice" },
    };
    for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
        Op *op = &ops[i];
        if (op->type == OP_DELETE || op->type == OP_SET_STATS || op->type == OP_SET_ICON ||
            op->type == OP_INIT_ADD || op->type == OP_INIT_REMOVE || op->type == OP_SET_OWNER)
            da_push(&op->ids, hero->id);
        if (op->type == OP_INIT_ADD) da_push(&op->values, 0.5);
        CHECK_R(ops_check(op, &alice), OPR_GM_ONLY);
        CHECK_R(ops_check(op, &GM), OPR_OK);
        op_free(op);
    }
}

static void test_structure(void) {
    reset_table();
    Entity *hero = add_character((Hex){ 10, 10 }, 3, "");

    Op del = { .type = OP_DELETE };
    da_push(&del.ids, hero->id);
    da_push(&del.ids, 12345);
    CHECK_R(ops_check(&del, &GM), OPR_NO_ENTITY);
    op_free(&del);

    Op add = { .type = OP_INIT_ADD };
    da_push(&add.ids, hero->id);
    CHECK_R(ops_check(&add, &GM), OPR_INVALID); /* no roll for it */
    da_push(&add.values, NAN);
    CHECK_R(ops_check(&add, &GM), OPR_INVALID);
    add.values.items[0] = 0.5;
    CHECK_R(ops_check(&add, &GM), OPR_OK);
    op_free(&add);

    Op own = { .type = OP_SET_OWNER, .name = "two words" };
    da_push(&own.ids, hero->id);
    CHECK_R(ops_check(&own, &GM), OPR_INVALID);
    snprintf(own.name, sizeof(own.name), "Alice");
    CHECK_R(ops_check(&own, &GM), OPR_OK);
    own.name[0] = '\0';
    CHECK_R(ops_check(&own, &GM), OPR_OK); /* "" removes the owner */
    op_free(&own);

    Op wp = { .type = OP_WAYPOINT_SET };
    CHECK_R(ops_check(&wp, &GM), OPR_INVALID); /* no name */
    memset(wp.name, 'x', sizeof(wp.name));
    CHECK_R(ops_check(&wp, &GM), OPR_INVALID); /* not terminated */

    Op spawn = { .type = OP_SPAWN };
    da_push(&spawn.specs, ((EntitySpec){ .type = ENTITY_WALL, .image = NULL }));
    CHECK_R(ops_check(&spawn, &GM), OPR_INVALID); /* no image */
    op_free(&spawn);

    Op icon = { .type = OP_SET_ICON };
    da_push(&icon.ids, hero->id);
    CHECK_R(ops_check(&icon, &GM), OPR_INVALID); /* no image */
    op_free(&icon);

    Op bogus = { .type = OP_COUNT };
    CHECK_R(ops_check(&bogus, &GM), OPR_INVALID);
}
```

In `main`, after `test_initiative_values();` add:

```c
    test_player_moves();
    test_turns();
    test_gm_only();
    test_structure();
```

- [ ] **Step 2: Run it to verify it fails**

Run: `make build/core_test`
Expected: `fatal error: ops.h: No such file or directory`.

- [ ] **Step 3: Write `src/ops.h`**

```c
#ifndef TOH_OPS_H
#define TOH_OPS_H

/*
 * Every change to the shared table is an Op. ops_submit checks whether the local user
 * may do it (ops_check) and then applies it (ops_apply), the only code that changes
 * table state. Offline the local user is the GM. In a session (plan 3) clients send
 * their ops to the host, which checks them again and broadcasts the ones it applied.
 * Design: docs/superpowers/specs/2026-11-21-multiplayer-design.md, section 3.
 */

#include "app.h"

typedef enum {
    OP_MOVE,             /* id -> tile */
    OP_SPAWN,            /* specs; ops_apply stores the new entities' ids in ids */
    OP_DELETE,           /* ids */
    OP_SET_STATS,        /* ids; statMask says which of the values below change */
    OP_SET_ICON,         /* ids, image */
    OP_SET_BACKGROUND,   /* image, NULL = none */
    OP_GRID_ORIENTATION, /* toggles flat / pointy */
    OP_WAYPOINT_SET,     /* name, tile: creates the waypoint or moves it */
    OP_WAYPOINT_DELETE,  /* name */
    OP_INIT_ADD,         /* ids, values: one roll per id */
    OP_INIT_REMOVE,      /* ids */
    OP_INIT_CLEAR,
    OP_INIT_SHOW,        /* toggles the turn order markers */
    OP_INIT_NEXT,
    OP_SET_OWNER,        /* ids, name: player name, "" = no owner */
    OP_COUNT
} OpType;

enum { STAT_SIZE = 1, STAT_MAXHEALTH = 2, STAT_AC = 4, STAT_SPEED = 8, STAT_INITIATIVE = 16 };

/* An entity that OP_SPAWN creates. */
typedef struct {
    EntityType type;
    Texture *image; /* borrowed; the new entity takes its own reference */
    Hex tile;
    int size, maxHealth, health, attack, ac, speed, initiative; /* characters only */
} EntitySpec;

typedef struct Op {
    OpType type;
    uint32_t id; /* OP_MOVE */
    Hex tile;    /* OP_MOVE, OP_WAYPOINT_SET */
    DA(uint32_t) ids;
    DA(double) values;    /* OP_INIT_ADD */
    DA(EntitySpec) specs; /* OP_SPAWN */
    Texture *image;       /* OP_SET_ICON, OP_SET_BACKGROUND; borrowed */
    int statMask;         /* OP_SET_STATS */
    int size, maxHealth, ac, speed, initiative;
    char name[WAYPOINT_NAME_MAX]; /* OP_WAYPOINT_*: waypoint name, OP_SET_OWNER: player name */
} Op;

/* Who sends an op. */
typedef struct {
    bool gm;
    char name[PLAYER_NAME_MAX]; /* players: their name, matched against Entity.owner */
} OpAuthor;

typedef enum {
    OPR_OK,
    OPR_GM_ONLY,
    OPR_NOT_YOURS,
    OPR_NOT_YOUR_TURN,
    OPR_OUT_OF_RANGE,
    OPR_NO_ENTITY,
    OPR_INVALID,
} OpResult;

void       op_free(Op *op); /* frees the lists, not the borrowed textures */
EntitySpec op_spec_from_entity(const Entity *e);

OpAuthor    ops_local_author(void);
OpResult    ops_check(const Op *op, const OpAuthor *author);
void        ops_apply(Op *op); /* op must have passed ops_check */
/* Checks op as the local user and applies it. Logs the reason when it is refused. */
OpResult    ops_submit(Op *op);
const char *ops_result_text(OpResult r);

#endif
```

- [ ] **Step 4: Write `src/ops.c` with the rules**

```c
/* The op layer: rules for who may change what (ops_check) and the changes themselves (ops_apply). */
#include "ops.h"
#include "savefmt.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

_Static_assert(PLAYER_NAME_MAX <= WAYPOINT_NAME_MAX, "Op.name must hold player names");

void op_free(Op *op) {
    da_free(&op->ids);
    da_free(&op->values);
    da_free(&op->specs);
}

EntitySpec op_spec_from_entity(const Entity *e) {
    return (EntitySpec){ .type = e->type, .image = e->image, .tile = e->tile, .size = e->size,
                         .maxHealth = e->maxHealth, .health = e->health, .attack = e->attack, .ac = e->ac,
                         .speed = e->speed, .initiative = e->initiative };
}

/* Offline the local user is the game master. */
OpAuthor ops_local_author(void) { return (OpAuthor){ .gm = true }; }

const char *ops_result_text(OpResult r) {
    switch (r) {
    case OPR_OK:            return "OK";
    case OPR_GM_ONLY:       return "Only the GM can do that";
    case OPR_NOT_YOURS:     return "Not your character";
    case OPR_NOT_YOUR_TURN: return "It's not your character's turn";
    case OPR_OUT_OF_RANGE:  return "Out of range";
    case OPR_NO_ENTITY:     return "That entity no longer exists";
    case OPR_INVALID:       return "Invalid action";
    }
    return "Unknown error";
}

/* ---- ops_check --------------------------------------------------------------------------- */

static bool ids_exist(const Op *op) {
    for (int i = 0; i < op->ids.count; i++)
        if (!entity_by_id(op->ids.items[i])) return false;
    return true;
}

static bool is_owner(const Entity *e, const OpAuthor *a) {
    return e && e->owner[0] != '\0' && strcasecmp(e->owner, a->name) == 0;
}

/* Rules for everyone, the GM included: the op has to make sense for the current table. */
static OpResult check_structure(const Op *op) {
    if (!memchr(op->name, '\0', sizeof(op->name))) return OPR_INVALID;
    switch (op->type) {
    case OP_MOVE: {
        const Entity *e = entity_by_id(op->id);
        return e && e->type == ENTITY_CHARACTER ? OPR_OK : OPR_NO_ENTITY;
    }
    case OP_SPAWN:
        for (int i = 0; i < op->specs.count; i++) {
            const EntitySpec *s = &op->specs.items[i];
            if (!s->image || (s->type != ENTITY_CHARACTER && s->type != ENTITY_WALL)) return OPR_INVALID;
        }
        return OPR_OK;
    case OP_SET_ICON:
        if (!op->image) return OPR_INVALID;
        return ids_exist(op) ? OPR_OK : OPR_NO_ENTITY;
    case OP_INIT_ADD:
        if (op->values.count != op->ids.count) return OPR_INVALID;
        for (int i = 0; i < op->values.count; i++)
            if (!isfinite(op->values.items[i])) return OPR_INVALID;
        return ids_exist(op) ? OPR_OK : OPR_NO_ENTITY;
    case OP_SET_OWNER:
        if (op->name[0] != '\0' && !savefmt_owner_valid(op->name)) return OPR_INVALID;
        return ids_exist(op) ? OPR_OK : OPR_NO_ENTITY;
    case OP_DELETE:
    case OP_SET_STATS:
    case OP_INIT_REMOVE:
        return ids_exist(op) ? OPR_OK : OPR_NO_ENTITY;
    case OP_WAYPOINT_SET:
    case OP_WAYPOINT_DELETE:
        return op->name[0] != '\0' ? OPR_OK : OPR_INVALID;
    case OP_SET_BACKGROUND:
    case OP_GRID_ORIENTATION:
    case OP_INIT_CLEAR:
    case OP_INIT_SHOW:
    case OP_INIT_NEXT:
        return OPR_OK;
    case OP_COUNT:
        break;
    }
    return OPR_INVALID;
}

/* Players may move their own character (in range, on its turn) and pass their own turn. */
static OpResult check_player(const Op *op, const OpAuthor *a) {
    switch (op->type) {
    case OP_MOVE: {
        Entity *e = entity_by_id(op->id);
        if (!is_owner(e, a)) return OPR_NOT_YOURS;
        if (gh->init.count > 0 && gh->currentTurn != e) return OPR_NOT_YOUR_TURN;
        TileSet range = { 0 };
        astar_range(e->tile, e->speed, e, &range);
        bool in_range = ts_has(&range, op->tile);
        ts_free(&range);
        return in_range ? OPR_OK : OPR_OUT_OF_RANGE;
    }
    case OP_INIT_NEXT:
        return is_owner(gh->currentTurn, a) ? OPR_OK : OPR_NOT_YOUR_TURN;
    default:
        return OPR_GM_ONLY;
    }
}

OpResult ops_check(const Op *op, const OpAuthor *author) {
    OpResult r = check_structure(op);
    if (r != OPR_OK || author->gm) return r;
    return check_player(op, author);
}

/* ---- ops_apply ---------------------------------------------------------------------------- */

void ops_apply(Op *op) {
    (void)op; /* Task 5 */
}

OpResult ops_submit(Op *op) {
    OpAuthor me = ops_local_author();
    OpResult r = ops_check(op, &me);
    if (r != OPR_OK) {
        console_log("%s", ops_result_text(r));
        return r;
    }
    ops_apply(op);
    return OPR_OK;
}
```

- [ ] **Step 5: Add `ops.c` to the core sources**

In `Makefile`, replace

```make
CORE_SRC    := src/entity.c src/game.c src/hex.c src/astar.c src/util.c src/savefmt.c
```

with

```make
CORE_SRC    := src/ops.c src/entity.c src/game.c src/hex.c src/astar.c src/util.c src/savefmt.c
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `make test && make`
Expected: `0 failed` twice, no compiler warnings. (`ops.c` is also picked up by `make` through `$(wildcard src/*.c)`; nothing calls it yet.)

If `test_player_moves` fails on `hex_distance(start, far.tile) == 5`, the neighbour direction 0 does not run straight for this grid; use direction 1 for `near`/`far`/`blocked` instead and re-run.

- [ ] **Step 7: Commit**

```bash
git add src/ops.h src/ops.c Makefile tests/core_test.c
git commit -m "Add the op layer with GM / player rules (ops_check)"
```

---

### Task 5: Applying ops (`ops_apply`)

**Files:**
- Modify: `src/ops.c`, `src/graphics.c`, `src/app.h`
- Test: `tests/core_test.c`

- [ ] **Step 1: Write the failing tests**

In `tests/core_test.c`, before `int main(void)` add:

```c
/* ---- ops_apply ------------------------------------------------------------------------- */

static void test_apply_entities(void) {
    reset_table();
    Op spawn = { .type = OP_SPAWN };
    da_push(&spawn.specs, ((EntitySpec){ .type = ENTITY_CHARACTER, .image = TEX_A, .tile = { 5, 5 },
                                         .size = SIZE_NORMAL, .maxHealth = 20, .health = 7, .attack = 1,
                                         .ac = 14, .speed = 4, .initiative = 2 }));
    da_push(&spawn.specs, ((EntitySpec){ .type = ENTITY_WALL, .image = TEX_B, .tile = { 5, 6 } }));
    CHECK_R(ops_submit(&spawn), OPR_OK);
    CHECK(spawn.ids.count == 2 && gh->entities.count == 2);
    Entity *hero = entity_by_id(spawn.ids.items[0]);
    Entity *wall = entity_by_id(spawn.ids.items[1]);
    CHECK(hero && hero->type == ENTITY_CHARACTER && hero->image == TEX_A && hex_eq(hero->tile, (Hex){ 5, 5 }));
    CHECK(hero && hero->maxHealth == 20 && hero->health == 7 && hero->attack == 1 && hero->ac == 14 &&
          hero->speed == 4 && hero->initiative == 2);
    CHECK(wall && wall->type == ENTITY_WALL && wall->image == TEX_B && hex_eq(wall->tile, (Hex){ 5, 6 }));
    op_free(&spawn);
    if (!hero || !wall) return;

    Op mv = move_op(hero, (Hex){ 7, 5 });
    CHECK_R(ops_submit(&mv), OPR_OK);
    CHECK(hex_eq(hero->tile, (Hex){ 7, 5 }));

    /* only the masked stats change, speed is capped, walls are skipped */
    Op st = { .type = OP_SET_STATS, .statMask = STAT_SPEED | STAT_AC, .speed = 5000, .ac = 9, .maxHealth = 1 };
    da_push(&st.ids, hero->id);
    da_push(&st.ids, wall->id);
    CHECK_R(ops_submit(&st), OPR_OK);
    CHECK(hero->speed == MAX_SPEED && hero->ac == 9 && hero->maxHealth == 20);
    op_free(&st);

    Op icon = { .type = OP_SET_ICON, .image = TEX_B };
    da_push(&icon.ids, hero->id);
    CHECK_R(ops_submit(&icon), OPR_OK);
    CHECK(hero->image == TEX_B);
    op_free(&icon);

    Op own = { .type = OP_SET_OWNER, .name = "Alice" };
    da_push(&own.ids, hero->id);
    da_push(&own.ids, wall->id);
    CHECK_R(ops_submit(&own), OPR_OK);
    CHECK(strcmp(hero->owner, "Alice") == 0 && wall->owner[0] == '\0');
    own.name[0] = '\0';
    CHECK_R(ops_submit(&own), OPR_OK);
    CHECK(hero->owner[0] == '\0');
    op_free(&own);

    Op del = { .type = OP_DELETE };
    da_push(&del.ids, wall->id);
    uint32_t wall_id = wall->id;
    CHECK_R(ops_submit(&del), OPR_OK);
    CHECK(gh->entities.count == 1 && entity_by_id(wall_id) == NULL);
    op_free(&del);

    /* rejected ops change nothing and tell the user why */
    Op bad = { .type = OP_MOVE, .id = 4242, .tile = { 0, 0 } };
    CHECK_R(ops_submit(&bad), OPR_NO_ENTITY);
    CHECK(strcmp(stub_last_log, "That entity no longer exists") == 0);
}

static void test_apply_world(void) {
    reset_table();
    Op bg = { .type = OP_SET_BACKGROUND, .image = TEX_A };
    CHECK_R(ops_submit(&bg), OPR_OK);
    CHECK(stub_background == TEX_A);
    bg.image = NULL;
    CHECK_R(ops_submit(&bg), OPR_OK);
    CHECK(stub_background == NULL);

    bool flat = gh->isFlat;
    Op grid = { .type = OP_GRID_ORIENTATION };
    CHECK_R(ops_submit(&grid), OPR_OK);
    CHECK(gh->isFlat != flat);

    Op set = { .type = OP_WAYPOINT_SET, .tile = { 1, 2 }, .name = "Camp" };
    CHECK_R(ops_submit(&set), OPR_OK);
    CHECK(gh->waypoints.count == 1 && strcmp(gh->waypoints.items[0].name, "Camp") == 0);
    Op move = { .type = OP_WAYPOINT_SET, .tile = { 3, 4 }, .name = "camp" };
    CHECK_R(ops_submit(&move), OPR_OK); /* same waypoint: keeps its name, moves */
    CHECK(gh->waypoints.count == 1 && strcmp(gh->waypoints.items[0].name, "Camp") == 0);
    CHECK(hex_eq(gh->waypoints.items[0].tile, (Hex){ 3, 4 }));
    Op del = { .type = OP_WAYPOINT_DELETE, .name = "CAMP" };
    CHECK_R(ops_submit(&del), OPR_OK);
    CHECK(gh->waypoints.count == 0);
}

static void test_apply_initiative(void) {
    reset_table();
    Entity *a = add_character((Hex){ 0, 0 }, 3, "");
    Entity *b = add_character((Hex){ 0, 4 }, 3, "");
    Entity *c = add_character((Hex){ 0, 8 }, 3, "");
    Op add = { .type = OP_INIT_ADD };
    Entity *es[3] = { a, b, c };
    double vals[3] = { 0.2, 0.9, 0.5 };
    for (int i = 0; i < 3; i++) {
        da_push(&add.ids, es[i]->id);
        da_push(&add.values, vals[i]);
    }
    CHECK_R(ops_submit(&add), OPR_OK);
    CHECK(gh->init.count == 3 && gh->init.items[0].entity == b && gh->init.items[2].entity == a);
    op_free(&add);

    Op next = { .type = OP_INIT_NEXT };
    CHECK_R(ops_submit(&next), OPR_OK);
    CHECK(gh->currentTurn != NULL);

    Op rm = { .type = OP_INIT_REMOVE };
    da_push(&rm.ids, c->id);
    CHECK_R(ops_submit(&rm), OPR_OK);
    CHECK(gh->init.count == 2 && !game_is_in_initiative(c));
    op_free(&rm);

    /* deleting an entity also takes it out of the initiative */
    Op del = { .type = OP_DELETE };
    da_push(&del.ids, b->id);
    CHECK_R(ops_submit(&del), OPR_OK);
    CHECK(gh->init.count == 1 && gh->init.items[0].entity == a);
    op_free(&del);

    Op show = { .type = OP_INIT_SHOW };
    bool visible = gh->initMarkers.items[0]->visible;
    CHECK_R(ops_submit(&show), OPR_OK);
    CHECK(gh->initMarkers.items[0]->visible != visible);

    Op clear = { .type = OP_INIT_CLEAR };
    CHECK_R(ops_submit(&clear), OPR_OK);
    CHECK(gh->init.count == 0 && gh->currentTurn == NULL);
}
```

In `main`, after `test_structure();` add:

```c
    test_apply_entities();
    test_apply_world();
    test_apply_initiative();
```

- [ ] **Step 2: Run it to verify it fails**

Run: `make test`
Expected: `core_test` reports failing CHECKs in the three `test_apply_*` functions (for example `spawn.ids.count == 2 && gh->entities.count == 2`), because `ops_apply` does nothing yet.

- [ ] **Step 3: Add `gfx_set_background_texture`**

In `src/app.h`, after `void gfx_set_background_image(const char *path_or_null);` add:

```c
void gfx_set_background_texture(Texture *t); /* NULL = no background; takes its own reference */
```

In `src/graphics.c`, after the function `update_background_size` add:

```c
void gfx_set_background_texture(Texture *t) {
    vkr_texture_retain(t);
    vkr_texture_release(gh->backgroundImage);
    gh->backgroundImage = t;
    update_background_size();
}
```

- [ ] **Step 4: Implement `ops_apply`**

In `src/ops.c`, replace

```c
void ops_apply(Op *op) {
    (void)op; /* Task 5 */
}
```

with

```c
static void apply_spawn(Op *op) {
    da_clear(&op->ids);
    for (int i = 0; i < op->specs.count; i++) {
        const EntitySpec *s = &op->specs.items[i];
        Entity *e;
        if (s->type == ENTITY_WALL) {
            e = entity_new_wall(s->image, s->tile);
        } else {
            e = entity_new_character(s->image, s->tile, s->size, s->maxHealth, s->ac, s->speed, s->initiative);
            e->health = s->health;
            e->attack = s->attack;
        }
        entity_register(e);
        da_push(&op->ids, e->id);
    }
}

static void apply_delete(const Op *op) {
    bool deleted = false;
    for (int i = 0; i < op->ids.count; i++) {
        Entity *e = entity_by_id(op->ids.items[i]);
        if (!e) continue;
        entity_delete(e);
        deleted = true;
    }
    if (deleted) entities_clear_highlights();
}

/* The characters among op->ids; walls are skipped like in the Java version. */
static Entity *character_at(const Op *op, int i) {
    Entity *e = entity_by_id(op->ids.items[i]);
    return e && e->type == ENTITY_CHARACTER ? e : NULL;
}

static void apply_stats(const Op *op) {
    for (int i = 0; i < op->ids.count; i++) {
        Entity *e = character_at(op, i);
        if (!e) continue;
        if (op->statMask & STAT_SIZE)       e->size = op->size;
        if (op->statMask & STAT_MAXHEALTH)  e->maxHealth = op->maxHealth;
        if (op->statMask & STAT_AC)         e->ac = op->ac;
        if (op->statMask & STAT_SPEED)      e->speed = op->speed > MAX_SPEED ? MAX_SPEED : op->speed;
        if (op->statMask & STAT_INITIATIVE) e->initiative = op->initiative;
    }
}

static Waypoint *waypoint_named(const char *name) {
    for (int i = 0; i < gh->waypoints.count; i++)
        if (strcasecmp(gh->waypoints.items[i].name, name) == 0) return &gh->waypoints.items[i];
    return NULL;
}

static void apply_waypoint_set(const Op *op) {
    Waypoint *w = waypoint_named(op->name);
    if (w) {
        w->tile = op->tile;
        return;
    }
    Waypoint nw = { .tile = op->tile };
    snprintf(nw.name, sizeof(nw.name), "%s", op->name);
    da_push(&gh->waypoints, nw);
}

static void apply_init_add(const Op *op) {
    EntityList es = { 0 };
    DA(double) vs = { 0 };
    for (int i = 0; i < op->ids.count; i++) {
        Entity *e = entity_by_id(op->ids.items[i]);
        if (!e) continue;
        da_push(&es, e);
        da_push(&vs, op->values.items[i]);
    }
    game_initiative_list(es.items, vs.items, es.count);
    da_free(&es);
    da_free(&vs);
}

void ops_apply(Op *op) {
    switch (op->type) {
    case OP_MOVE:
        game_move_character(op->tile, entity_by_id(op->id));
        break;
    case OP_SPAWN:
        apply_spawn(op);
        break;
    case OP_DELETE:
        apply_delete(op);
        break;
    case OP_SET_STATS:
        apply_stats(op);
        break;
    case OP_SET_ICON:
        for (int i = 0; i < op->ids.count; i++) {
            Entity *e = character_at(op, i);
            if (e) entity_set_image(e, op->image);
        }
        break;
    case OP_SET_BACKGROUND:
        gfx_set_background_texture(op->image);
        break;
    case OP_GRID_ORIENTATION:
        gfx_toggle_grid_orientation();
        break;
    case OP_WAYPOINT_SET:
        apply_waypoint_set(op);
        break;
    case OP_WAYPOINT_DELETE: {
        Waypoint *w = waypoint_named(op->name);
        if (w) da_remove_at(&gh->waypoints, (int)(w - gh->waypoints.items));
        break;
    }
    case OP_INIT_ADD:
        apply_init_add(op);
        break;
    case OP_INIT_REMOVE:
        for (int i = 0; i < op->ids.count; i++) {
            Entity *e = entity_by_id(op->ids.items[i]);
            if (e) game_remove_from_initiative(e);
        }
        break;
    case OP_INIT_CLEAR:
        game_clear_initiative();
        break;
    case OP_INIT_SHOW:
        game_toggle_show_initiative();
        break;
    case OP_INIT_NEXT:
        game_next_turn();
        break;
    case OP_SET_OWNER:
        for (int i = 0; i < op->ids.count; i++) {
            Entity *e = character_at(op, i);
            if (e) snprintf(e->owner, sizeof(e->owner), "%s", op->name);
        }
        break;
    case OP_COUNT:
        break;
    }
}
```

- [ ] **Step 5: Run the tests and the build**

Run: `make test && make`
Expected: `0 failed` twice, no compiler warnings.

- [ ] **Step 6: Commit**

```bash
git add src/ops.c src/graphics.c src/app.h tests/core_test.c
git commit -m "Implement ops_apply for every op"
```

---

### Task 6: Moves and paste go through ops

**Files:**
- Modify: `src/input.c`

- [ ] **Step 1: Route the move**

In `src/input.c`, replace `#include "app.h"` with:

```c
#include "app.h"
#include "ops.h"
```

In `select_tile`, replace

```c
    if (is_char && ((gh->isShiftDown && ts_has(&gh->entityRangeTiles, gh->currentHexagon)) || gh->gameMaster)) {
        game_move_character(gh->currentHexagon, sel);
```

with

```c
    if (is_char && ((gh->isShiftDown && ts_has(&gh->entityRangeTiles, gh->currentHexagon)) || gh->gameMaster)) {
        Op op = { .type = OP_MOVE, .id = sel->id, .tile = gh->currentHexagon };
        if (ops_submit(&op) != OPR_OK) return;
```

- [ ] **Step 2: Route paste**

Replace the whole `paste_entities` function with:

```c
static void paste_entities(void) {
    if (gh->clipboard.count == 0 || !gh->hasTileUnderMouse) return;
    Hex copy = gh->clipboard.items[0]->tile, paste = gh->tileUnderMouse;
    Cube pivot = hex_to_cube(paste.row - copy.row, paste.col - copy.col);
    Op op = { .type = OP_SPAWN };
    for (int i = 0; i < gh->clipboard.count; i++) {
        Entity *en = gh->clipboard.items[i];
        Cube cp = hex_to_cube(en->tile.row, en->tile.col);
        /* like the Java version, the clipboard moves along, so the next paste is relative to this one */
        en->tile = cube_to_hex((Cube){ pivot.x + cp.x, pivot.y + cp.y, pivot.z + cp.z });
        da_push(&op.specs, op_spec_from_entity(en)); /* copies have no owner */
    }
    ops_submit(&op);
    op_free(&op);
}
```

- [ ] **Step 3: Build and check by hand**

Run: `make`
Expected: no warnings.

Run (draws a character, Shift-click moves it, copy-paste duplicates it, and screenshots each step):

```bash
mkdir -p build/smoke
./toh --size 600x400 --auto-file resources/images/toolIcons/area.png \
  --exec "@click 300 200" --exec ":c speed 3" --exec "@click 300 200" \
  --exec "@down lshift" --exec "@click 360 200" --exec "@up lshift" --exec "@shot build/smoke/moved.png" \
  --exec "@key ctrl+c" --exec "@move 200 120" --exec "@key ctrl+v" --exec "@shot build/smoke/pasted.png" \
  --exec ":q!"
```

Expected: `build/smoke/moved.png` shows the character one or two tiles right of the centre; `build/smoke/pasted.png` shows a second copy near the top left. Look at both images with the Read tool.

- [ ] **Step 4: Commit**

```bash
git add src/input.c
git commit -m "Route moves and paste through the op layer"
```

---

### Task 7: File dialog results go through ops

**Files:**
- Modify: `src/graphics.c`

- [ ] **Step 1: Route background, spawns, walls and icons**

In `src/graphics.c`, after `#include "image.h"` add `#include "ops.h"`.

In `gfx_handle_file_result`, replace the cases from `case ACTION_BACKGROUND:` up to (not including) `default:` with:

```c
    case ACTION_BACKGROUND: {
        Texture *t = texture_from_image(&img);
        Op op = { .type = OP_SET_BACKGROUND, .image = t };
        ops_submit(&op);
        vkr_texture_release(t);
        break;
    }
    case ACTION_SPAWN_CHARACTERS: {
        Image cut = image_cut_circle(&img);
        image_free(&img);
        Texture *t = texture_from_image(&cut);
        Op op = { .type = OP_SPAWN };
        for (int i = 0; i < a->tiles.count; i++)
            da_push(&op.specs, ((EntitySpec){ .type = ENTITY_CHARACTER, .image = t, .tile = a->tiles.items[i],
                                               .size = a->size, .maxHealth = a->maxHealth, .health = a->maxHealth,
                                               .ac = a->ac, .speed = a->speed, .initiative = a->initiative }));
        ops_submit(&op);
        op_free(&op);
        vkr_texture_release(t);
        break;
    }
    case ACTION_WALLS: {
        Image cut = image_cut_hex(&img, gh->isFlat);
        image_free(&img);
        Texture *t = texture_from_image(&cut);
        Op op = { .type = OP_SPAWN };
        for (int i = 0; i < a->tiles.count; i++)
            da_push(&op.specs, ((EntitySpec){ .type = ENTITY_WALL, .image = t, .tile = a->tiles.items[i] }));
        ops_submit(&op);
        op_free(&op);
        vkr_texture_release(t);
        break;
    }
    case ACTION_CHARACTER_ICON: {
        Image cut = image_cut_circle(&img);
        image_free(&img);
        Texture *t = texture_from_image(&cut);
        Op op = { .type = OP_SET_ICON, .image = t };
        for (int i = 0; i < a->tiles.count; i++) {
            Entity *e = entity_at(a->tiles.items[i]);
            if (e && e->type == ENTITY_CHARACTER) da_push(&op.ids, e->id);
        }
        if (op.ids.count > 0) ops_submit(&op);
        op_free(&op);
        vkr_texture_release(t);
        break;
    }
```

- [ ] **Step 2: Build and run the save/load end-to-end test**

Run: `make && make e2e`
Expected: no warnings; `e2e save/load: OK (…)`. The e2e test sets a background, spawns a character and a wall, which now all run through ops.

- [ ] **Step 3: Commit**

```bash
git add src/graphics.c
git commit -m "Route background, spawn, wall and icon dialogs through the op layer"
```

---

### Task 8: Console commands go through ops; `:c owner`

**Files:**
- Modify: `src/console.c`

- [ ] **Step 1: Includes and helpers**

In `src/console.c`, after `#include "app.h"` add:

```c
#include "ops.h"
#include "savefmt.h"
```

(Keep the existing includes; `strings.h` is already included for `strcasecmp`.)

Replace

```c
static const char *const ARGS_CHARACTER[] = { "delete", "size", "maxhealth", "armorclass", "speed", "initiative", "icon", NULL };
```

with

```c
static const char *const ARGS_CHARACTER[] = { "delete", "size", "maxhealth", "armorclass", "speed", "initiative", "icon", "owner", NULL };
```

Replace the functions `cmd_wall` and `cmd_entity` with:

```c
static bool op_has_id(const Op *op, uint32_t id) {
    for (int i = 0; i < op->ids.count; i++)
        if (op->ids.items[i] == id) return true;
    return false;
}

static void submit_simple(OpType type) {
    Op op = { .type = type };
    ops_submit(&op);
}

/* :c / :w / :e delete remove every entity of a type (0 = characters, 1 = walls), like the Java version. */
static void delete_entities(int type) {
    Op op = { .type = OP_DELETE };
    for (int i = 0; i < gh->entities.count; i++) {
        const Entity *e = gh->entities.items[i];
        if ((type == 0) == (e->type == ENTITY_CHARACTER)) da_push(&op.ids, e->id);
    }
    if (op.ids.count > 0) ops_submit(&op);
    op_free(&op);
}

static void cmd_wall(Args *a) {
    if (a->argc > 1 && is_delete(a->argv[1])) delete_entities(1);
    else gfx_summon_wall();
}

static void cmd_entity(Args *a) {
    /* The original passes the wall type here as well. */
    if (a->argc > 1 && is_delete(a->argv[1])) delete_entities(1);
}
```

- [ ] **Step 2: `:c` edits and `:c owner`**

Replace the whole `cmd_character` function with:

```c
static void cmd_character(Args *a) {
    int size = 0, max_health = 0, ac = 0, speed = 0, initiative = 0;
    bool has[6] = { 0 }; /* size, maxhealth, ac, speed, initiative (= STAT_* bit order), icon */
    bool has_owner = false;
    char owner[PLAYER_NAME_MAX] = "";
    for (int i = 1; i < a->argc;) {
        char key[64];
        snprintf(key, sizeof(key), "%s", a->argv[i]);
        lower(key);
        const char *val = (i + 1 < a->argc) ? a->argv[i + 1] : NULL;
        if (strcmp(key, "d") == 0 || strcmp(key, "delete") == 0) {
            delete_entities(0);
            return;
        } else if (strcmp(key, "ic") == 0 || strcmp(key, "icon") == 0) {
            has[5] = true;
            i += 1;
            continue;
        } else if (strcmp(key, "ow") == 0 || strcmp(key, "owner") == 0) {
            if (!val) {
                console_log("Expected a player name or \"none\" after \"%s\"", a->argv[i]);
                return;
            }
            if (strcasecmp(val, "none") == 0) {
                owner[0] = '\0';
            } else if (savefmt_owner_valid(val)) {
                snprintf(owner, sizeof(owner), "%s", val);
            } else {
                console_log("Player names are 1 to %d bytes long, without spaces", PLAYER_NAME_MAX - 1);
                return;
            }
            has_owner = true;
            i += 2;
            continue;
        }
        int *target = NULL;
        int slot = -1;
        if      (!strcmp(key, "sz") || !strcmp(key, "size"))       { target = &size; slot = 0; }
        else if (!strcmp(key, "h")  || !strcmp(key, "maxhealth"))  { target = &max_health; slot = 1; }
        else if (!strcmp(key, "ac") || !strcmp(key, "armorclass")) { target = &ac; slot = 2; }
        else if (!strcmp(key, "s")  || !strcmp(key, "speed"))      { target = &speed; slot = 3; }
        else if (!strcmp(key, "in") || !strcmp(key, "initiative")) { target = &initiative; slot = 4; }
        if (target && parse_int_arg(val, target)) has[slot] = true;
        else if (target) console_log("Expected a number after \"%s\"", a->argv[i]);
        if (slot == 3 && has[3] && speed > MAX_SPEED) {
            console_log("Speed is capped at %d", MAX_SPEED);
            speed = MAX_SPEED;
        }
        i += 2;
    }

    if (gh->selectedEntityTiles.count == 0) {
        if (has_owner) console_log("Select the characters first");
        else gfx_spawn_character(size, max_health, ac, speed, initiative);
        return;
    }

    Op stats = { .type = OP_SET_STATS, .size = size, .maxHealth = max_health, .ac = ac, .speed = speed,
                 .initiative = initiative };
    for (int s = 0; s < 5; s++)
        if (has[s]) stats.statMask |= 1 << s;
    Op own = { .type = OP_SET_OWNER };
    snprintf(own.name, sizeof(own.name), "%s", owner);
    HexList icon_targets = { 0 };
    for (int i = 0; i < gh->selectedEntityTiles.count; i++) {
        Entity *e = entity_at(gh->selectedEntityTiles.items[i]);
        if (!e || e->type != ENTITY_CHARACTER || op_has_id(&stats, e->id)) continue;
        da_push(&stats.ids, e->id);
        da_push(&own.ids, e->id);
        if (has[5]) da_push(&icon_targets, e->tile);
    }
    if (stats.statMask && stats.ids.count > 0) ops_submit(&stats);
    if (has_owner && own.ids.count > 0 && ops_submit(&own) == OPR_OK) {
        if (owner[0]) console_log("%d character(s) now belong to %s", own.ids.count, owner);
        else console_log("%d character(s) no longer have an owner", own.ids.count);
    }
    if (has[5] && icon_targets.count > 0) gfx_request_file(ACTION_CHARACTER_ICON, &icon_targets, 0, 0, 0, 0, 0);
    da_free(&icon_targets);
    op_free(&stats);
    op_free(&own);
}
```

In `cmd_help`, replace

```c
        console_log("  speed <n> | initiative <n> | icon");
```

with

```c
        console_log("  speed <n> | initiative <n> | icon | owner <player|none>");
```

- [ ] **Step 3: Initiative**

Replace the `roll_initiative` helper (added in Task 3) and the whole `cmd_init` function with:

```c
/* Adds the entities that are not in the initiative yet, each with a fresh roll. */
static void roll_initiative(Entity **entities, int n) {
    Op op = { .type = OP_INIT_ADD };
    for (int i = 0; i < n; i++) {
        if (game_is_in_initiative(entities[i]) || op_has_id(&op, entities[i]->id)) continue;
        da_push(&op.ids, entities[i]->id);
        da_push(&op.values, rand_unit());
    }
    if (op.ids.count > 0) ops_submit(&op);
    op_free(&op);
}

static void cmd_init(Args *a) {
    if (a->argc < 2) {
        roll_initiative(gh->entities.items, gh->entities.count);
        return;
    }
    char sub[32];
    snprintf(sub, sizeof(sub), "%s", a->argv[1]);
    lower(sub);
    EntityList sel = { 0 };
    if (!strcmp(sub, "add") || !strcmp(sub, "a")) {
        collect_selected_entities(&sel);
        roll_initiative(sel.items, sel.count);
    } else if (!strcmp(sub, "remove") || !strcmp(sub, "r")) {
        collect_selected_entities(&sel);
        Op op = { .type = OP_INIT_REMOVE };
        for (int i = 0; i < sel.count; i++)
            if (game_is_in_initiative(sel.items[i])) da_push(&op.ids, sel.items[i]->id);
        if (op.ids.count > 0) ops_submit(&op);
        op_free(&op);
    } else if (!strcmp(sub, "clear") || !strcmp(sub, "c")) {
        submit_simple(OP_INIT_CLEAR);
    } else if (!strcmp(sub, "show") || !strcmp(sub, "s")) {
        submit_simple(OP_INIT_SHOW);
    } else if (!strcmp(sub, "next") || !strcmp(sub, "n")) {
        /* names the current turn, so a repeated or crossing "next" can't skip a turn */
        Op op = { .type = OP_INIT_NEXT, .id = gh->currentTurn ? gh->currentTurn->id : 0 };
        ops_submit(&op);
    }
    da_free(&sel);
}
```

- [ ] **Step 4: Waypoints**

In `cmd_waypoint`, replace

```c
        console_log("Deleted waypoint %s", w->name);
        da_remove_at(&gh->waypoints, (int)(w - gh->waypoints.items));
        return;
```

with

```c
        Op op = { .type = OP_WAYPOINT_DELETE };
        snprintf(op.name, sizeof(op.name), "%s", w->name);
        if (ops_submit(&op) == OPR_OK) console_log("Deleted waypoint %s", op.name);
        return;
```

and replace the end of the function

```c
    Waypoint *w = find_waypoint(name);
    if (w) {
        w->tile = tile;
        console_log("Moved waypoint %s to [%d|%d]", w->name, tile.row, tile.col);
    } else {
        Waypoint nw = { .tile = tile };
        snprintf(nw.name, sizeof(nw.name), "%s", name);
        da_push(&gh->waypoints, nw);
        console_log("Created waypoint %s at [%d|%d]", nw.name, tile.row, tile.col);
    }
```

with

```c
    Waypoint *w = find_waypoint(name);
    bool existed = w != NULL;
    char shown[WAYPOINT_NAME_MAX];
    snprintf(shown, sizeof(shown), "%s", existed ? w->name : name);
    Op op = { .type = OP_WAYPOINT_SET, .tile = tile };
    snprintf(op.name, sizeof(op.name), "%s", name);
    if (ops_submit(&op) != OPR_OK) return;
    if (existed) console_log("Moved waypoint %s to [%d|%d]", shown, tile.row, tile.col);
    else console_log("Created waypoint %s at [%d|%d]", shown, tile.row, tile.col);
```

- [ ] **Step 5: Grid orientation**

In `console_command`, replace

```c
    else if (!strcmp(cmd, "grid") || !strcmp(cmd, ":g"))        gfx_toggle_grid_orientation();
```

with

```c
    else if (!strcmp(cmd, "grid") || !strcmp(cmd, ":g"))        submit_simple(OP_GRID_ORIENTATION);
```

- [ ] **Step 6: Check that nothing outside `ops.c` mutates the table any more**

Run:

```bash
grep -n "game_move_character\|game_initiative_list\|game_next_turn\|game_clear_initiative\|game_toggle_show_initiative\|game_remove_from_initiative\|entity_set_image\|gfx_toggle_grid_orientation\|gfx_set_background_texture\|da_push(&gh->waypoints\|da_remove_at(&gh->waypoints\|entity_register" src/*.c | grep -v "^src/ops.c\|^src/game.c\|^src/entity.c"
```

Expected output, exactly these lines (the load path in `save.c` rebuilds the whole table and stays local, `graphics.c` only defines the two functions):

```
src/graphics.c:…:void gfx_toggle_grid_orientation(void) { gh->isFlat = !gh->isFlat; }
src/graphics.c:…:void gfx_set_background_texture(Texture *t) {
src/save.c:…:    game_clear_initiative();
src/save.c:…:        entity_register(e);
src/save.c:…:        da_push(&gh->waypoints, w);
```

If anything else shows up, route it through an op the same way.

- [ ] **Step 7: Build and test**

Run: `make && make test && make e2e`
Expected: no warnings, `0 failed` twice, `e2e save/load: OK (…)`.

- [ ] **Step 8: Commit**

```bash
git add src/console.c
git commit -m "Route console commands through the op layer; add :c owner"
```

---

### Task 9: Owners in the end-to-end test and the README

**Files:**
- Modify: `tests/e2e_save.sh`
- Modify: `README.md`

- [ ] **Step 1: Extend the e2e test**

In `tests/e2e_save.sh`, replace

```bash
    --exec ":load! $out/table.tuhh" \
    --exec "@shot $out/reloaded.png" \
    --exec ":q" \
```

with

```bash
    --exec ":load! $out/table.tuhh" \
    --exec "@shot $out/reloaded.png" \
    --exec "@click 300 200" --exec ":c owner Bob" \
    --exec ":q" \
    --exec "@shot $out/owner-refused.png" \
    --exec ":c owner none" \
    --exec ":q" \
```

and after the line `[ -f "$out/quit-refused.png" ] || fail ":q quit despite unsaved changes"` add:

```bash
[ -f "$out/owner-refused.png" ] || fail ":q quit although the owner of a character changed"
```

(The final `:q` must still quit by itself: removing the owner again restores the saved table, so `timeout` catches a fingerprint that ignores owners.)

- [ ] **Step 2: Run it**

Run: `make e2e`
Expected: `e2e save/load: OK (…)`.

- [ ] **Step 3: Update the README**

In `README.md`, in the "Console commands" paragraph, replace

```
`:c` character
(`delete`, `size n`, `maxhealth n`, `armorclass n`, `speed n`, `initiative n`, `icon`),
```

with

```
`:c` character
(`delete`, `size n`, `maxhealth n`, `armorclass n`, `speed n`, `initiative n`, `icon`, `owner <player|none>`),
```

In the "Saving" section, replace

```
A save contains the background, characters (image and stats), walls, the initiative order, waypoints, grid
orientation, camera and theme; not the console, selections, clipboard or measurements.
```

with

```
A save contains the background, characters (image, stats and owner), walls, the initiative order, waypoints, grid
orientation, camera and theme; not the console, selections, clipboard or measurements. `:c owner <player>` gives the
selected characters to a player (for the upcoming multiplayer mode); pasted copies have no owner.
```

and replace

```
The format (`src/savefmt.h`) is binary and compact:
```

with

```
The format (`src/savefmt.h`, version 2; version 1 files still load) is binary and compact:
```

In the "Source layout" table, after the `src/game.c` row add:

```
| `src/ops.c` | – (new) every change to the table as a checked operation; GM / player rules |
```

In the "Tests" paragraph, replace `` `make test` (save format unit tests)`` with `` `make test` (save format and game core unit tests)``.

- [ ] **Step 4: Commit**

```bash
git add tests/e2e_save.sh README.md
git commit -m "Test owners in the save e2e test; document :c owner and format v2"
```

---

### Task 10: Final verification

- [ ] **Step 1: Clean build with sanitizers**

Run:

```bash
make clean && make CFLAGS="-O1 -g -fsanitize=address,undefined" && make test CFLAGS="-O1 -g -fsanitize=address,undefined" && make e2e
```

Expected: no compiler warnings, `0 failed` twice, no sanitizer reports, `e2e save/load: OK (…)`.

- [ ] **Step 2: Fuzz briefly (the decoder changed)**

Run: `make fuzz FUZZ_TIME=60`
Expected: libFuzzer runs 60 s and exits without a crash report. (Needs clang; if clang is missing, say so in the report instead of skipping silently.)

- [ ] **Step 3: Normal build again**

Run: `make clean && make`
Expected: builds `./toh` without warnings.

- [ ] **Step 4: Report**

Summarize for the user: which commits were made, test output (checks counts), and anything that behaved differently from this plan.

---

## Notes for Plan 3 (from the Task 4 review)

- **Texture lifetime:** `Op` borrows its `Texture *`s. Ops decoded from the network and queued must hold their own
  references (or name images by id and resolve them at apply time), or a texture can be freed between decode and apply.
- **Spawn ids on clients:** `ops_apply(OP_SPAWN)` replaces `op->ids` with fresh ids. Clients applying `OP_APPLIED`
  must create the entities with the host's ids instead.
- **Player names:** ownership matches names case-insensitively (ASCII `strcasecmp`); `:join` must reject a name that
  equals an existing player's name under the same comparison.
- **`OP_INIT_NEXT`** carries the id of the entity whose turn it is (`OPR_STALE` otherwise); clients fill it from their
  copy of the table.
- **Local view state in `ops_apply`:** `OP_DELETE` clears all selection highlights and `OP_MOVE` overwrites
  `gh->path`. Fine offline; once remote ops are applied, only clear highlights that refer to deleted entities and
  don't touch the local path preview for other players' moves.
