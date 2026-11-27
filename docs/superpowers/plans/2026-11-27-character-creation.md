# Character Creation in the HUD Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Create characters through the bottom bar (name → size → attributes → picture → summary → place), with
a GM "+" box and a GM-only tile-menu entry, and store the Tales of Honor attributes (STR, AGI, COG, SPI) on
characters so Max HP, Max Stamina, Speed and Initiative follow them.

**Architecture:** `src/attrs.c` holds the attribute rules (`attr_derive`). Entities, ops, saves (v9) and the wire
(protocol 11) carry `attr[4]`; `OP_SET_STATS` gets four attribute bits whose derived stats the host recalculates.
`src/charcreate.c` is the creation draft and step machine (no drawing); `src/hud_create.c` turns it into HUD rows
and values; six new HUD element types (button, text field, stepper, choice, label, image), a "+" part and
tooltips are added to the HUD core. Placing is a map mode in `input.c` / `graphics.c`.

**Tech Stack:** C11, SDL3, the project's Vulkan 2D renderer (`dr_*`), unit tests in `tests/*.c` (`make test`),
e2e shell scripts with ImageMagick (`make e2e`).

Spec: `docs/superpowers/specs/2026-11-27-character-creation-design.md`.

**Conventions (read once):**
- Build: `make -j`. Unit tests: `make test` (all must pass; it prints `N checks, 0 failed` per suite).
- `tests/core_test.c` links the files in `CORE_SRC` (Makefile) against `tests/core_stubs.c`. A new src file used
  by a CORE_SRC file must be added to `CORE_SRC`; a function from a file outside CORE_SRC needs a stub.
- Log text goes through `console_log`; tests read the last line from `stub_last_log`.
- Commit messages end with the line `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- The guides (`playercontrols.md`, `gmcontrols.md`, README Controls) are updated in Task 14, together with the
  feature commit that makes them true (the user-visible parts land in Tasks 6, 11, 12; Task 14 must follow them
  before anything is pushed).

---

## File structure

| File | Status | Responsibility |
|---|---|---|
| `src/attrs.h`, `src/attrs.c` | new | attribute ids, names, ranges, `attr_derive`, `attr_by_name` |
| `src/charcreate.h`, `src/charcreate.c` | new | the creation draft and its steps; placing; spawn / request |
| `src/hud_create.h`, `src/hud_create.c` | new | HUD rows per step and for "no character", publishing, HUD events → `cc_*` |
| `src/app.h` | modify | `Entity.attr`, `PendingAction.attr`, `ACTION_CREATE_PICTURE`, signatures, `console_write_log` |
| `src/ops.h`, `src/ops.c` | modify | `STAT_STR..STAT_SPI`, `Op.attr`, `EntitySpec.attr`, recalculation and log |
| `src/savefmt.h`, `src/savefmt.c`, `src/save.c` | modify | save format v9 |
| `src/netmsg.h`, `src/netmsg.c` | modify | protocol 11: attributes, 16-bit stat mask, `NM_CREATE_ANSWER` |
| `src/session.h`, `src/session.c` | modify | attributes over the wire, request seq and answers, client's waiting list |
| `src/console.c` | modify | `:c str/agi/cog/spi`, help, Tab completion, `console_write_log` |
| `src/graphics.c` | modify | picture dialog result, placement drawing, new `attr` parameters |
| `src/input.c` | modify | text field routing, placement clicks / Esc / keyboard |
| `src/hud.h`, `src/hud.c`, `src/hud_widgets.c`, `src/hud_theme.h`, `src/hud_theme.c` | modify | new element types, focus and editing, "+" part, tooltips |
| `src/hud_game.c` | modify | chooses rows (normal / no character / step), "+", STR/AGI pills, tab tooltip |
| `src/radial_config.c`, `src/radial_game.c` | modify | GM-only "Create character" on empty tiles |
| `src/main.c` | modify | `@logfile` script action |
| `Makefile` | modify | CORE_SRC, e2e target |
| `tests/core_test.c`, `tests/core_stubs.c`, `tests/save_test.c`, `tests/netmsg_test.c` | modify | unit tests and stubs |
| `tests/e2e_hud.sh` | modify | GM creation flows |
| `tests/e2e_create.sh` | new | a player creates through the bar; the GM declines, then accepts |
| `playercontrols.md`, `gmcontrols.md`, `README.md` | modify | guides |

---

### Task 1: Attribute rules (`attrs.c`)

**Files:**
- Create: `src/attrs.h`, `src/attrs.c`
- Modify: `Makefile` (CORE_SRC), `tests/core_test.c`

- [ ] **Step 1: Write the failing test**

In `tests/core_test.c` add `#include "attrs.h"` after `#include "app.h"`, add this test before `int main`, and call
`test_attr_rules();` in `main` after `test_hp_rules();`:

```c
/* Tales of Honor's stats from the attributes (attrs.h) */
static void test_attr_rules(void) {
    AttrStats d = attr_derive((const int[ATTR_COUNT]){ 0, 0, 0, 0 });
    CHECK(d.maxHealth == 12 && d.maxStamina == 8 && d.speed == 3 && d.initiative == 0);
    d = attr_derive((const int[ATTR_COUNT]){ -2, -2, -2, -2 }); /* the steps' lowest */
    CHECK(d.maxHealth == 6 && d.maxStamina == 6 && d.speed == 2 && d.initiative == -2);
    d = attr_derive((const int[ATTR_COUNT]){ 3, 3, 3, 3 }); /* the steps' highest */
    CHECK(d.maxHealth == 21 && d.maxStamina == 11 && d.speed == 6 && d.initiative == 3);
    d = attr_derive((const int[ATTR_COUNT]){ -10, -10, 10, 0 }); /* :c's range: never below 0, speed at least 2 */
    CHECK(d.maxHealth == 0 && d.maxStamina == 0 && d.speed == 2 && d.initiative == 10);
    CHECK(attr_by_name("STR") == ATTR_STR && attr_by_name("agi") == ATTR_AGI && attr_by_name("Cog") == ATTR_COG);
    CHECK(attr_by_name("spi") == ATTR_SPI && attr_by_name("vig") == -1 && attr_by_name("") == -1);
    CHECK(!strcmp(ATTR_NAMES[ATTR_STR], "STR") && !strcmp(ATTR_NAMES[ATTR_SPI], "SPI"));
    CHECK(attr_valid(ATTR_MIN) && attr_valid(ATTR_MAX) && !attr_valid(ATTR_MIN - 1) && !attr_valid(ATTR_MAX + 1));
}
```

- [ ] **Step 2: Run it to see it fail**

Run: `make build/core_test`
Expected: compile error, `attrs.h: No such file or directory`.

- [ ] **Step 3: Implement**

`src/attrs.h`:

```c
/* The attributes of Tales of Honor (STR, AGI, COG, SPI) and the stats they set (the rulebook's stats table):
 * Max HP 12 + 3 STR, Max Stamina 8 + AGI, Speed AGI + 3 (at least 2), Initiative COG. SPI sets nothing yet.
 * No dependencies: app.h, ops.c, console.c and the creation steps include it. */
#ifndef TOH_ATTRS_H
#define TOH_ATTRS_H

#include <stdbool.h>

enum { ATTR_STR, ATTR_AGI, ATTR_COG, ATTR_SPI, ATTR_COUNT };
#define ATTR_MIN (-10)        /* :c, saves, the wire */
#define ATTR_MAX 10
#define ATTR_CREATE_MIN (-2)  /* the creation steps and players' requests (the rulebook's range) */
#define ATTR_CREATE_MAX 3

extern const char *const ATTR_NAMES[ATTR_COUNT]; /* "STR", "AGI", "COG", "SPI" */

typedef struct { int maxHealth, maxStamina, speed, initiative; } AttrStats;

/* Max HP and max stamina never go below 0 (0 = the "no HP" / "no stamina" state). */
AttrStats attr_derive(const int attr[ATTR_COUNT]);
/* "str" .. "spi", any case; -1 if it is none of them. */
int attr_by_name(const char *s);
static inline bool attr_valid(int v) { return v >= ATTR_MIN && v <= ATTR_MAX; }

#endif
```

`src/attrs.c`:

```c
/* Attribute rules (see attrs.h). */
#include "attrs.h"

#include <strings.h>

const char *const ATTR_NAMES[ATTR_COUNT] = { "STR", "AGI", "COG", "SPI" };

AttrStats attr_derive(const int attr[ATTR_COUNT]) {
    AttrStats d;
    d.maxHealth = 12 + 3 * attr[ATTR_STR];
    d.maxStamina = 8 + attr[ATTR_AGI];
    d.speed = attr[ATTR_AGI] + 3;
    d.initiative = attr[ATTR_COG];
    if (d.maxHealth < 0) d.maxHealth = 0;
    if (d.maxStamina < 0) d.maxStamina = 0;
    if (d.speed < 2) d.speed = 2;
    return d;
}

int attr_by_name(const char *s) {
    for (int i = 0; i < ATTR_COUNT; i++)
        if (strcasecmp(s, ATTR_NAMES[i]) == 0) return i;
    return -1;
}
```

In the `Makefile`, add `src/attrs.c` to the end of the `CORE_SRC` list (the line ending in `src/dice.c`):
`src/moves.c src/move_ui.c src/recover.c src/dice.c src/attrs.c`.

- [ ] **Step 4: Run the tests**

Run: `make test`
Expected: every suite ends with `0 failed`.

- [ ] **Step 5: Commit**

```bash
git add src/attrs.h src/attrs.c Makefile tests/core_test.c
git commit -m "Attributes: attrs.c with the Tales of Honor stats from STR, AGI, COG, SPI

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: Attributes on characters and in ops

**Files:**
- Modify: `src/app.h`, `src/ops.h`, `src/ops.c`, `tests/core_test.c`

- [ ] **Step 1: Write the failing test**

Add before `int main` and call `test_attribute_ops();` after `test_stamina();`:

```c
static void test_attribute_ops(void) {
    reset_table();
    OpAuthor alice = player("Alice");
    Entity *hero = add_character((Hex){ 0, 0 }, 3, "Alice");
    hero->maxHealth = hero->health = 15;
    snprintf(hero->name, sizeof(hero->name), "Mao");
    CHECK(hero->attr[ATTR_STR] == 0 && hero->attr[ATTR_SPI] == 0); /* new characters: all 0 */

    /* STR sets max HP; full HP stays full; GM only */
    Op str = { .type = OP_SET_STATS, .statMask = STAT_STR };
    str.attr[ATTR_STR] = 2;
    da_push(&str.ids, hero->id);
    CHECK_R(ops_check(&str, &alice), OPR_GM_ONLY);
    CHECK_R(ops_check(&str, &GM), OPR_OK);
    ops_apply(&str);
    CHECK(hero->attr[ATTR_STR] == 2 && hero->maxHealth == 18 && hero->health == 18);
    CHECK(strstr(stub_last_log, "Mao: STR 0 -> 2, max HP 15 -> 18"));

    /* AGI sets max stamina and speed; a speed given in the same op wins */
    hero->health = 5;
    Op agi = { .type = OP_SET_STATS, .statMask = STAT_AGI | STAT_SPEED, .speed = 9 };
    agi.attr[ATTR_AGI] = 2;
    da_push(&agi.ids, hero->id);
    ops_apply(&agi);
    CHECK(hero->maxStamina == 10 && hero->stamina == 10 && hero->speed == 9 && hero->health == 5);
    CHECK(strstr(stub_last_log, "Mao: AGI 0 -> 2, max stamina 0 -> 10, speed 3 -> 9"));

    /* COG sets initiative; SPI nothing */
    Op cog = { .type = OP_SET_STATS, .statMask = STAT_COG | STAT_SPI };
    cog.attr[ATTR_COG] = 1;
    cog.attr[ATTR_SPI] = -1;
    da_push(&cog.ids, hero->id);
    ops_apply(&cog);
    CHECK(hero->initiative == 1 && hero->attr[ATTR_SPI] == -1);
    CHECK(strstr(stub_last_log, "Mao: COG 0 -> 1, initiative 0 -> 1, SPI 0 -> -1"));

    /* someone else's op changes the values but logs nothing here */
    int logs = stub_log_count;
    cog.remote = true;
    cog.attr[ATTR_COG] = 2;
    ops_apply(&cog);
    CHECK(hero->initiative == 2 && stub_log_count == logs);

    /* out of range */
    str.attr[ATTR_STR] = ATTR_MAX + 1;
    CHECK_R(ops_check(&str, &GM), OPR_INVALID);
    str.attr[ATTR_STR] = ATTR_MIN - 1;
    CHECK_R(ops_check(&str, &GM), OPR_INVALID);

    /* spawns and copies keep the attributes */
    EntitySpec s = op_spec_from_entity(hero);
    CHECK(s.attr[ATTR_STR] == 2 && s.attr[ATTR_AGI] == 2 && s.attr[ATTR_SPI] == -1);
    s.tile = (Hex){ 0, 4 };
    Op sp = { .type = OP_SPAWN };
    da_push(&sp.specs, s);
    CHECK_R(ops_check(&sp, &GM), OPR_OK);
    ops_apply(&sp);
    Entity *copy = entity_by_id(sp.ids.items[0]);
    CHECK(copy && copy->attr[ATTR_COG] == 2 && copy->attr[ATTR_SPI] == -1);
    sp.specs.items[0].attr[ATTR_AGI] = ATTR_MAX + 1;
    CHECK_R(ops_check(&sp, &GM), OPR_INVALID);

    op_free(&str);
    op_free(&agi);
    op_free(&cog);
    op_free(&sp);
    reset_table();
}
```

- [ ] **Step 2: Run it to see it fail**

Run: `make build/core_test`
Expected: compile errors: `'Entity' has no member named 'attr'`, `'STAT_STR' undeclared`.

- [ ] **Step 3: Implement**

`src/app.h`: add `#include "attrs.h"` next to the other includes at the top. In `struct Entity` after
`int maxStamina, stamina;` add:

```c
    int attr[ATTR_COUNT];    /* Tales of Honor attributes (attrs.h); the stats they set follow them (OP_SET_STATS) */
```

Check `entity_new_character` in `src/entity.c` allocates with `calloc` (so `attr` starts at 0); if it uses
`malloc`, add `memset(e->attr, 0, sizeof(e->attr));` there.

`src/ops.h`:
- Replace the `STAT_*` enum with:

```c
enum { STAT_SIZE = 1, STAT_MAXHEALTH = 2, STAT_SPEED = 4, STAT_INITIATIVE = 8, STAT_MAXSTAMINA = 16,
       STAT_STR = 32, STAT_AGI = 64, STAT_COG = 128, STAT_SPI = 256, STAT_ALL = 511 };
#define STAT_ATTR(a) (STAT_STR << (a)) /* the bit of attribute a (attrs.h) */
#define STAT_ATTRS (STAT_STR | STAT_AGI | STAT_COG | STAT_SPI)
```

- In `EntitySpec` after `int maxStamina, stamina;` add `int attr[ATTR_COUNT]; /* characters only */`.
- In `Op` after `int size, maxHealth, speed, initiative, maxStamina;` add
  `int attr[ATTR_COUNT]; /* OP_SET_STATS: the attribute bits' values */`.
- Change the `OP_SET_STATS` comment to: `/* ids; statMask says which of the values below change; an attribute bit also sets the stats it gives (attrs.h) unless they are in the mask too */`.

`src/ops.c`:
- `op_spec_from_entity`: after the `snprintf(s.party, ...)` line add `memcpy(s.attr, e->attr, sizeof(s.attr));`.
- `check_structure`, case `OP_SPAWN`, inside the loop after the max stamina check add:

```c
            for (int a = 0; a < ATTR_COUNT; a++)
                if (!attr_valid(s->attr[a])) return OPR_INVALID;
```

- `check_structure`, case `OP_SET_STATS`, before `return ids_exist(...)` add:

```c
        if (op->statMask & ~STAT_ALL) return OPR_INVALID;
        for (int a = 0; a < ATTR_COUNT; a++)
            if ((op->statMask & STAT_ATTR(a)) && !attr_valid(op->attr[a])) return OPR_INVALID;
```

- `apply_spawn`: in the character branch after `snprintf(e->party, ...)` add `memcpy(e->attr, s->attr, sizeof(e->attr));`.
- Replace `apply_stats` with:

```c
/* "Mao: STR 1 -> 2, max HP 15 -> 18" for the person who made the change */
static void log_attributes(const Entity *e, int mask, const int old_attr[ATTR_COUNT], int maxHealth, int maxStamina,
                           int speed, int initiative) {
    char name[CHAR_NAME_MAX + 16], line[256];
    entity_display_name(e, name, sizeof(name));
    int n = snprintf(line, sizeof(line), "%s:", name);
    const char *sep = " ";
#define ADD(...)                                                                                     \
    do {                                                                                             \
        if (n < (int)sizeof(line)) n += snprintf(line + n, sizeof(line) - (size_t)n, "%s", sep);    \
        if (n < (int)sizeof(line)) n += snprintf(line + n, sizeof(line) - (size_t)n, __VA_ARGS__);  \
        sep = ", ";                                                                                  \
    } while (0)
    for (int a = 0; a < ATTR_COUNT; a++) {
        if (!(mask & STAT_ATTR(a))) continue;
        ADD("%s %d -> %d", ATTR_NAMES[a], old_attr[a], e->attr[a]);
        if (a == ATTR_STR) ADD("max HP %d -> %d", maxHealth, e->maxHealth);
        if (a == ATTR_AGI) {
            ADD("max stamina %d -> %d", maxStamina, e->maxStamina);
            ADD("speed %d -> %d", speed, e->speed);
        }
        if (a == ATTR_COG) ADD("initiative %d -> %d", initiative, e->initiative);
    }
#undef ADD
    console_log("%s", line);
}

static void apply_stats(const Op *op) {
    int m = op->statMask;
    for (int i = 0; i < op->ids.count; i++) {
        Entity *e = character_at(op, i);
        if (!e) continue;
        int old_attr[ATTR_COUNT];
        memcpy(old_attr, e->attr, sizeof(old_attr));
        int oldMaxH = e->maxHealth, oldMaxSt = e->maxStamina, oldSpeed = e->speed, oldIni = e->initiative;
        for (int a = 0; a < ATTR_COUNT; a++)
            if (m & STAT_ATTR(a)) e->attr[a] = op->attr[a];
        AttrStats d = attr_derive(e->attr);
        int maxH = e->maxHealth, maxSt = e->maxStamina, speed = e->speed, ini = e->initiative;
        if (m & STAT_STR) maxH = d.maxHealth;
        if (m & STAT_AGI) {
            maxSt = d.maxStamina;
            speed = d.speed;
        }
        if (m & STAT_COG) ini = d.initiative;
        /* a value given by hand in the same op wins over the attribute's */
        if (m & STAT_MAXHEALTH) maxH = op->maxHealth;
        if (m & STAT_MAXSTAMINA) maxSt = op->maxStamina;
        if (m & STAT_SPEED) speed = op->speed;
        if (m & STAT_INITIATIVE) ini = op->initiative;
        if (m & STAT_SIZE) e->size = op->size;
        if (maxH != e->maxHealth) {
            e->health = hp_after_max_change(e->health, e->maxHealth, maxH);
            e->maxHealth = maxH;
        }
        if (maxSt != e->maxStamina) {
            e->stamina = hp_after_max_change(e->stamina, e->maxStamina, maxSt);
            e->maxStamina = maxSt;
        }
        e->speed = speed > MAX_SPEED ? MAX_SPEED : speed;
        e->initiative = ini;
        if ((m & STAT_ATTRS) && !op->remote) log_attributes(e, m, old_attr, oldMaxH, oldMaxSt, oldSpeed, oldIni);
    }
}
```

`ops.c` needs `#include <string.h>` (check it is there).

- [ ] **Step 4: Run the tests**

Run: `make test`
Expected: all suites `0 failed`. (`test_stamina` still passes: it sets only `STAT_MAXSTAMINA`.)

- [ ] **Step 5: Commit**

```bash
git add src/app.h src/ops.h src/ops.c src/entity.c tests/core_test.c
git commit -m "Attributes on characters: OP_SET_STATS bits for STR/AGI/COG/SPI recalculate the stats they set

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: Save format v9

**Files:**
- Modify: `src/savefmt.h`, `src/savefmt.c`, `src/save.c`, `tests/save_test.c`

- [ ] **Step 1: Write the failing test**

In `tests/save_test.c` add before `int main` and call `test_attr_versions();` after `test_turn_used_versions();`:

```c
/* attributes (version 9): saved per character; a version 8 body loads with 0s */
static void test_attr_versions(void) {
    Sample s;
    sample_init(&s);
    s.entities[0].attr[0] = 3;
    s.entities[0].attr[3] = -2;
    size_t len;
    uint8_t *file = encode_sample(&s, &len);
    SaveData d;
    CHECK_ERR(savefmt_decode(file, len, &d), SAVE_OK);
    CHECK(d.entities[0].attr[0] == 3 && d.entities[0].attr[1] == 0 && d.entities[0].attr[3] == -2);
    savefmt_free(&d);
    free(file);

    s.entities[0].attr[1] = SAVEFMT_MAX_ATTR + 1;
    CHECK(encode_sample_err(&s) == SAVE_ERR_INVALID);
    s.entities[0].attr[1] = SAVEFMT_MIN_ATTR - 1;
    CHECK(encode_sample_err(&s) == SAVE_ERR_INVALID);
    s.entities[0].attr[1] = 0;
    s.entities[2].attr[0] = 1; /* a wall */
    CHECK(encode_sample_err(&s) == SAVE_ERR_INVALID);
    sample_free(&s);

    W st = { .n = 0 }, im = { .n = 0 };
    wimage(&im, 8, 8);
    wworld(&st, 0);
    wuv(&st, 1);
    wcharacter(&st, 3, "Bob");
    wstr(&st, "Mao");
    wsv(&st, 9); wsv(&st, 4);
    wstr(&st, "Heroes");
    w8(&st, 1); /* turn_used (version 8) */
    wuv(&st, 0); wuv(&st, 0);
    wuv(&st, 0);
    uint8_t *f = wrap_v(&st, &im, &len, 1, 8);
    CHECK_ERR(savefmt_decode(f, len, &d), SAVE_OK);
    CHECK(d.nentities == 1 && d.entities[0].turn_used == 1 && d.entities[0].attr[0] == 0 && d.entities[0].attr[3] == 0);
    savefmt_free(&d);
    free(f);
}
```

(`sample_init` makes entity 2 the wall, as `test_turn_used_versions` uses it.)

- [ ] **Step 2: Run it to see it fail**

Run: `make build/save_test`
Expected: compile error `'SaveEntity' has no member named 'attr'`.

- [ ] **Step 3: Implement**

`src/savefmt.h`:
- In the header comment after the version 8 line add ` * Version 9 adds the attributes STR, AGI, COG and SPI after turn_used.` and change "version 8" in the first line to "version 9".
- `#define SAVEFMT_VERSION     9`
- After `SAVEFMT_MAX_TURN_USED` add:

```c
#define SAVEFMT_ATTRS           4    /* = ATTR_COUNT */
#define SAVEFMT_MIN_ATTR        (-10) /* = ATTR_MIN */
#define SAVEFMT_MAX_ATTR        10   /* = ATTR_MAX */
```

- In `SaveEntity` after `turn_used` add
  `int32_t attr[SAVEFMT_ATTRS]; /* characters only: STR, AGI, COG, SPI (version 9) */`.

`src/savefmt.c`:
- In the entity validation loop (after the `party` check) add:

```c
        for (int a = 0; a < SAVEFMT_ATTRS; a++)
            if (e->attr[a] < SAVEFMT_MIN_ATTR || e->attr[a] > SAVEFMT_MAX_ATTR ||
                (e->attr[a] && e->type != SAVE_ENT_CHARACTER))
                return false;
```

- `write_state`, character branch after `put_u8(b, e->turn_used);`:
  `for (int a = 0; a < SAVEFMT_ATTRS; a++) put_sv(b, e->attr[a]);`
- decode, character branch after the `version >= 8` line:
  `if (version >= 9) for (int a = 0; a < SAVEFMT_ATTRS; a++) e->attr[a] = get_sv(&r);`

`src/save.c`:
- Writing (the block that sets `s->turn_used`): add `memcpy(s->attr, e->attr, sizeof(s->attr));` inside the
  `if (e->type == ENTITY_CHARACTER)` block.
- Loading (after `e->turnUsed = s->turn_used;`): add `memcpy(e->attr, s->attr, sizeof(e->attr));`.
- Next to the existing `_Static_assert(SAVEFMT_MAX_TURN_USED == TURN_ALL, ...)` add
  `_Static_assert(SAVEFMT_ATTRS == ATTR_COUNT && SAVEFMT_MIN_ATTR == ATTR_MIN && SAVEFMT_MAX_ATTR == ATTR_MAX, "attribute limits differ");`
  (find it with `grep -n _Static_assert src/*.c`; if it is not in save.c, put the new one in save.c).

- [ ] **Step 4: Run the tests**

Run: `make test`
Expected: all suites `0 failed`.

- [ ] **Step 5: Commit**

```bash
git add src/savefmt.h src/savefmt.c src/save.c tests/save_test.c
git commit -m "Save format v9: characters keep their attributes (older saves load with 0s)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: Protocol 11 (`netmsg`)

**Files:**
- Modify: `src/netmsg.h`, `src/netmsg.c`, `tests/netmsg_test.c`, `tests/core_test.c`

- [ ] **Step 1: Write the failing test**

`tests/netmsg_test.c`:
- In `sample_op`, case `NOP_SET_STATS`: change the mask line to
  `o.stat_mask = 1 | 4 | 8 | 16 | NSTAT_STR | NSTAT_SPI; o.max_stamina = 7; o.size = -2; o.speed = 21; o.initiative = 4; o.attr[0] = 3; o.attr[3] = -2;`
- In the `NOP_SPAWN` sample, give the character spec `.attr = { 1, 2, 0, -1 }` (add to its initializer).
- In `sample_msg`, case `NM_CREATE`: after `strcpy(m.spec.name, "Nina");` add
  `m.spec.attr[0] = 1; m.spec.attr[1] = 2; m.spec.attr[3] = -1;`
- In `sample_msg` add a case:

```c
    case NM_CREATE_ANSWER: m.seq = 7; m.accepted = true; break;
```

- Add a test and call it from `main` after the other invalid-message tests:

```c
static void test_attr_limits(void) {
    NetMsg m = sample_msg(NM_CREATE, 0);
    m.spec.attr[2] = SAVEFMT_MAX_ATTR + 1;
    check_invalid(m, "create with an attribute over the limit");
    NetMsg op = sample_msg(NM_OP, NOP_SET_STATS);
    op.op.stat_mask = NSTAT_ALL + 1;
    check_invalid(op, "set stats with an unknown bit");
    op = sample_msg(NM_OP, NOP_SET_STATS);
    op.op.attr[0] = SAVEFMT_MIN_ATTR - 1;
    check_invalid(op, "set stats with an attribute under the limit");
    /* CREATE_ANSWER: "accepted" is 0 or 1 */
    NetMsg a = sample_msg(NM_CREATE_ANSWER, 0);
    uint8_t *buf;
    size_t len;
    CHECK(netmsg_encode(&a, &buf, &len));
    buf[len - 1] = 2;
    NetMsg d;
    CHECK(!netmsg_decode(buf, len, &d));
    netmsg_free(&d);
    free(buf);
}
```

(If `sample_msg(NM_OP, ...)` takes the op type differently, use the same call the file's other tests use for an
`NM_OP` sample. Free the samples' arrays the way the other `check_invalid` callers do.)

`tests/core_test.c`: next to the stat-bit asserts add

```c
_Static_assert((int)NSTAT_STR == (int)STAT_STR && (int)NSTAT_AGI == (int)STAT_AGI && (int)NSTAT_COG == (int)STAT_COG &&
               (int)NSTAT_SPI == (int)STAT_SPI && (int)NSTAT_ALL == (int)STAT_ALL && NETMSG_ATTRS == ATTR_COUNT,
               "attribute bits must match");
```

- [ ] **Step 2: Run it to see it fail**

Run: `make build/netmsg_test`
Expected: compile errors (`NSTAT_STR`, `NM_CREATE_ANSWER`, `accepted` unknown).

- [ ] **Step 3: Implement**

`src/netmsg.h`:
- `#define NET_PROTOCOL_VERSION 11`
- `#define NETMSG_ATTRS 4 /* = ATTR_COUNT: STR, AGI, COG, SPI */`
- After `NM_CREATE` in `NetMsgType`: `NM_CREATE_ANSWER, /* H->C seq (of the player's CREATE), accepted */`
- Replace the NSTAT enum:

```c
enum { NSTAT_SIZE = 1, NSTAT_MAXHEALTH = 2, NSTAT_SPEED = 4, NSTAT_INITIATIVE = 8, NSTAT_MAXSTAMINA = 16,
       NSTAT_STR = 32, NSTAT_AGI = 64, NSTAT_COG = 128, NSTAT_SPI = 256, NSTAT_ALL = 511 };
```

- `NOP_SET_STATS` comment: `/* ids, stat_mask (NSTAT_* bits, 1..NSTAT_ALL), the masked values */`
- `NetSpec`: after `party` add `int32_t attr[NETMSG_ATTRS]; /* characters: SAVEFMT_MIN_ATTR..SAVEFMT_MAX_ATTR */`
- `NetOp`: `uint16_t stat_mask;` (was `uint8_t`) and after `max_stamina` add `int32_t attr[NETMSG_ATTRS];`
- `NetMsg`: after `spec` add `bool accepted; /* CREATE_ANSWER */` and change the `seq` comment to
  `/* OP, OP_REJECTED, CREATE, CREATE_ANSWER */`.

`src/netmsg.c`:
- Add a helper above `spec_ok`:

```c
static bool attrs_ok(const int32_t *a) {
    for (int i = 0; i < NETMSG_ATTRS; i++)
        if (a[i] < SAVEFMT_MIN_ATTR || a[i] > SAVEFMT_MAX_ATTR) return false;
    return true;
}
```

- `spec_ok`: change the final return to
  `return s->speed <= SAVEFMT_MAX_SPEED && ... && s->stamina <= SAVEFMT_MAX_HP && attrs_ok(s->attr);`
- `op_ok`, `NOP_SET_STATS`: add `if ((o->stat_mask & (NSTAT_STR | NSTAT_AGI | NSTAT_COG | NSTAT_SPI)) && !attrs_ok(o->attr)) return false;`
  (the existing `o->stat_mask > NSTAT_ALL` check now uses the new NSTAT_ALL).
- `msg_ok`: add `case NM_CREATE_ANSWER: return true;`
- Encoding, `NOP_SPAWN` character branch after `put_str(b, s->party);`:
  `for (int a = 0; a < NETMSG_ATTRS; a++) put_sv(b, s->attr[a]);`
- Encoding, `NOP_SET_STATS`:

```c
    case NOP_SET_STATS: {
        put_ids(b, o->ids, o->nids);
        put_uv(b, o->stat_mask);
        const int32_t v[9] = { o->size, o->max_health, o->speed, o->initiative, o->max_stamina,
                               o->attr[0], o->attr[1], o->attr[2], o->attr[3] };
        for (int i = 0; i < 9; i++)
            if (o->stat_mask & (1 << i)) put_sv(b, v[i]);
        break;
    }
```

- Encoding, `NM_CREATE` after `put_sv(&b, m->spec.max_stamina);`:
  `for (int a = 0; a < NETMSG_ATTRS; a++) put_sv(&b, m->spec.attr[a]);`
- Encoding, new case:

```c
    case NM_CREATE_ANSWER:
        put_uv(&b, m->seq);
        put_u8(&b, m->accepted ? 1 : 0);
        break;
```

- Decoding, `NOP_SPAWN` character branch after `get_str(r, s->party, ...)`:
  `for (int a = 0; a < NETMSG_ATTRS; a++) s->attr[a] = get_sv(r);`
- Decoding, `NOP_SET_STATS`:

```c
    case NOP_SET_STATS: {
        o->ids = get_ids(r, &o->nids);
        uint32_t mask = get_uv(r);
        if (mask > 0xFFFF) r->bad = true;
        o->stat_mask = (uint16_t)mask;
        int32_t v[9] = { 0 };
        for (int i = 0; i < 9; i++)
            if (o->stat_mask & (1 << i)) v[i] = get_sv(r);
        o->size = v[0];
        o->max_health = v[1];
        o->speed = v[2];
        o->initiative = v[3];
        o->max_stamina = v[4];
        for (int a = 0; a < NETMSG_ATTRS; a++) o->attr[a] = v[5 + a];
        break;
    }
```

- Decoding, `NM_CREATE` after `out->spec.max_stamina = get_sv(&r);`:
  `for (int a = 0; a < NETMSG_ATTRS; a++) out->spec.attr[a] = get_sv(&r);`
- Decoding, new case:

```c
    case NM_CREATE_ANSWER: {
        out->seq = get_uv(&r);
        uint8_t a = get_u8(&r);
        if (a > 1) r.bad = true;
        out->accepted = a == 1;
        break;
    }
```

(Use the reader variable names of the surrounding code: `r` is a struct in `netmsg_decode` and a pointer in
`get_op`.)

- [ ] **Step 4: Run the tests**

Run: `make test`
Expected: all suites `0 failed`. `make` (the game) may still fail to compile in `session.c` because `stat_mask`
widened; that is fixed in Task 5 — if it fails only there with a narrowing warning-as-error, continue.

- [ ] **Step 5: Commit**

```bash
git add src/netmsg.h src/netmsg.c tests/netmsg_test.c tests/core_test.c
git commit -m "Protocol 11: attributes in spawns, set-stats and create requests; NM_CREATE_ANSWER

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: Session: attributes, request answers, the client's waiting list

**Files:**
- Modify: `src/session.h`, `src/session.c`, `src/graphics.c`, `src/app.h`, `src/console.c`, `tests/core_stubs.c`

No unit test can reach `session.c` (it is not in CORE_SRC); Task 13's `tests/e2e_create.sh` covers this task.

- [ ] **Step 1: The session API**

`src/session.h`: add `#include "attrs.h"` and replace the `session_request_character` declaration with:

```c
#define SESSION_REQUESTS_MAX 3 /* a player's claims and new characters waiting for the GM, together */

/* Client: asks the GM for a new character of ours with this picture (at most NET_CREATE_MAX_DIM per side).
 * attr: its attributes (NULL = all 0). */
void session_request_character(const Image *pic, Hex tile, int size, int maxHealth, int speed, int initiative,
                               int maxStamina, const char *name, const int attr[ATTR_COUNT]);
/* Client: our create requests the GM has not answered yet, oldest first; their names ("" = no name). */
int         session_pending_count(void);
const char *session_pending_name(int i);
```

- [ ] **Step 2: Implement in `src/session.c`**

- `#define CLAIMS_PER_PLAYER 3` → `#define CLAIMS_PER_PLAYER SESSION_REQUESTS_MAX`.
- `Request`: add `uint32_t seq; /* REQ_CREATE: the player's CREATE seq, for the answer */`.
- In the session state struct `S`, in the `/* client */` part add:

```c
    DA(struct { uint32_t seq; char name[CHAR_NAME_MAX]; }) pending; /* our create requests the GM hasn't answered */
```

  If `DA(...)` with an anonymous struct does not compile, declare `typedef struct { uint32_t seq; char name[CHAR_NAME_MAX]; } PendingCreate;`
  above `S` and use `DA(PendingCreate) pending;`.
- `op_to_net`: after `o->max_stamina = op->maxStamina;` add `memcpy(o->attr, op->attr, sizeof(o->attr));` and
  change `o->stat_mask = (uint8_t)op->statMask;` to `o->stat_mask = (uint16_t)op->statMask;`. In its spec loop
  after `n->stamina = s->stamina;` add `memcpy(n->attr, s->attr, sizeof(n->attr));`.
- `op_from_net`: after `op->maxStamina = o->max_stamina;` add `memcpy(op->attr, o->attr, sizeof(op->attr));`;
  in its spec loop after the `EntitySpec s = {...}` initializer add `memcpy(s.attr, n->attr, sizeof(s.attr));`.
- Add above `host_create`:

```c
static void send_create_answer(Player *p, uint32_t seq, bool accepted) {
    NetMsg a = { .type = NM_CREATE_ANSWER, .seq = seq, .accepted = accepted };
    net_send(S.net, p->conn, &a);
}
```

- `host_create`: after the image check add the attribute rule and answer every refusal:

```c
    for (int a = 0; a < NETMSG_ATTRS && !rej.reason; a++)
        if (m->spec.attr[a] < ATTR_CREATE_MIN || m->spec.attr[a] > ATTR_CREATE_MAX) rej.reason = OPR_INVALID;
    if (rej.reason) {
        free(rgba);
        net_send(S.net, p->conn, &rej);
        send_create_answer(p, m->seq, false);
        return;
    }
```

  (merge this with the existing `if (rej.reason) {...}` block: the attribute loop goes before it, and the block
  gains the `send_create_answer` line). Note: the image is decoded before the attribute check; that is fine.
  After `Request r = {...}` add `r.seq = m->seq;` and after the `r.spec = (EntitySpec){...}` line add
  `memcpy(r.spec.attr, m->spec.attr, sizeof(r.spec.attr));`.
- `answer_create`: send an answer on every outcome:
  - in `if (!yes) { ... }` before `return;`: `send_create_answer(p, r->seq, false);`
  - in the "No room any more" block before `return;`: `send_create_answer(p, r->seq, false);`
  - after `send_log(p, "The GM accepted your character.");`: `send_create_answer(p, r->seq, true);`
- `ask_next_request`, REQ_CREATE question: build the attribute part and append it:

```c
            char at[40];
            snprintf(at, sizeof(at), " STR %d AGI %d COG %d SPI %d", c->spec.attr[ATTR_STR], c->spec.attr[ATTR_AGI],
                     c->spec.attr[ATTR_COG], c->spec.attr[ATTR_SPI]);
            snprintf(q, sizeof(q), "%s: new character%s [%d|%d] size %d HP %d speed %d init %d%s%s. Allow? (y/n)",
                     p->name, named, c->spec.tile.row, c->spec.tile.col, c->spec.size, c->spec.maxHealth,
                     c->spec.speed, c->spec.initiative, st, at);
```

- `session_request_character`: new signature (as in the header); after the `m.spec = (NetSpec){...}` line add
  `if (attr) memcpy(m.spec.attr, attr, sizeof(m.spec.attr));`. Replace the `else if (!net_send(...))` /
  `else console_log("Asked ...")` lines with:

```c
    else if (!net_send(S.net, 1, &m)) console_log("Could not send that to the host");
    else {
        console_log("Asked the GM to create your character.");
        PendingCreate pc = { .seq = m.seq };
        snprintf(pc.name, sizeof(pc.name), "%s", name ? name : "");
        da_push(&S.pending, pc);
    }
```

  (use `da_push(&S.pending, ((PendingCreate){ ... }))` style if the anonymous-struct DA is used.)
- Add the accessors (near `session_request_character`):

```c
int session_pending_count(void) { return S.role == ROLE_CLIENT ? S.pending.count : 0; }
const char *session_pending_name(int i) {
    return i >= 0 && i < session_pending_count() ? S.pending.items[i].name : "";
}
```

- The client's message switch (where `case NM_OP_REJECTED:` logs the reason) gets:

```c
    case NM_CREATE_ANSWER:
        for (int i = 0; i < S.pending.count; i++)
            if (S.pending.items[i].seq == m->seq) {
                da_remove(&S.pending, i); /* keeps the order */
                break;
            }
        break;
```

  Check `util.h` for the name of the order-keeping remove (`grep -n "define da_" src/util.h`); if there is none,
  shift the items down with `memmove` and decrement `count`.
- `end_client`: add `da_clear(&S.pending);`.

- [ ] **Step 3: Pass the attributes through the existing creation path**

`src/app.h`:
- `PendingAction`: after `maxStamina` add `int attr[ATTR_COUNT]; /* ACTION_SPAWN_CHARACTERS */`.
- `ActionKind`: add `ACTION_CREATE_PICTURE, /* the creation steps' picture (charcreate.h) */` before `ACTION_SAVE`.
- Change both declarations:

```c
void gfx_request_file(ActionKind kind, const HexList *tiles, int size, int maxHealth, int speed, int initiative,
                      int maxStamina, const char *name, const char *party, const int *attr);
void gfx_spawn_character(int size, int maxhealth, int speed, int initiative, int maxStamina, const char *name,
                         const char *party, const int *attr);
```

  (`attr`: `ATTR_COUNT` values or NULL = all 0.)

`src/graphics.c`:
- `gfx_request_file`: new last parameter; after copying `party` add:

```c
    for (int i = 0; i < ATTR_COUNT; i++) a->attr[i] = attr ? attr[i] : 0;
```

- `gfx_spawn_character`: new last parameter; in the confirm branch after copying `party` add
  `for (int i = 0; i < ATTR_COUNT; i++) c->attr[i] = attr ? attr[i] : 0;`; pass `attr` on to `gfx_request_file`.
- `spawn_confirmed` (the line with `gfx_request_file(ACTION_SPAWN_CHARACTERS, &c->tiles, ...)`): pass `c->attr`.
- `gfx_handle_file_result`, `ACTION_SPAWN_CHARACTERS`: pass `a->attr` as the new last argument of
  `session_request_character`, and in the `EntitySpec s = {...}` loop add `memcpy(s.attr, a->attr, sizeof(s.attr));`.
- Every other `gfx_request_file(...)` call (walls, icon, background, save/load: find them with
  `grep -n "gfx_request_file\|gfx_spawn_character" src/*.c`) gets `, NULL` as the last argument.

`src/console.c`: the `gfx_spawn_character(...)` call in `cmd_character` gets `, NULL` for now (Task 6 passes the
attributes); the `gfx_request_file(ACTION_CHARACTER_ICON, ...)` call gets `, NULL`.

`tests/core_stubs.c`: add stubs (Task 9 and 10 use them; adding them now keeps `make test` building):

```c
/* the file dialog and the player's request (graphics.c, session.c are not in the core tests) */
int stub_file_requests;
ActionKind stub_file_kind;
void gfx_request_file(ActionKind kind, const HexList *tiles, int size, int maxHealth, int speed, int initiative,
                      int maxStamina, const char *name, const char *party, const int *attr) {
    (void)tiles, (void)size, (void)maxHealth, (void)speed, (void)initiative, (void)maxStamina, (void)name,
        (void)party, (void)attr;
    stub_file_requests++;
    stub_file_kind = kind;
}
int stub_requests, stub_request_max_health, stub_request_attr[ATTR_COUNT];
char stub_request_name[CHAR_NAME_MAX];
Hex stub_request_tile;
void session_request_character(const Image *pic, Hex tile, int size, int maxHealth, int speed, int initiative,
                               int maxStamina, const char *name, const int attr[ATTR_COUNT]) {
    (void)pic, (void)size, (void)speed, (void)initiative, (void)maxStamina;
    stub_requests++;
    stub_request_tile = tile;
    stub_request_max_health = maxHealth;
    snprintf(stub_request_name, sizeof(stub_request_name), "%s", name ? name : "");
    for (int i = 0; i < ATTR_COUNT; i++) stub_request_attr[i] = attr ? attr[i] : 0;
}
int stub_pending;
int session_pending_count(void) { return stub_pending; }
const char *session_pending_name(int i) { return i < stub_pending ? "Mao" : ""; }
```

(`core_stubs.c` must `#include "image.h"` for `Image`; it already includes `session.h`, which includes it.)

- [ ] **Step 4: Build and test**

Run: `make -j && make test`
Expected: the game builds without warnings in the changed files; all suites `0 failed`.

- [ ] **Step 5: Commit**

```bash
git add src/session.h src/session.c src/graphics.c src/app.h src/console.c tests/core_stubs.c
git commit -m "Session: attributes over the wire, the host answers create requests, the client keeps its waiting list

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 6: `:c str / agi / cog / spi`

**Files:**
- Modify: `src/console.c`, `src/app.h`, `src/main.c`

Verification for this task is the `@logfile` check in Task 13 plus the manual run below (console.c is not in the
core tests).

- [ ] **Step 1: `console_write_log` and `@logfile`**

`src/app.h`, next to `console_log`: `void console_write_log(FILE *f); /* every log line, oldest first (scripts) */`
(`app.h` includes `<stdio.h>`? If not, add it.)

`src/console.c` after `console_log`:

```c
void console_write_log(FILE *f) {
    for (int i = 0; i < C->logs.count; i++) fprintf(f, "%s\n", C->logs.items[i]);
}
```

`src/main.c`, in `run_script_line` before the `touch` branch:

```c
    } else if (!strcmp(cmd, "logfile")) { /* the console log, for checks */
        FILE *f = fopen(arg, "w");
        if (f) {
            console_write_log(f);
            fclose(f);
        } else {
            fprintf(stderr, "@logfile: cannot write \"%s\"\n", arg);
        }
```

- [ ] **Step 2: Parse the attributes in `cmd_character`**

At the top of `cmd_character` add:

```c
    int attr[ATTR_COUNT] = { 0 };
    bool has_attr[ATTR_COUNT] = { 0 };
```

In the argument loop, before the `int *target = NULL;` line add:

```c
        int at = attr_by_name(key);
        if (at >= 0) {
            int v;
            if (!parse_int_arg(val, &v)) {
                console_log("Expected a number after \"%s\"", a->argv[i]);
                return;
            }
            /* a player's request (nothing selected) can't make a stronger character than the creation steps */
            bool request = session_is_client() && gh->selectedEntities.count == 0;
            int lo = request ? ATTR_CREATE_MIN : ATTR_MIN, hi = request ? ATTR_CREATE_MAX : ATTR_MAX;
            if (v < lo || v > hi) {
                console_log("Attributes go from %d to %d", lo, hi);
                return;
            }
            attr[at] = v;
            has_attr[at] = true;
            i += 2;
            continue;
        }
```

Creating (the `if (gh->selectedEntities.count == 0)` block): before the `gfx_spawn_character` call, fill the
derived stats that were not given by hand, and pass the attributes:

```c
        AttrStats d = attr_derive(attr);
        if (has_attr[ATTR_STR] && !has[1]) max_health = d.maxHealth;
        if (has_attr[ATTR_AGI] && !has[4]) max_stamina = d.maxStamina;
        if (has_attr[ATTR_AGI] && !has[2]) speed = d.speed;
        if (has_attr[ATTR_COG] && !has[3]) initiative = d.initiative;
```

and change the call's last argument from `NULL` to `attr`. (Put the `AttrStats` lines inside the `else` branch
that calls `gfx_spawn_character`, wrapped in braces.)

Editing: after the loop `for (int s = 0; s < 5; s++) if (has[s]) stats.statMask |= 1 << s;` add:

```c
    for (int at = 0; at < ATTR_COUNT; at++)
        if (has_attr[at]) {
            stats.statMask |= STAT_ATTR(at);
            stats.attr[at] = attr[at];
        }
```

(A player's edit is refused by `ops_submit` with "Only the GM can do that", as for the other stats.)

- [ ] **Step 3: Help and Tab completion**

`ARGS_CHARACTER`: add `{ "str", NULL }, { "agi", NULL }, { "cog", NULL }, { "spi", NULL },` before `{ NULL, NULL }`.

`cmd_help`, the `character` branch: after the `maxstamina` line add

```c
        console_log("  str <n> | agi <n> | cog <n> | spi <n> (-10..10): STR sets max HP (12 + 3 STR), AGI max");
        console_log("  stamina (8 + AGI) and speed (AGI + 3, at least 2), COG initiative - GM only on a selection");
```

- [ ] **Step 4: Build and try it**

Run: `make -j && ./toh --size 800x600 --auto-file resources/images/toolIcons/area.png --exec "@click 400 300" --exec ":c name Mao str 1 agi 2" --exec "@wait 5" --exec "@click 400 300" --exec ":c str 3" --exec "@logfile /tmp/claude-1000/toh-attr.log" --exec "@quit"; cat /tmp/claude-1000/toh-attr.log`
Expected: the log contains `Mao: STR 1 -> 3, max HP 15 -> 21`.

- [ ] **Step 5: Commit**

```bash
git add src/console.c src/app.h src/main.c
git commit -m ":c str / agi / cog / spi: attributes on the selection or a new character; @logfile script action

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 7: HUD element types: button, text field, stepper, choice, label, image

**Files:**
- Modify: `src/hud.h`, `src/hud.c`, `src/hud_widgets.c`, `src/hud_theme.h`, `src/hud_theme.c`, `tests/core_test.c`

- [ ] **Step 1: Write the failing test**

Add before `int main`, call `test_hud_controls();` after `test_hud_review();`. It reuses the file's HUD event
recorder (`hud_events`, `hud_nevents`, `hud_record`). The recorder keeps 8 events; this test sends more, so raise
it to 32 (both the array and the bound in `hud_record`):

```c
static HudEvent hud_events[32];
static int hud_nevents;
static void hud_record(const HudEvent *ev) {
    if (hud_nevents < 32) hud_events[hud_nevents] = *ev;
    hud_nevents++;
}
```

(`ev->text` points into the field's live text, so the checks read it right after the event.)

```c
static const HudElement CTRL_A[] = {
    { .type = HUD_TEXTFIELD, .key = "name", .accent = HUD_ACCENT_GOLD, .label = "Name", .width = 200, .max_len = 6 },
    { .type = HUD_BUTTON, .key = "ok", .accent = HUD_ACCENT_GREEN, .label = "OK", .width = 60 },
};
static const HudElement CTRL_B[] = {
    { .type = HUD_STEPPER, .key = "str", .accent = HUD_ACCENT_RED, .width = 120 },
    { .type = HUD_CHOICE, .key = "size", .accent = HUD_ACCENT_BLUE, .width = 210 },
};
static const HudElement CTRL_C[] = {
    { .type = HUD_IMAGE, .key = "pic", .width = 44 },
    { .type = HUD_LABEL, .key = "line", .width = 100 },
};
static const HudRow CTRL_ROWS[] = { { CTRL_A, 2 }, { CTRL_B, 2 }, { CTRL_C, 2 } };

static void click_part(const char *part) {
    HudRect r;
    CHECK(hud_part_rect(part, &r));
    float x = r.x + r.w / 2, y = r.y + r.h / 2;
    hud_mouse_move(x, y);
    hud_mouse_down(x, y);
    hud_mouse_up(x, y);
}

static void test_hud_controls(void) {
    char err[160];
    CHECK(hud_config_check(CTRL_ROWS, 3, err, sizeof(err)) == 0);
    static const HudElement NOWIDTH[] = { { .type = HUD_BUTTON, .key = "b" } };
    static const HudRow BAD[] = { { NOWIDTH, 1 } };
    CHECK(hud_config_check(BAD, 1, err, sizeof(err)) == -1 && strstr(err, "width"));
    static const HudElement LONG[] = { { .type = HUD_TEXTFIELD, .key = "t", .width = 50, .max_len = HUD_TEXT_MAX } };
    static const HudRow BAD2[] = { { LONG, 1 } };
    CHECK(hud_config_check(BAD2, 1, err, sizeof(err)) == -1);

    HudScreen scr = { 1000, 700, 700, 0, 1 };
    hud_set_screen(&scr);
    hud_use_config(CTRL_ROWS, 3);
    hud_set_visible(true);
    hud_set_event_handler(hud_record);
    hud_nevents = 0;
    hud_begin_frame();
    hud_set_textfield("name", "Ab");
    hud_set_button("ok", "OK", false, "Give a name");
    hud_set_stepper("str", "STR", 3, -2, 3);
    static const char *const OPTS[] = { "A", "B", "C" }, *const HINTS[] = { "a hint", "b hint", "c hint" };
    hud_set_choice("size", OPTS, HINTS, 3, 1);
    hud_set_label("line", "hello");

    /* a disabled button sends nothing */
    click_part("ok");
    CHECK(hud_nevents == 0);
    /* the field: a click focuses it with the published text; typing and editing send the new text */
    click_part("name");
    CHECK(hud_focused());
    CHECK(hud_text_input("c") && hud_nevents == 1 && hud_events[0].type == HUD_EV_TEXT && !strcmp(hud_events[0].key, "name"));
    CHECK(!strcmp(hud_events[0].text, "Abc"));
    CHECK(hud_text_key(HUD_EDIT_HOME) && hud_text_key(HUD_EDIT_DELETE) && !strcmp(hud_events[hud_nevents - 1].text, "bc"));
    CHECK(hud_text_key(HUD_EDIT_END) && hud_text_key(HUD_EDIT_BACKSPACE) && !strcmp(hud_events[hud_nevents - 1].text, "b"));
    CHECK(hud_text_input("\xc3\xa4") && hud_text_key(HUD_EDIT_BACKSPACE) && !strcmp(hud_events[hud_nevents - 1].text, "b"));
    int n = hud_nevents;
    CHECK(hud_text_input("123456") && hud_nevents == n); /* over max_len (6): ignored, still used */
    CHECK(hud_text_key(HUD_EDIT_ENTER) && hud_events[hud_nevents - 1].type == HUD_EV_SUBMIT);
    CHECK(hud_text_key(HUD_EDIT_ESCAPE) && !hud_focused());
    CHECK(!hud_text_input("x") && !hud_text_key(HUD_EDIT_LEFT)); /* not focused: not used */
    CHECK(hud_focus("name") && hud_focused() && !hud_focus("ok"));
    hud_mouse_down(5, 5); /* outside the HUD: blurs */
    hud_mouse_up(5, 5);
    CHECK(!hud_focused());

    /* stepper: + at the maximum does nothing, - sends -1 */
    n = hud_nevents;
    click_part("str.plus");
    CHECK(hud_nevents == n);
    click_part("str.minus");
    CHECK(hud_nevents == n + 1 && hud_events[n].type == HUD_EV_STEP && hud_events[n].value == -1);

    /* choice: a click on another option sends its index */
    HudRect c;
    CHECK(hud_part_rect("size", &c));
    float x = c.x + c.w * 5 / 6, y = c.y + c.h / 2; /* the third option */
    hud_mouse_move(x, y);
    hud_mouse_down(x, y);
    hud_mouse_up(x, y);
    CHECK(hud_events[hud_nevents - 1].type == HUD_EV_CHOICE && hud_events[hud_nevents - 1].value == 2);
    hud_mouse_move(x, y);
    CHECK(hud_tooltip() && !strcmp(hud_tooltip(), "c hint"));

    /* an enabled button sends HUD_EV_BUTTON; a disabled one's tooltip is its reason */
    HudRect ok;
    CHECK(hud_part_rect("ok", &ok));
    hud_mouse_move(ok.x + 2, ok.y + 2);
    CHECK(hud_tooltip() && !strcmp(hud_tooltip(), "Give a name"));
    hud_begin_frame();
    hud_set_button("ok", "OK", true, NULL);
    n = hud_nevents;
    click_part("ok");
    CHECK(hud_nevents == n + 1 && hud_events[n].type == HUD_EV_BUTTON && !strcmp(hud_events[n].key, "ok"));

    hud_draw(); /* every component draws with and without values */
    hud_begin_frame();
    hud_draw();
    hud_set_event_handler(NULL);
    hud_use_config(NULL, 0);
}
```

- [ ] **Step 2: Run it to see it fail**

Run: `make build/core_test`
Expected: compile errors (`HUD_TEXTFIELD`, `hud_set_textfield`, ... undeclared).

- [ ] **Step 3: Theme tokens**

`src/hud_theme.h`, after the hotbar tokens add:

```c
    /* controls: buttons, text fields, steppers, choices; tooltips; the "+" box */
    float controlH, stepperLabelW, stepperBtn, caretW, tipPad, tipGap, plusSize;
```

`src/hud_theme.c`, after the hotbar line add:

```c
    .controlH = 26, .stepperLabelW = 36, .stepperBtn = 22, .caretW = 1.5f, .tipPad = 6, .tipGap = 6, .plusSize = 44,
```

- [ ] **Step 4: Types and API in `src/hud.h`**

- `HudElementType`: `HUD_BAR, HUD_PILL, HUD_SLIDER, HUD_HOTBAR, HUD_BUTTON, HUD_TEXTFIELD, HUD_STEPPER, HUD_CHOICE, HUD_LABEL, HUD_IMAGE, HUD_ELEMENT_COUNT`
- `HudElement`: add

```c
    float width;  /* button, text field, stepper, choice, label, image: width in logical pixels (image: also height) */
    int max_len;  /* text field: at most this many bytes (0 = HUD_TEXT_MAX - 1) */
```

  and change the `label` comment to `/* placeholder text (pill, slider, button, text field when empty, label); may be NULL */`.
- Values, after `HudSlider`:

```c
#define HUD_MAX_CHOICES 8
#define HUD_LABEL_MAX 128
typedef struct { char label[HUD_TEXT_MAX]; bool enabled; char why[2 * HUD_TEXT_MAX]; } HudButton;
typedef struct { char text[HUD_TEXT_MAX]; } HudText;
typedef struct { char label[HUD_TEXT_MAX]; int value, min, max; } HudStepper;
typedef struct {
    char options[HUD_MAX_CHOICES][16];
    char hints[HUD_MAX_CHOICES][HUD_TEXT_MAX]; /* hover text per option; "" = none */
    int count, selected;
} HudChoice;
typedef struct { char text[HUD_LABEL_MAX]; } HudLabel;
typedef struct { Texture *tex; } HudImage; /* borrowed; must stay alive for the frame */
```

  and in `HudValue`'s union add `HudButton button; HudText text; HudStepper stepper; HudChoice choice; HudLabel label; HudImage image;`.
- Setters after `hud_set_slot`:

```c
void hud_set_button(const char *key, const char *label, bool enabled, const char *why); /* why: tooltip when disabled */
void hud_set_textfield(const char *key, const char *text); /* the text while the field has no focus */
void hud_set_stepper(const char *key, const char *label, int value, int min, int max);
void hud_set_choice(const char *key, const char *const *options, const char *const *hints, int n, int selected);
void hud_set_label(const char *key, const char *text);
void hud_set_image(const char *key, Texture *tex);
```

- `HudEventType`: `HUD_EV_TARGET_CLOSED, HUD_EV_SLOT_USED, HUD_EV_SLIDER_CHANGED, HUD_EV_BUTTON, HUD_EV_TEXT, HUD_EV_SUBMIT, HUD_EV_STEP, HUD_EV_CHOICE, HUD_EV_PLUS`
- `HudEvent`: change `value`'s comment to `/* SLIDER_CHANGED; STEP: -1 or +1; CHOICE: the option */` and add
  `const char *text; /* TEXT: the field's new text, valid during the handler */`.
- Text editing, after `hud_key`:

```c
/* Text fields: a left press on one focuses it, a press anywhere else (or Esc) takes the focus away. While a field
 * has the focus, the game sends it text and editing keys; it sends HUD_EV_TEXT on every change, HUD_EV_SUBMIT on
 * Enter. Changing the config or hiding the HUD drops the focus. */
typedef enum { HUD_EDIT_LEFT, HUD_EDIT_RIGHT, HUD_EDIT_HOME, HUD_EDIT_END, HUD_EDIT_BACKSPACE, HUD_EDIT_DELETE,
               HUD_EDIT_ENTER, HUD_EDIT_ESCAPE } HudEditKey;
bool hud_focus(const char *key); /* focuses that text field with its published text; false if there is none */
void hud_blur(void);
bool hud_focused(void);
bool hud_text_input(const char *utf8); /* true = used (the field has the focus) */
bool hud_text_key(HudEditKey k);       /* true = used */
/* The hovered element's or tab's tooltip, or NULL (tests; hud_draw draws it). */
const char *hud_tooltip(void);
```

- `HudUiState`: add `int focus; /* the focused text field's flat index, -1 = none */ const char *editText; int caret;`
  and `bool hoverTab, hoverPlus, pressPlus;` (the last three are used in Task 8).
- `HudComponent`: add after `mouse`:

```c
    /* Optional: the tooltip while the pointer is over the element (ui->hoverSub says where inside), or NULL. */
    const char *(*tooltip)(const HudElement *e, const HudValue *v, const HudUiState *ui, int self);
```

- After `hud_slot_usable` add `HudRect hud_stepper_button(HudRect stepper, bool plus, float scale);` and
  `void hud_draw_tooltip(HudRect r, float scale, const char *text); /* a small panel with the text */`.
- In the "How to" comment, add: `Text fields, buttons and the other controls: see hud_create.c for a config that uses them.`

- [ ] **Step 5: Core in `src/hud.c`**

- `hud_config_check`, inside the element loop after the accent check:

```c
            bool sized = e->type == HUD_BUTTON || e->type == HUD_TEXTFIELD || e->type == HUD_STEPPER ||
                         e->type == HUD_CHOICE || e->type == HUD_LABEL || e->type == HUD_IMAGE;
            if (sized && !(e->width > 0)) {
                snprintf(err, cap, "row %d element %d: needs a width", i + 1, k + 1);
                return -1;
            }
            if (e->type == HUD_TEXTFIELD && (e->max_len < 0 || e->max_len > HUD_TEXT_MAX - 1)) {
                snprintf(err, cap, "row %d element %d: max_len 0 to %d", i + 1, k + 1, HUD_TEXT_MAX - 1);
                return -1;
            }
```

  and extend the reserved-key check with `|| strcmp(e->key, "plus") == 0`.
- Setters after `hud_set_slot`:

```c
void hud_set_button(const char *key, const char *label, bool enabled, const char *why) {
    HudValue *v = value_for(key, HUD_BUTTON);
    if (!v) return;
    snprintf(v->button.label, sizeof(v->button.label), "%s", label ? label : "");
    v->button.enabled = enabled;
    snprintf(v->button.why, sizeof(v->button.why), "%s", why ? why : "");
}

void hud_set_textfield(const char *key, const char *text) {
    HudValue *v = value_for(key, HUD_TEXTFIELD);
    if (v) snprintf(v->text.text, sizeof(v->text.text), "%s", text ? text : "");
}

void hud_set_stepper(const char *key, const char *label, int value, int min, int max) {
    HudValue *v = value_for(key, HUD_STEPPER);
    if (!v) return;
    snprintf(v->stepper.label, sizeof(v->stepper.label), "%s", label ? label : "");
    v->stepper.value = value;
    v->stepper.min = min;
    v->stepper.max = max;
}

void hud_set_choice(const char *key, const char *const *options, const char *const *hints, int n, int selected) {
    HudValue *v = value_for(key, HUD_CHOICE);
    if (!v) return;
    if (n > HUD_MAX_CHOICES) n = HUD_MAX_CHOICES;
    v->choice.count = n < 0 ? 0 : n;
    v->choice.selected = selected;
    for (int i = 0; i < v->choice.count; i++) {
        snprintf(v->choice.options[i], sizeof(v->choice.options[i]), "%s", options[i]);
        snprintf(v->choice.hints[i], sizeof(v->choice.hints[i]), "%s", hints && hints[i] ? hints[i] : "");
    }
}

void hud_set_label(const char *key, const char *text) {
    HudValue *v = value_for(key, HUD_LABEL);
    if (v) snprintf(v->label.text, sizeof(v->label.text), "%s", text ? text : "");
}

void hud_set_image(const char *key, Texture *tex) {
    HudValue *v = value_for(key, HUD_IMAGE);
    if (v) v->image.tex = tex;
}
```

- `ui` initializer and `ui_reset`: the struct now has more fields; write it with designated initializers:

```c
static const HudUiState UI_NONE = { .hover = -1, .press = -1, .hoverSub = -1, .pressSub = -1, .focus = -1, .editText = "" };
static HudUiState ui = { .hover = -1, .press = -1, .hoverSub = -1, .pressSub = -1, .focus = -1, .editText = "" };
```

  and in `ui_reset`: `ui = UI_NONE; pressActive = false; edit.idx = -1;` (declare `edit` above `ui_reset`, below).
  `hud_clear_hover` also clears `ui.hoverTab = ui.hoverPlus = false;`.
- Editing state and functions (after `ui_reset`'s declarations, before the input section):

```c
/* the focused text field: its flat element index, its text and the caret (a byte offset) */
static struct { int idx; char text[HUD_TEXT_MAX]; int caret; } edit = { -1, "", 0 };

static void sync_edit(void) {
    ui.focus = edit.idx;
    ui.editText = edit.text;
    ui.caret = edit.caret;
}

static void focus_index(int idx) {
    HudValue *v = element_value(element(idx));
    edit.idx = idx;
    snprintf(edit.text, sizeof(edit.text), "%s", v ? v->text.text : "");
    edit.caret = (int)strlen(edit.text);
    sync_edit();
}

bool hud_focus(const char *key) {
    if (!active()) return false;
    ensure_layout();
    for (int i = 0; i < row_count(); i++)
        for (int k = 0; k < rows[i].count; k++)
            if (rows[i].elements[k].type == HUD_TEXTFIELD && strcmp(rows[i].elements[k].key, key) == 0) {
                focus_index(i * HUD_MAX_ROW_ELEMENTS + k);
                return true;
            }
    return false;
}

void hud_blur(void) {
    edit.idx = -1;
    sync_edit();
}

bool hud_focused(void) { return edit.idx >= 0 && active(); }

static int field_max(const HudElement *e) { return e->max_len > 0 ? e->max_len : HUD_TEXT_MAX - 1; }

static void text_changed(void) {
    sync_edit();
    send(&(HudEvent){ .type = HUD_EV_TEXT, .key = element(edit.idx)->key, .text = edit.text });
}

bool hud_text_input(const char *s) {
    if (!hud_focused()) return false;
    size_t n = strlen(s), len = strlen(edit.text);
    if (n == 0 || len + n > (size_t)field_max(element(edit.idx))) return true; /* too long: ignored */
    memmove(edit.text + edit.caret + n, edit.text + edit.caret, len - (size_t)edit.caret + 1);
    memcpy(edit.text + edit.caret, s, n);
    edit.caret += (int)n;
    text_changed();
    return true;
}

/* UTF-8: the start of the character before / after byte i */
static int utf8_prev(const char *t, int i) {
    if (i <= 0) return 0;
    do i--; while (i > 0 && ((unsigned char)t[i] & 0xC0) == 0x80);
    return i;
}
static int utf8_next(const char *t, int i) {
    int n = (int)strlen(t);
    if (i >= n) return n;
    do i++; while (i < n && ((unsigned char)t[i] & 0xC0) == 0x80);
    return i;
}

bool hud_text_key(HudEditKey k) {
    if (!hud_focused()) return false;
    int len = (int)strlen(edit.text);
    switch (k) {
    case HUD_EDIT_LEFT: edit.caret = utf8_prev(edit.text, edit.caret); break;
    case HUD_EDIT_RIGHT: edit.caret = utf8_next(edit.text, edit.caret); break;
    case HUD_EDIT_HOME: edit.caret = 0; break;
    case HUD_EDIT_END: edit.caret = len; break;
    case HUD_EDIT_BACKSPACE:
        if (edit.caret > 0) {
            int p = utf8_prev(edit.text, edit.caret);
            memmove(edit.text + p, edit.text + edit.caret, (size_t)(len - edit.caret + 1));
            edit.caret = p;
            text_changed();
            return true;
        }
        break;
    case HUD_EDIT_DELETE:
        if (edit.caret < len) {
            int nx = utf8_next(edit.text, edit.caret);
            memmove(edit.text + edit.caret, edit.text + nx, (size_t)(len - nx + 1));
            text_changed();
            return true;
        }
        break;
    case HUD_EDIT_ENTER:
        send(&(HudEvent){ .type = HUD_EV_SUBMIT, .key = element(edit.idx)->key });
        return true;
    case HUD_EDIT_ESCAPE:
        hud_blur();
        return true;
    }
    sync_edit();
    return true;
}
```

  (`element_value` and `send` are defined further down in hud.c; move these functions below them, or add
  `static HudValue *element_value(const HudElement *e);` and `static void send(const HudEvent *ev);` forward
  declarations above.)
- `hud_mouse_down`: focus or blur. Replace the end of the function:

```c
    if (!hud_rect_contains(layout.panel, x, y)) {
        hud_blur();
        return false;
    }
    pressActive = true;
    ui.press = element_at(x, y);
    if (ui.press >= 0 && element(ui.press)->type == HUD_TEXTFIELD) {
        if (edit.idx != ui.press) focus_index(ui.press);
    } else {
        hud_blur();
    }
    if (ui.press >= 0) element_mouse(ui.press, HUD_MOUSE_DOWN, x, y);
    return true;
```

  and the tab branch above it also calls `hud_blur();` before `return true;`. The early `if (!active()) return false;`
  stays as is.
- `hud_set_visible`: `ui_reset()` already drops the focus (it clears `edit.idx`).
- `hud_part_rect`: before the hotbar `"<key>.<slot>"` parsing add the stepper buttons:

```c
    const char *sdot = strrchr(part, '.');
    if (sdot && (strcmp(sdot, ".minus") == 0 || strcmp(sdot, ".plus") == 0))
        for (int i = 0; i < row_count(); i++)
            for (int k = 0; k < rows[i].count; k++) {
                const HudElement *e = &rows[i].elements[k];
                if (e->type == HUD_STEPPER && strlen(e->key) == (size_t)(sdot - part) &&
                    strncmp(e->key, part, (size_t)(sdot - part)) == 0) {
                    *out = hud_stepper_button(layout.elems[i][k], sdot[1] == 'p', layout.scale);
                    return true;
                }
            }
```

- Tooltips:

```c
const char *hud_tooltip(void) {
    if (!active()) return NULL;
    ensure_layout();
    if (ui.hover < 0) return NULL;
    const HudElement *e = element(ui.hover);
    const HudComponent *c = component(e);
    return c->tooltip ? c->tooltip(e, element_value(e), &ui, ui.hover) : NULL;
}
```

  At the end of `hud_draw`:

```c
    const char *tip = hud_tooltip();
    if (tip) {
        float fs = HUD_THEME.font * s, pad = HUD_THEME.tipPad * s;
        float w = dr_text_width(FONT_PLAIN, fs, tip) + 2 * pad, h = fs + 2 * pad;
        HudRect over = ui.hover >= 0 ? element_rect(ui.hover) : layout.tab;
        float x = over.x + over.w / 2 - w / 2, y = (ui.hover >= 0 ? layout.panel.y : layout.tab.y) - HUD_THEME.tipGap * s - h;
        if (ui.hoverTab) y = layout.tab.y + layout.tab.h + HUD_THEME.tipGap * s; /* below the tab, over the panel */
        if (x + w > screen.winW - 2) x = screen.winW - 2 - w;
        if (x < screen.left + 2) x = screen.left + 2;
        hud_draw_tooltip((HudRect){ x, y, w, h }, s, tip);
    }
```

  (`hud.c` must `#include "draw.h"` for `dr_text_width` and `FONT_PLAIN`.)

- [ ] **Step 6: Components in `src/hud_widgets.c`**

Add before the component map:

```c
/* ---- controls: button, text field, stepper, choice, label, image ---- */

static float fixed_width(const HudElement *e, float s) { return e->width * s; }
static float control_height(const HudElement *e, float s) { return T->controlH * s; }
static float image_height(const HudElement *e, float s) { return e->width * s; }

static void control_frame(HudRect r, float s, Color bg, Color line) {
    round_rect(r, T->slotRadius * s, bg);
    stroke_round_rect(r, T->slotRadius * s, fmaxf(1, s), line);
}

static bool button_enabled(const HudValue *v) { return v && v->button.enabled; }

static void button_draw(const HudElement *e, HudRect r, const HudValue *v, const HudUiState *ui, int self, float s) {
    bool on = button_enabled(v), hover = on && ui->hover == self, pressed = hover && ui->press == self;
    Color line = pressed ? T->pressed : hover ? T->hover : on ? T->accent[e->accent] : T->emptyLine;
    control_frame(r, s, pressed ? T->veil : T->slotBg, line);
    const char *label = v && v->button.label[0] ? v->button.label : e->label ? e->label : "";
    text_centered(FONT_BOLD, T->font * s, r, on ? T->text : T->textDim, label);
}

static bool button_mouse(const HudElement *e, HudRect r, HudValue *v, HudMouse m, HudUiState *ui, int self,
                         float s, HudEvent *ev) {
    if (m.kind != HUD_MOUSE_UP || !button_enabled(v) || !hud_rect_contains(r, m.x, m.y)) return false;
    ev->type = HUD_EV_BUTTON;
    return true;
}

static const char *button_tip(const HudElement *e, const HudValue *v, const HudUiState *ui, int self) {
    return v && !v->button.enabled && v->button.why[0] ? v->button.why : NULL;
}

static void field_draw(const HudElement *e, HudRect r, const HudValue *v, const HudUiState *ui, int self, float s) {
    bool focused = ui->focus == self;
    control_frame(r, s, T->barBg, focused ? T->accent[e->accent] : ui->hover == self ? T->hover : T->emptyLine);
    const char *text = focused ? ui->editText : v ? v->text.text : "";
    float x = r.x + T->pillPad * s, fs = T->font * s;
    if (text[0]) text_left(FONT_PLAIN, fs, x, r, T->text, text);
    else if (!focused && e->label) text_left(FONT_PLAIN, fs, x, r, T->textDim, e->label);
    if (focused) {
        char before[HUD_TEXT_MAX];
        snprintf(before, sizeof(before), "%.*s", ui->caret, text);
        float cx = x + dr_text_width(FONT_PLAIN, fs, before);
        dr_line(cx, r.y + r.h * 0.2f, cx, r.y + r.h * 0.8f, fmaxf(1, T->caretW * s), T->text);
    }
}

HudRect hud_stepper_button(HudRect r, bool plus, float s) {
    float b = T->stepperBtn * s, y = r.y + (r.h - b) / 2;
    return plus ? (HudRect){ r.x + r.w - b, y, b, b } : (HudRect){ r.x + T->stepperLabelW * s, y, b, b };
}

static bool stepper_can(const HudValue *v, bool plus) {
    return v && (plus ? v->stepper.value < v->stepper.max : v->stepper.value > v->stepper.min);
}

/* 0 = minus, 1 = plus, -1 = neither */
static int stepper_at(HudRect r, float x, float y, float s) {
    if (hud_rect_contains(hud_stepper_button(r, false, s), x, y)) return 0;
    if (hud_rect_contains(hud_stepper_button(r, true, s), x, y)) return 1;
    return -1;
}

static void stepper_draw(const HudElement *e, HudRect r, const HudValue *v, const HudUiState *ui, int self, float s) {
    const char *label = v && v->stepper.label[0] ? v->stepper.label : e->label ? e->label : "";
    text_left(FONT_BOLD, T->font * s, r.x, r, T->accent[e->accent], label);
    for (int p = 0; p < 2; p++) {
        HudRect b = hud_stepper_button(r, p == 1, s);
        bool on = stepper_can(v, p == 1), hover = on && ui->hover == self && ui->hoverSub == p;
        bool pressed = hover && ui->press == self && ui->pressSub == p;
        control_frame(b, s, pressed ? T->veil : T->slotBg, hover ? T->hover : on ? T->emptyLine : T->veil);
        text_centered(FONT_BOLD, T->font * s, b, on ? T->text : T->textDim, p ? "+" : "-");
    }
    HudRect minus = hud_stepper_button(r, false, s), plus = hud_stepper_button(r, true, s);
    HudRect mid = { minus.x + minus.w, r.y, plus.x - (minus.x + minus.w), r.h };
    char val[16];
    snprintf(val, sizeof(val), "%d", v ? v->stepper.value : 0);
    text_centered(FONT_BOLD, T->font * s, mid, v ? T->text : T->textDim, val);
}

static bool stepper_mouse(const HudElement *e, HudRect r, HudValue *v, HudMouse m, HudUiState *ui, int self,
                          float s, HudEvent *ev) {
    int at = stepper_at(r, m.x, m.y, s);
    switch (m.kind) {
    case HUD_MOUSE_DOWN: ui->pressSub = at; return false;
    case HUD_MOUSE_MOVE: ui->hoverSub = at; return false;
    case HUD_MOUSE_UP:
        if (at < 0 || at != ui->pressSub || !stepper_can(v, at == 1)) return false;
        ev->type = HUD_EV_STEP;
        ev->value = at == 1 ? 1 : -1;
        return true;
    }
    return false;
}

static HudRect choice_option(HudRect r, int i, int n, float s) {
    float g = T->slotGap * s, w = n > 0 ? (r.w - (float)(n - 1) * g) / (float)n : r.w;
    return (HudRect){ r.x + (float)i * (w + g), r.y, w, r.h };
}

static int choice_at(const HudValue *v, HudRect r, float x, float y, float s) {
    int n = v ? v->choice.count : 0;
    for (int i = 0; i < n; i++)
        if (hud_rect_contains(choice_option(r, i, n, s), x, y)) return i;
    return -1;
}

static void choice_draw(const HudElement *e, HudRect r, const HudValue *v, const HudUiState *ui, int self, float s) {
    if (!v || v->choice.count == 0) {
        control_frame(r, s, T->slotBg, T->emptyLine);
        return;
    }
    for (int i = 0; i < v->choice.count; i++) {
        HudRect o = choice_option(r, i, v->choice.count, s);
        bool sel = i == v->choice.selected, hover = ui->hover == self && ui->hoverSub == i;
        control_frame(o, s, sel ? T->accent[e->accent] : T->slotBg, hover ? T->hover : sel ? T->accent[e->accent] : T->emptyLine);
        text_centered(sel ? FONT_BOLD : FONT_PLAIN, T->smallFont * s, o, T->text, v->choice.options[i]);
    }
}

static bool choice_mouse(const HudElement *e, HudRect r, HudValue *v, HudMouse m, HudUiState *ui, int self,
                         float s, HudEvent *ev) {
    int at = choice_at(v, r, m.x, m.y, s);
    switch (m.kind) {
    case HUD_MOUSE_DOWN: ui->pressSub = at; return false;
    case HUD_MOUSE_MOVE: ui->hoverSub = at; return false;
    case HUD_MOUSE_UP:
        if (at < 0 || at != ui->pressSub || at == v->choice.selected) return false;
        ev->type = HUD_EV_CHOICE;
        ev->value = at;
        return true;
    }
    return false;
}

static const char *choice_tip(const HudElement *e, const HudValue *v, const HudUiState *ui, int self) {
    int i = ui->hoverSub;
    return v && i >= 0 && i < v->choice.count && v->choice.hints[i][0] ? v->choice.hints[i] : NULL;
}

static void label_draw(const HudElement *e, HudRect r, const HudValue *v, const HudUiState *ui, int self, float s) {
    const char *text = v ? v->label.text : e->label ? e->label : "";
    if (text[0]) text_left(FONT_PLAIN, T->font * s, r.x, r, v ? T->text : T->textDim, text);
}

static void image_draw(const HudElement *e, HudRect r, const HudValue *v, const HudUiState *ui, int self, float s) {
    if (v && v->image.tex) dr_image(v->image.tex, r.x, r.y, r.w, r.h);
    else control_frame(r, s, T->slotBg, T->emptyLine);
}

void hud_draw_tooltip(HudRect r, float s, const char *text) {
    hud_draw_panel(r, s * 0.6f);
    text_centered(FONT_PLAIN, T->font * s, r, T->text, text);
}
```

Component map additions (field order: flex, min_width, height, draw, mouse, tooltip):

```c
static const HudComponent BUTTON = { false, fixed_width, control_height, button_draw, button_mouse, button_tip };
static const HudComponent TEXTFIELD = { false, fixed_width, control_height, field_draw, NULL, NULL };
static const HudComponent STEPPER = { false, fixed_width, control_height, stepper_draw, stepper_mouse, NULL };
static const HudComponent CHOICE = { false, fixed_width, control_height, choice_draw, choice_mouse, choice_tip };
static const HudComponent LABEL = { true, fixed_width, control_height, label_draw, NULL, NULL };
static const HudComponent IMAGE = { false, fixed_width, image_height, image_draw, NULL, NULL };
```

and in `HUD_COMPONENTS`: `[HUD_BUTTON] = &BUTTON, [HUD_TEXTFIELD] = &TEXTFIELD, [HUD_STEPPER] = &STEPPER, [HUD_CHOICE] = &CHOICE, [HUD_LABEL] = &LABEL, [HUD_IMAGE] = &IMAGE,`.

Note: `hud_mouse_up` only delivers `HUD_MOUSE_UP` to the element the press started on; `button_mouse` checks
the pointer is still inside. The text field has no `mouse` callback: `hud.c` focuses it on the press.

- [ ] **Step 7: Run the tests**

Run: `make test`
Expected: all suites `0 failed`, including the existing HUD tests (their configs have no new types).

- [ ] **Step 8: Commit**

```bash
git add src/hud.h src/hud.c src/hud_widgets.c src/hud_theme.h src/hud_theme.c tests/core_test.c
git commit -m "HUD: button, text field, stepper, choice, label and image elements; focus, editing and tooltips

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 8: HUD "+" part and the tab tooltip

**Files:**
- Modify: `src/hud.h`, `src/hud.c`, `tests/core_test.c`

- [ ] **Step 1: Write the failing test**

Add before `int main`, call `test_hud_plus();` after `test_hud_controls();`:

```c
static void test_hud_plus(void) {
    HudScreen scr = { 1000, 700, 700, 0, 1 };
    hud_set_screen(&scr);
    hud_use_config(NULL, 0);
    hud_set_visible(true);
    hud_set_event_handler(hud_record);
    hud_nevents = 0;
    hud_begin_frame();
    HudRect plus, panel;
    CHECK(!hud_part_rect("plus", &plus)); /* not published this frame */
    hud_set_plus(true);
    CHECK(hud_part_rect("plus", &plus) && hud_part_rect("panel", &panel));
    CHECK(plus.x + plus.w < panel.x && near(plus.y + plus.h, panel.y + panel.h, 0.01f)); /* bottom left of the bar */
    CHECK(hud_contains(plus.x + 2, plus.y + 2));
    click_part("plus");
    CHECK(hud_nevents == 1 && hud_events[0].type == HUD_EV_PLUS);
    /* a press on "+" released elsewhere does nothing */
    hud_mouse_down(plus.x + 2, plus.y + 2);
    hud_mouse_up(5, 5);
    CHECK(hud_nevents == 1);
    /* the tab's tooltip */
    hud_set_target("Mao", "", false);
    hud_set_target_tip("COG 1 · SPI 0");
    HudRect tab;
    CHECK(hud_part_rect("tab", &tab));
    hud_mouse_move(tab.x + 2, tab.y + 2);
    CHECK(hud_tooltip() && !strcmp(hud_tooltip(), "COG 1 · SPI 0"));
    hud_draw();
    /* a narrow window: the bar moves right to make room for "+" */
    HudScreen narrow = { 420, 700, 700, 32, 1 };
    hud_set_screen(&narrow);
    CHECK(hud_part_rect("plus", &plus) && plus.x >= 32);
    hud_set_event_handler(NULL);
    hud_begin_frame();
}
```

- [ ] **Step 2: Run it to see it fail**

Run: `make build/core_test`
Expected: compile errors (`hud_set_plus`, `hud_set_target_tip`).

- [ ] **Step 3: Implement**

`src/hud.h`:
- `HudTarget`: add `char tip[HUD_TEXT_MAX]; /* tooltip while the pointer is over the tab; "" = none */`.
- After `hud_set_target`: `void hud_set_target_tip(const char *tip);` and
  `void hud_set_plus(bool shown); /* this frame: the "+" box at the bottom left of the panel (it sends HUD_EV_PLUS) */`.
- `hud_part_rect`'s comment: add `"plus"` to the list of parts.

`src/hud.c`:
- Values: `static bool plus;` next to `target`; `hud_begin_frame` adds `plus = false;`.

```c
void hud_set_target_tip(const char *tip) { snprintf(target.tip, sizeof(target.tip), "%s", tip ? tip : ""); }
void hud_set_plus(bool shown) { plus = shown; }
```

- `Layout`: add `bool plus; HudRect plusRect;`. In `layout_compute`, right after the panel's `x` is computed
  (after `if (x < screen.left + t->margin * s) ...`):

```c
    float side = t->plusSize * s, gap = t->elemGap * s;
    if (plus && x < screen.left + t->margin * s + side + gap) x = screen.left + t->margin * s + side + gap;
```

  after `layout.panel = ...`:

```c
    layout.plusRect = plus ? (HudRect){ x - gap - side, y + h - side, side, side } : (HudRect){ 0 };
    layout.plus = plus;
```

- `ensure_layout`: add `layout.plus == plus &&` to the condition. `same_target` also compares
  `strcmp(a->tip, b->tip) == 0` (cheap; keeps the cache honest).
- `hud_part_rect`: after the tab branch:

```c
    if (strcmp(part, "plus") == 0) {
        if (!plus) return false;
        *out = layout.plusRect;
        return true;
    }
```

- `hud_contains`: `|| (plus && hud_rect_contains(layout.plusRect, x, y))`.
- `refresh_hover`: add
  `ui.hoverTab = target.shown && hud_rect_contains(layout.tab, x, y);`
  `ui.hoverPlus = plus && hud_rect_contains(layout.plusRect, x, y);`
- `hud_mouse_down`, before the tab check:

```c
    if (plus && hud_rect_contains(layout.plusRect, x, y)) {
        hud_blur();
        ui.pressPlus = true;
        pressActive = true;
        return true;
    }
```

- `hud_cancel_press`: `ui.pressPlus = false;`.
- `hud_mouse_up`: read `bool plusPressed = ui.pressPlus; ui.pressPlus = false;` next to `close`, and inside
  `if (active())` after the close check:

```c
        if (plusPressed && plus && hud_rect_contains(layout.plusRect, x, y)) send(&(HudEvent){ .type = HUD_EV_PLUS });
```

- `hud_tooltip`: add the line `if (ui.hoverTab && target.shown && target.tip[0]) return target.tip;` before the
  element lookup (as written in Task 7).
- `hud_draw`, after `hud_draw_tab(...)`:

```c
    if (plus) {
        HudRect p = layout.plusRect;
        hud_draw_panel(p, s);
        Color c = ui.pressPlus && ui.hoverPlus ? HUD_THEME.pressed : ui.hoverPlus ? HUD_THEME.hover : HUD_THEME.text;
        float fs = HUD_THEME.font * 2 * s, a, d;
        dr_font_metrics(FONT_BOLD, fs, &a, &d);
        dr_text(FONT_BOLD, fs, p.x + (p.w - dr_text_width(FONT_BOLD, fs, "+")) / 2, p.y + (p.h + a - d) / 2, c, "+");
    }
```

- [ ] **Step 4: Run the tests**

Run: `make test`
Expected: all suites `0 failed`.

- [ ] **Step 5: Commit**

```bash
git add src/hud.h src/hud.c tests/core_test.c
git commit -m "HUD: the \"+\" part left of the panel and a tooltip on the tab

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 9: The creation draft and steps (`charcreate.c`)

**Files:**
- Create: `src/charcreate.h`, `src/charcreate.c`
- Modify: `Makefile` (CORE_SRC), `tests/core_test.c`

- [ ] **Step 1: Write the failing test**

Add `#include "charcreate.h"` to `tests/core_test.c`, the externs below next to the other `extern`s, the test
before `int main`, and call `test_char_create();` after `test_attribute_ops();`:

```c
extern int stub_file_requests, stub_requests, stub_request_max_health, stub_request_attr[ATTR_COUNT], stub_pending;
extern ActionKind stub_file_kind;
extern char stub_request_name[CHAR_NAME_MAX];
extern Hex stub_request_tile;
extern bool stub_client;
```

(`stub_client` may already be declared; keep one declaration.)

```c
static Entity *last_entity(void) { return app.entities.items[app.entities.count - 1]; }

static void test_char_create(void) {
    reset_table();
    stub_client = false;
    const CharCreate *c = cc_state();
    CHECK(!c->active);
    const char *why = NULL;

    cc_start(NULL);
    CHECK(c->active && c->step == CC_NAME && c->size == SIZE_NORMAL && !c->hasTile && !c->placing);
    CHECK(!cc_can_next(&why) && why && !strcmp(why, "Give the character a name"));
    cc_next();
    CHECK(c->step == CC_NAME); /* blocked */
    cc_set_name("Mao");
    CHECK(cc_can_next(&why));
    cc_next();
    CHECK(c->step == CC_SIZE);
    cc_set_size(SIZE_SMALL);
    cc_set_size(9); /* not a size: ignored */
    CHECK(c->size == SIZE_SMALL);
    cc_next();
    CHECK(c->step == CC_ATTRS);
    for (int i = 0; i < 6; i++) cc_step_attr(ATTR_STR, 1);
    CHECK(c->attr[ATTR_STR] == ATTR_CREATE_MAX);
    for (int i = 0; i < 6; i++) cc_step_attr(ATTR_AGI, -1);
    CHECK(c->attr[ATTR_AGI] == ATTR_CREATE_MIN);
    cc_step_attr(ATTR_AGI, 3);
    CHECK(c->attr[ATTR_AGI] == 1);
    cc_back();
    CHECK(c->step == CC_SIZE && c->size == SIZE_SMALL && c->attr[ATTR_STR] == 3); /* Back keeps everything */
    cc_next();
    cc_next();
    CHECK(c->step == CC_PICTURE);
    CHECK(!cc_can_next(&why) && !strcmp(why, "Choose a picture first"));
    int files = stub_file_requests;
    cc_choose_picture();
    CHECK(stub_file_requests == files + 1 && stub_file_kind == ACTION_CREATE_PICTURE);
    int refs = stub_refs(TEX_A);
    cc_set_picture(TEX_A, (Image){ 0 });
    CHECK(stub_refs(TEX_A) == refs + 1 && c->picture == TEX_A);
    cc_next();
    CHECK(c->step == CC_SUMMARY);
    cc_next();
    CHECK(c->placing && strstr(stub_last_log, "Click a tile to place Mao (Esc: back)"));

    /* no room: nothing happens but the log line */
    add_wall((Hex){ 3, 3 });
    int before = app.entities.count;
    CHECK(cc_place((Hex){ 3, 3 }) && app.entities.count == before && c->placing);
    CHECK(strstr(stub_last_log, "No room on that tile"));
    cc_back(); /* Esc: back to the summary */
    CHECK(c->active && !c->placing && c->step == CC_SUMMARY);
    cc_next();
    CHECK(cc_place((Hex){ 5, 5 }));
    CHECK(!c->active && app.entities.count == before + 1);
    Entity *e = last_entity();
    CHECK(!strcmp(e->name, "Mao") && e->size == SIZE_SMALL && e->attr[ATTR_STR] == 3 && e->attr[ATTR_AGI] == 1);
    CHECK(e->maxHealth == 21 && e->health == 21 && e->maxStamina == 9 && e->stamina == 9 && e->speed == 4);
    CHECK(e->initiative == 0 && !e->owner[0] && hex_eq(e->tile, (Hex){ 5, 5 }));
    CHECK(stub_refs(TEX_A) == refs + 1); /* the draft let go of its reference, the character holds one */

    /* a tile chosen beforehand (the tile menu): Create on the summary, no placing */
    cc_start(&(Hex){ 7, 7 });
    cc_set_name("Orc");
    cc_next();
    cc_next();
    cc_next();
    cc_set_picture(TEX_B, (Image){ 0 });
    cc_next();
    CHECK(c->step == CC_SUMMARY && c->hasTile);
    cc_next();
    CHECK(!c->active && !c->placing && !strcmp(last_entity()->name, "Orc") && hex_eq(last_entity()->tile, (Hex){ 7, 7 }));

    /* Cancel throws the draft away */
    cc_start(NULL);
    cc_set_name("Gone");
    cc_next();
    int count = app.entities.count;
    cc_cancel();
    CHECK(!c->active && c->name[0] == '\0' && app.entities.count == count);
    CHECK(strstr(stub_last_log, "Character creation cancelled"));

    /* a player asks the GM instead */
    stub_client = true;
    cc_start(NULL);
    cc_set_name("Kiri");
    cc_next();
    cc_next();
    cc_step_attr(ATTR_COG, 1);
    cc_next();
    Image small = { 2, 2, calloc(16, 1) };
    cc_set_picture(TEX_A, small); /* the draft owns small now */
    cc_next();
    cc_next();
    int asks = stub_requests;
    count = app.entities.count;
    CHECK(cc_place((Hex){ 9, 9 }));
    CHECK(stub_requests == asks + 1 && !strcmp(stub_request_name, "Kiri") && stub_request_max_health == 12);
    CHECK(stub_request_attr[ATTR_COG] == 1 && hex_eq(stub_request_tile, (Hex){ 9, 9 }) && app.entities.count == count);
    CHECK(!c->active);
    stub_client = false;
    reset_table();
}
```

- [ ] **Step 2: Run it to see it fail**

Run: `make build/core_test`
Expected: `charcreate.h: No such file or directory`.

- [ ] **Step 3: Implement**

`src/charcreate.h`:

```c
/* Creating a character through the HUD's steps (spec: docs/superpowers/specs/2026-11-27-character-creation-design.md).
 * The draft and its steps; no drawing. hud_create.c shows it in the HUD, input.c / graphics.c place it on the map.
 * The GM's character is spawned (OP_SPAWN, no owner); a player's is asked for (session_request_character). */
#ifndef TOH_CHARCREATE_H
#define TOH_CHARCREATE_H

#include "app.h"
#include "image.h"

typedef enum { CC_NAME, CC_SIZE, CC_ATTRS, CC_PICTURE, CC_SUMMARY, CC_STEPS } CcStep;

typedef struct {
    bool active;  /* creation is running (the steps or placing) */
    bool placing; /* after the summary: the picture follows the mouse, a click places it */
    CcStep step;
    char name[CHAR_NAME_MAX];
    int size;
    int attr[ATTR_COUNT];
    bool hasTile; /* started from the tile menu: created on tile, no placing */
    Hex tile;
    Texture *picture; /* one reference, or NULL */
    Image small;      /* players: the picture for the request (px NULL = none) */
} CharCreate;

const CharCreate *cc_state(void);
void cc_start(const Hex *tile); /* tile: from the tile menu, else NULL. Drops a running draft. */
void cc_cancel(void);           /* logs "Character creation cancelled" */
bool cc_can_next(const char **why);
void cc_next(void);             /* the next step; on the summary: place (or create on the chosen tile) */
void cc_back(void);             /* the step before; while placing: back to the summary */
void cc_set_name(const char *name);
void cc_set_size(int size);     /* SIZE_MIN..SIZE_MAX_CLASS, else ignored */
void cc_step_attr(int attr, int delta); /* within ATTR_CREATE_MIN..ATTR_CREATE_MAX */
void cc_choose_picture(void);   /* opens the PNG dialog (ACTION_CREATE_PICTURE) */
/* The chosen picture: t is retained; small (players' request copy, may be empty) is taken over. */
void cc_set_picture(Texture *t, Image small);
/* While placing, a click on tile h: places the character there if it fits, else logs "No room on that tile".
 * True = the click was used (always, while placing). */
bool cc_place(Hex h);
bool cc_placing(void);

#endif
```

`src/charcreate.c`:

```c
/* The creation draft and its steps (see charcreate.h). */
#include "charcreate.h"
#include "ops.h"
#include "savefmt.h"
#include "session.h"

#include <stdio.h>
#include <string.h>

static CharCreate C;

const CharCreate *cc_state(void) { return &C; }
bool cc_placing(void) { return C.active && C.placing; }

static void reset(void) {
    if (C.picture) vkr_texture_release(C.picture);
    image_free(&C.small);
    memset(&C, 0, sizeof(C));
}

void cc_start(const Hex *tile) {
    reset();
    C.active = true;
    C.step = CC_NAME;
    C.size = SIZE_NORMAL;
    if (tile) {
        C.hasTile = true;
        C.tile = *tile;
    }
}

void cc_cancel(void) {
    if (!C.active) return;
    reset();
    console_log("Character creation cancelled");
}

bool cc_can_next(const char **why) {
    *why = NULL;
    if (!C.active) return false;
    if (C.step == CC_NAME && !savefmt_char_name_valid(C.name)) *why = "Give the character a name";
    else if (C.step == CC_PICTURE && !C.picture) *why = "Choose a picture first";
    return *why == NULL;
}

/* creates the character on h: the GM spawns it, a player asks the GM */
static void create(Hex h) {
    AttrStats d = attr_derive(C.attr);
    if (session_is_client()) {
        session_request_character(&C.small, h, C.size, d.maxHealth, d.speed, d.initiative, d.maxStamina, C.name,
                                  C.attr);
    } else {
        EntitySpec s = { .type = ENTITY_CHARACTER, .image = C.picture, .tile = h, .size = C.size,
                         .maxHealth = d.maxHealth, .health = d.maxHealth, .speed = d.speed,
                         .initiative = d.initiative, .maxStamina = d.maxStamina, .stamina = d.maxStamina };
        snprintf(s.name, sizeof(s.name), "%s", C.name);
        memcpy(s.attr, C.attr, sizeof(s.attr));
        Op op = { .type = OP_SPAWN };
        da_push(&op.specs, s);
        ops_submit(&op);
        op_free(&op);
    }
    reset();
}

void cc_next(void) {
    const char *why;
    if (!cc_can_next(&why) || C.placing) return;
    if (C.step < CC_SUMMARY) {
        C.step++;
    } else if (C.hasTile) {
        create(C.tile);
    } else {
        C.placing = true;
        console_log("Click a tile to place %s (Esc: back)", C.name);
    }
}

void cc_back(void) {
    if (!C.active) return;
    if (C.placing) C.placing = false;
    else if (C.step > CC_NAME) C.step--;
}

void cc_set_name(const char *name) {
    if (C.active) snprintf(C.name, sizeof(C.name), "%s", name ? name : "");
}

void cc_set_size(int size) {
    if (C.active && size >= SIZE_MIN && size <= SIZE_MAX_CLASS) C.size = size;
}

void cc_step_attr(int a, int delta) {
    if (!C.active || a < 0 || a >= ATTR_COUNT) return;
    int v = C.attr[a] + delta;
    C.attr[a] = v < ATTR_CREATE_MIN ? ATTR_CREATE_MIN : v > ATTR_CREATE_MAX ? ATTR_CREATE_MAX : v;
}

void cc_choose_picture(void) {
    if (C.active) gfx_request_file(ACTION_CREATE_PICTURE, NULL, 0, 0, 0, 0, 0, NULL, NULL, NULL);
}

void cc_set_picture(Texture *t, Image small) {
    if (!C.active) {
        image_free(&small);
        return;
    }
    if (t) vkr_texture_retain(t);
    if (C.picture) vkr_texture_release(C.picture);
    C.picture = t;
    image_free(&C.small);
    C.small = small;
}

bool cc_place(Hex h) {
    if (!cc_placing()) return false;
    if (!character_fits(h, C.size, NULL)) {
        console_log("No room on that tile");
        return true;
    }
    create(h);
    return true;
}
```

(`vkr_texture_retain` / `_release` are declared in `vk_renderer.h`, which `app.h` includes; check with
`grep -n vkr_texture_retain src/*.h`. `savefmt_char_name_valid` is in `savefmt.h`.)

`Makefile`: add `src/charcreate.c` to `CORE_SRC` after `src/attrs.c`.

- [ ] **Step 4: Run the tests**

Run: `make test`
Expected: all suites `0 failed` (`stub_bad_releases == 0` at the end of `main` covers the picture references).

- [ ] **Step 5: Commit**

```bash
git add src/charcreate.h src/charcreate.c Makefile tests/core_test.c
git commit -m "Character creation: the draft and its steps (charcreate.c), spawn for the GM, request for players

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 10: The steps in the bar (`hud_create.c`, `hud_game.c`)

**Files:**
- Create: `src/hud_create.h`, `src/hud_create.c`
- Modify: `src/hud_game.c`, `Makefile` (CORE_SRC), `tests/core_test.c`

- [ ] **Step 1: Write the failing test**

Add `#include "hud_create.h"`, the test before `int main`, and call `test_hud_create();` after `test_hud_game();`:

```c
static void test_hud_create(void) {
    reset_table();
    app.winW = 1000;
    app.winH = 700;
    app.console.fontSize = 16;
    app.console.active = false;
    app.toolbox.size = 32;
    stub_client = false;
    stub_pending = 0;
    hud_use_config(NULL, 0);
    hud_set_visible(true);
    hud_game_init("resources");
    HudRect r;

    /* every step's rows and the empty rows pass the config check */
    char err[160];
    const HudRow *rows;
    int n;
    for (int w = 0; w <= 3; w++) {
        hud_create_empty_rows(w, &rows, &n);
        CHECK(n == 1 + w && hud_config_check(rows, n, err, sizeof(err)) == 0);
    }

    /* the GM with no character on the board: the Create button and "+" */
    hud_game_update(0);
    CHECK(hud_part_rect("cc.start", &r) && hud_part_rect("plus", &r));
    CHECK(hud_target()->shown && !strcmp(hud_target()->name, "No character"));
    click_part("cc.start");
    hud_game_update(0);
    const CharCreate *c = cc_state();
    CHECK(c->active && c->step == CC_NAME && !hud_part_rect("plus", &r)); /* "+" hides while creating */
    CHECK(!strcmp(hud_target()->name, "New character") && !strcmp(hud_target()->extra, "· 1/5"));
    CHECK(hud_focused()); /* the name field has the focus */
    hud_text_input("Mao");
    CHECK(!strcmp(c->name, "Mao"));
    hud_game_update(0);
    CHECK(hud_part_rect("cc.next", &r) && !hud_part_rect("cc.back", &r)); /* no Back on the first step */
    hud_text_key(HUD_EDIT_ENTER); /* Enter = Next */
    hud_game_update(0);
    CHECK(c->step == CC_SIZE && !strcmp(hud_value("cc.size")->choice.options[c->size - SIZE_MIN], "Normal"));
    click_part("cc.next");
    hud_game_update(0);
    CHECK(c->step == CC_ATTRS);
    click_part("cc.str.plus");
    hud_game_update(0);
    CHECK(c->attr[ATTR_STR] == 1 && strstr(hud_value("cc.derived")->label.text, "Max HP 15"));
    click_part("cc.next");
    hud_game_update(0);
    CHECK(c->step == CC_PICTURE && !hud_value("cc.next")->button.enabled);
    cc_set_picture(TEX_A, (Image){ 0 });
    hud_game_update(0);
    click_part("cc.next");
    hud_game_update(0);
    CHECK(c->step == CC_SUMMARY && !strcmp(hud_value("cc.next")->button.label, "Place"));
    CHECK(strstr(hud_value("cc.summary")->label.text, "Mao · Normal · STR 1 AGI 0 COG 0 SPI 0 · HP 15 ST 8 SPD 3 INI 0"));
    click_part("cc.cancel");
    hud_game_update(0);
    CHECK(!c->active && hud_part_rect("cc.start", &r));

    /* "+" starts creation while characters exist; the normal bar shows STR / AGI and the tab tooltip */
    Entity *a = add_character((Hex){ 2, 2 }, 3, "");
    a->attr[ATTR_STR] = 2;
    a->attr[ATTR_AGI] = -1;
    a->attr[ATTR_COG] = 1;
    da_push(&app.selectedEntities, a->id);
    hud_game_update(0);
    CHECK(!hud_part_rect("cc.start", &r) && hud_part_rect("plus", &r));
    CHECK(!strcmp(hud_value("pill.left")->pill.label, "STR") && !strcmp(hud_value("pill.left")->pill.value, "2"));
    CHECK(!strcmp(hud_value("pill.right")->pill.label, "AGI") && !strcmp(hud_value("pill.right")->pill.value, "-1"));
    CHECK(!strcmp(hud_target()->tip, "COG 1 · SPI 0"));
    click_part("plus");
    hud_game_update(0);
    CHECK(c->active && c->step == CC_NAME);
    cc_cancel();
    da_clear(&app.selectedEntities);

    /* a player who owns none: Create, with a line per waiting request; full at SESSION_REQUESTS_MAX */
    stub_client = true; /* "alice" owns nothing */
    stub_pending = 2;
    hud_game_update(0);
    CHECK(hud_part_rect("cc.start", &r) && hud_part_rect("cc.wait.2", &r) && !hud_part_rect("cc.wait.3", &r));
    CHECK(!hud_part_rect("plus", &r)); /* players never get "+" */
    CHECK(!strcmp(hud_value("cc.wait.1")->label.text, "Waiting for the GM: Mao"));
    stub_pending = SESSION_REQUESTS_MAX;
    hud_game_update(0);
    CHECK(!hud_value("cc.start")->button.enabled);
    stub_pending = 0;
    stub_client = false;

    hud_game_shutdown();
    hud_use_config(NULL, 0);
    reset_table();
}
```

- [ ] **Step 2: Run it to see it fail**

Run: `make build/core_test`
Expected: `hud_create.h: No such file or directory`.

- [ ] **Step 3: `src/hud_create.h`**

```c
/* The creation steps in the HUD: the rows of each step and of the "no character" bar, the values they show, and
 * their events (charcreate.h does the work). hud_game.c decides when to show them. */
#ifndef TOH_HUD_CREATE_H
#define TOH_HUD_CREATE_H

#include "hud.h"

void hud_create_rows(const HudRow **rows, int *n);                   /* the current step's rows */
void hud_create_empty_rows(int waiting, const HudRow **rows, int *n); /* Create + waiting lines (0..3) */
void hud_create_publish(void);       /* the current step's values and tab */
void hud_create_publish_empty(void); /* the Create button, the waiting lines and the tab */
bool hud_create_event(const HudEvent *ev); /* true = it was one of ours ("cc.*") */

#endif
```

- [ ] **Step 4: `src/hud_create.c`**

```c
/* The creation steps in the HUD (see hud_create.h). Edit the tables to change a step's layout. */
#include "hud_create.h"
#include "charcreate.h"
#include "ops.h"
#include "session.h"

#include <stdio.h>
#include <string.h>

#define BTN 84
#define HINT { .type = HUD_LABEL, .key = "cc.hint", .width = 40 } /* flex: pushes the buttons right */
#define CANCEL { .type = HUD_BUTTON, .key = "cc.cancel", .accent = HUD_ACCENT_GREY, .label = "Cancel", .width = BTN }
#define BACK { .type = HUD_BUTTON, .key = "cc.back", .accent = HUD_ACCENT_GREY, .label = "< Back", .width = BTN }
#define NEXT { .type = HUD_BUTTON, .key = "cc.next", .accent = HUD_ACCENT_GREEN, .label = "Next >", .width = BTN }
#define PREVIEW { .type = HUD_IMAGE, .key = "cc.preview", .width = 44 }

static const HudElement BUTTONS_FIRST[] = { HINT, CANCEL, NEXT };
static const HudElement BUTTONS[] = { HINT, CANCEL, BACK, NEXT };

static const HudElement NAME[] = {
    { .type = HUD_LABEL, .key = "cc.title", .label = "Name", .width = 60 },
    { .type = HUD_TEXTFIELD, .key = "cc.name", .accent = HUD_ACCENT_GOLD, .label = "Name of the character",
      .width = 300, .max_len = CHAR_NAME_MAX - 1 },
};
static const HudElement SIZE[] = { { .type = HUD_CHOICE, .key = "cc.size", .accent = HUD_ACCENT_BLUE, .width = 476 } };
static const char *const STEPPER_KEYS[ATTR_COUNT] = { "cc.str", "cc.agi", "cc.cog", "cc.spi" };
static const HudElement ATTRS[] = {
    { .type = HUD_STEPPER, .key = "cc.str", .accent = HUD_ACCENT_RED, .width = 112 },
    { .type = HUD_STEPPER, .key = "cc.agi", .accent = HUD_ACCENT_GREEN, .width = 112 },
    { .type = HUD_STEPPER, .key = "cc.cog", .accent = HUD_ACCENT_BLUE, .width = 112 },
    { .type = HUD_STEPPER, .key = "cc.spi", .accent = HUD_ACCENT_VIOLET, .width = 112 },
};
static const HudElement DERIVED[] = { { .type = HUD_LABEL, .key = "cc.derived", .width = 360 } };
static const HudElement PICTURE[] = {
    PREVIEW,
    { .type = HUD_BUTTON, .key = "cc.picture", .accent = HUD_ACCENT_BLUE, .label = "Choose picture…", .width = 160 },
    { .type = HUD_LABEL, .key = "cc.picname", .width = 120 },
};
static const HudElement SUMMARY[] = { PREVIEW, { .type = HUD_LABEL, .key = "cc.summary", .width = 440 } };

#define ROW(e) { (e), (int)(sizeof(e) / sizeof((e)[0])) }
static const HudRow ROWS_NAME[] = { ROW(NAME), ROW(BUTTONS_FIRST) };
static const HudRow ROWS_SIZE[] = { ROW(SIZE), ROW(BUTTONS) };
static const HudRow ROWS_ATTRS[] = { ROW(ATTRS), ROW(DERIVED), ROW(BUTTONS) };
static const HudRow ROWS_PICTURE[] = { ROW(PICTURE), ROW(BUTTONS) };
static const HudRow ROWS_SUMMARY[] = { ROW(SUMMARY), ROW(BUTTONS) };
static const struct { const HudRow *rows; int n; } STEPS[CC_STEPS] = {
    [CC_NAME] = { ROWS_NAME, 2 },       [CC_SIZE] = { ROWS_SIZE, 2 },     [CC_ATTRS] = { ROWS_ATTRS, 3 },
    [CC_PICTURE] = { ROWS_PICTURE, 2 }, [CC_SUMMARY] = { ROWS_SUMMARY, 2 },
};

static const HudElement START[] = {
    { .type = HUD_BUTTON, .key = "cc.start", .accent = HUD_ACCENT_GREEN, .label = "+ Create character", .width = 280 },
};
static const HudElement WAIT1[] = { { .type = HUD_LABEL, .key = "cc.wait.1", .width = 280 } };
static const HudElement WAIT2[] = { { .type = HUD_LABEL, .key = "cc.wait.2", .width = 280 } };
static const HudElement WAIT3[] = { { .type = HUD_LABEL, .key = "cc.wait.3", .width = 280 } };
static const HudRow EMPTY[] = { ROW(START), ROW(WAIT1), ROW(WAIT2), ROW(WAIT3) };

/* sizes Wee..Gigantic (SIZE_MIN + index) */
static const char *const SIZE_NAMES[] = { "Wee", "Tiny", "Small", "Normal", "Big", "Huge", "Gigantic" };
static const char *const SIZE_HINTS[] = {
    "Wee: 1 tile, bulk 0",    "Tiny: 1 tile, bulk 1", "Small: 1 tile, bulk 2", "Normal: 1 tile, bulk 3",
    "Big: 1 tile, bulk 4",    "Huge: 3 tiles, a triangle", "Gigantic: 7 tiles, a tile and its neighbours",
};
_Static_assert(sizeof(SIZE_NAMES) / sizeof(SIZE_NAMES[0]) == SIZE_MAX_CLASS - SIZE_MIN + 1, "one name per size");

void hud_create_rows(const HudRow **rows, int *n) {
    CcStep s = cc_state()->step;
    *rows = STEPS[s].rows;
    *n = STEPS[s].n;
}

void hud_create_empty_rows(int waiting, const HudRow **rows, int *n) {
    if (waiting < 0) waiting = 0;
    if (waiting > 3) waiting = 3;
    *rows = EMPTY;
    *n = 1 + waiting;
}

void hud_create_publish(void) {
    const CharCreate *c = cc_state();
    char extra[24], line[HUD_LABEL_MAX];
    snprintf(extra, sizeof(extra), "· %d/%d", (int)c->step + 1, (int)CC_STEPS);
    hud_set_target("New character", extra, false);

    const char *why;
    bool ok = cc_can_next(&why);
    hud_set_button("cc.next", c->step < CC_SUMMARY ? "Next >" : c->hasTile ? "Create" : "Place", ok, why);
    hud_set_button("cc.cancel", "Cancel", true, NULL);
    hud_set_button("cc.back", "< Back", true, NULL);

    AttrStats d = attr_derive(c->attr);
    switch (c->step) {
    case CC_NAME:
        hud_set_label("cc.title", "Name");
        hud_set_textfield("cc.name", c->name);
        break;
    case CC_SIZE:
        hud_set_choice("cc.size", SIZE_NAMES, SIZE_HINTS, SIZE_MAX_CLASS - SIZE_MIN + 1, c->size - SIZE_MIN);
        break;
    case CC_ATTRS:
        for (int a = 0; a < ATTR_COUNT; a++)
            hud_set_stepper(STEPPER_KEYS[a], ATTR_NAMES[a], c->attr[a], ATTR_CREATE_MIN, ATTR_CREATE_MAX);
        snprintf(line, sizeof(line), "Max HP %d · Stamina %d · Speed %d · Initiative %d", d.maxHealth, d.maxStamina,
                 d.speed, d.initiative);
        hud_set_label("cc.derived", line);
        break;
    case CC_PICTURE:
        hud_set_image("cc.preview", c->picture);
        hud_set_label("cc.picname", c->picture ? "Picture chosen" : "No picture yet");
        break;
    case CC_SUMMARY:
        hud_set_image("cc.preview", c->picture);
        snprintf(line, sizeof(line), "%s · %s · STR %d AGI %d COG %d SPI %d · HP %d ST %d SPD %d INI %d", c->name,
                 SIZE_NAMES[c->size - SIZE_MIN], c->attr[ATTR_STR], c->attr[ATTR_AGI], c->attr[ATTR_COG],
                 c->attr[ATTR_SPI], d.maxHealth, d.maxStamina, d.speed, d.initiative);
        hud_set_label("cc.summary", line);
        break;
    case CC_STEPS:
        break;
    }
}

void hud_create_publish_empty(void) {
    hud_set_target("No character", "", false);
    int n = session_is_client() ? session_pending_count() : 0;
    bool full = n >= SESSION_REQUESTS_MAX;
    hud_set_button("cc.start", "+ Create character", !full, full ? ops_result_text(OPR_CLAIM_PENDING) : NULL);
    for (int i = 0; i < n && i < 3; i++) {
        char key[16], line[HUD_LABEL_MAX];
        const char *name = session_pending_name(i);
        snprintf(key, sizeof(key), "cc.wait.%d", i + 1);
        snprintf(line, sizeof(line), "Waiting for the GM: %s", name[0] ? name : "new character");
        hud_set_label(key, line);
    }
}

bool hud_create_event(const HudEvent *ev) {
    if (!ev->key || strncmp(ev->key, "cc.", 3) != 0) return false;
    const char *k = ev->key + 3;
    switch (ev->type) {
    case HUD_EV_BUTTON:
        if (!strcmp(k, "start")) cc_start(NULL);
        else if (!strcmp(k, "cancel")) cc_cancel();
        else if (!strcmp(k, "back")) cc_back();
        else if (!strcmp(k, "next")) cc_next();
        else if (!strcmp(k, "picture")) cc_choose_picture();
        return true;
    case HUD_EV_TEXT:
        if (!strcmp(k, "name")) cc_set_name(ev->text);
        return true;
    case HUD_EV_SUBMIT:
        if (!strcmp(k, "name")) cc_next();
        return true;
    case HUD_EV_CHOICE:
        if (!strcmp(k, "size")) cc_set_size(SIZE_MIN + ev->value);
        return true;
    case HUD_EV_STEP:
        for (int a = 0; a < ATTR_COUNT; a++)
            if (!strcmp(ev->key, STEPPER_KEYS[a])) cc_step_attr(a, ev->value);
        return true;
    default:
        return false;
    }
}
```

- [ ] **Step 5: `src/hud_game.c`**

- Includes: `#include "charcreate.h"` and `#include "hud_create.h"`.
- Above `hud_game_update`:

```c
/* the rows hud_game asked for last (NULL = HUD_ROWS); switched only on a change, so the layout cache holds */
static const HudRow *shown_rows;
static int shown_n;
static int focused_step = -1; /* the step whose name field got the focus */

static void use_rows(const HudRow *r, int n) {
    if (r == shown_rows && n == shown_n) return;
    shown_rows = r;
    shown_n = n;
    hud_use_config(r, n);
}

/* nothing for the bar to show: a player owns no character; the GM (or alone) has none on the board */
static bool viewer_has_none(void) {
    for (int i = 0; i < gh->entities.count; i++) {
        const Entity *e = gh->entities.items[i];
        if (session_is_client() ? mine(e) : e->type == ENTITY_CHARACTER) return false;
    }
    return true;
}
```

- `hud_game_update`: replace the part after `if (!hud_visible()) return false;` with:

```c
    if (gh->console.active) hud_blur(); /* typing goes to the console now */
    const CharCreate *c = cc_state();
    if (c->active && !c->placing) {
        const HudRow *r;
        int n;
        hud_create_rows(&r, &n);
        use_rows(r, n);
        hud_create_publish();
        if ((int)c->step != focused_step) { /* a step opens: the name field gets the focus */
            focused_step = (int)c->step;
            if (c->step == CC_NAME) hud_focus("cc.name");
        }
        return false;
    }
    focused_step = -1;
    if (!c->active) hud_set_plus(!session_is_client());
    if (viewer_has_none()) {
        const HudRow *r;
        int n;
        hud_create_empty_rows(session_is_client() ? session_pending_count() : 0, &r, &n);
        use_rows(r, n);
        hud_create_publish_empty();
        return false;
    }
    use_rows(NULL, 0);
    bool moving = false;
    int others;
    bool from_selection;
    Entity *e = hud_pick_character(&others, &from_selection);
    if (e) publish_character(e, others, from_selection, dt, &moving);
    publish_placeholders();
    return moving;
```

- `publish_character`: after `hud_set_target(name, extra, from_selection);` add:

```c
    char v[16], tip[HUD_TEXT_MAX];
    snprintf(v, sizeof(v), "%d", e->attr[ATTR_STR]);
    hud_set_pill("pill.left", "STR", v);
    snprintf(v, sizeof(v), "%d", e->attr[ATTR_AGI]);
    hud_set_pill("pill.right", "AGI", v);
    snprintf(tip, sizeof(tip), "COG %d · SPI %d", e->attr[ATTR_COG], e->attr[ATTR_SPI]);
    hud_set_target_tip(tip);
```

  and remove the sentence "The stat pills stay unpublished ("—")" from `publish_placeholders`' comment.
- `on_event`: at the top `if (hud_create_event(ev)) return;`, and a new case
  `case HUD_EV_PLUS: cc_start(NULL); break;`. Add empty cases for the other new event types so the switch stays
  complete: `case HUD_EV_BUTTON: case HUD_EV_TEXT: case HUD_EV_SUBMIT: case HUD_EV_STEP: case HUD_EV_CHOICE: break;`.
- `hud.h`'s "How to" comment: "Real content" line — mention that the pills show STR / AGI now.

`Makefile`: add `src/hud_create.c` to `CORE_SRC` after `src/charcreate.c`.

`tests/core_test.c`, `test_hud_game`: the first block ("the GM with nothing selected") still has characters on the
board, so it is unchanged; its check `hud_value("pill.left") == NULL` still holds (nothing selected).

- [ ] **Step 6: Run the tests**

Run: `make test`
Expected: all suites `0 failed`.

- [ ] **Step 7: Commit**

```bash
git add src/hud_create.h src/hud_create.c src/hud_game.c src/hud.h Makefile tests/core_test.c
git commit -m "HUD: the creation steps in the bar, Create character when you have none, the GM's +, STR / AGI pills

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 11: Picture dialog, typing into the bar, placing on the map

**Files:**
- Modify: `src/graphics.c`, `src/input.c`, `src/hud_game.h`, `src/hud_game.c`

Verified by the e2e checks in Task 13.

- [ ] **Step 1: The picture from the dialog**

`src/graphics.c`: `#include "charcreate.h"`; in `gfx_handle_file_result`'s switch add:

```c
    case ACTION_CREATE_PICTURE: {
        Image cut = image_cut_circle(&img);
        image_free(&img);
        Image small = { 0 };
        if (session_is_client()) small = image_scale_to_fit(&cut, NET_CREATE_MAX_DIM); /* for the request */
        Texture *t = texture_from_image(&cut);
        cc_set_picture(t, small); /* retains t, takes small */
        vkr_texture_release(t);
        break;
    }
```

(A cancelled dialog reaches the `if (!path)` branch, which logs nothing for this kind: the step stays as it was.)

- [ ] **Step 2: Drawing the character while placing**

`src/graphics.c`, in the map overlay function right after the move-action block (it ends with
`dr_set_alpha(0.5f);` inside `if (gh->moveMode.active) { ... }`):

```c
    /* placing a new character (charcreate.h): its picture under the mouse, its tiles green if it fits, else red */
    if (cc_placing() && gh->hasTileUnderMouse) {
        const CharCreate *cc = cc_state();
        Hex tiles[MAX_OCCUPIED];
        int n = character_occupied_tiles(gh->tileUnderMouse, cc->size, tiles);
        bool fits = character_fits(gh->tileUnderMouse, cc->size, NULL);
        dr_set_alpha(0.9f);
        for (int i = 0; i < n; i++)
            draw_hex(tiles[i], th + 3, HUD_THEME.accent[fits ? HUD_ACCENT_GREEN : HUD_ACCENT_RED]);
        float x, y, d;
        gfx_character_rect(gh->tileUnderMouse, cc->size, &x, &y, &d);
        dr_set_alpha(0.75f);
        if (cc->picture) dr_image(cc->picture, x, y, d, d);
        dr_set_alpha(0.5f);
    }
```

(Check the alpha the function uses after the move block; restore that value.)

- [ ] **Step 3: Text and keys for the name field**

`src/hud_game.h`:

```c
/* While a HUD text field has the focus: text and editing keys go to it (true = used). Ctrl / Alt + Enter and the
 * modifier keys are left to the game; Ctrl + V pastes into the field. */
bool hud_game_text(const char *utf8);
bool hud_game_text_key(SDL_Keycode key);
```

`src/hud_game.c`:

```c
bool hud_game_text(const char *utf8) { return hud_text_input(utf8); }

bool hud_game_text_key(SDL_Keycode key) {
    if (!hud_focused()) return false;
    switch (key) {
    case SDLK_LSHIFT: case SDLK_RSHIFT: case SDLK_LCTRL: case SDLK_RCTRL: case SDLK_LALT: case SDLK_RALT:
        return false; /* the game tracks them */
    case SDLK_RETURN: case SDLK_KP_ENTER:
        if (gh->isCtrlDown || gh->isAltDown) return false; /* console / log toggles */
        return hud_text_key(HUD_EDIT_ENTER);
    case SDLK_V:
        if (gh->isCtrlDown) {
            char *clip = SDL_GetClipboardText();
            if (clip) {
                for (char *p = clip; *p; p++)
                    if (*p == '\n' || *p == '\r') *p = ' ';
                hud_text_input(clip);
                SDL_free(clip);
            }
        }
        return true;
    case SDLK_LEFT: return hud_text_key(HUD_EDIT_LEFT);
    case SDLK_RIGHT: return hud_text_key(HUD_EDIT_RIGHT);
    case SDLK_HOME: return hud_text_key(HUD_EDIT_HOME);
    case SDLK_END: return hud_text_key(HUD_EDIT_END);
    case SDLK_BACKSPACE: return hud_text_key(HUD_EDIT_BACKSPACE);
    case SDLK_DELETE: return hud_text_key(HUD_EDIT_DELETE);
    case SDLK_ESCAPE: return hud_text_key(HUD_EDIT_ESCAPE);
    default:
        return true; /* every other key belongs to the field while it has the focus */
    }
}
```

(Pasted text longer than the field's limit is ignored as a whole, like typing past it.)

`src/input.c`: `#include "charcreate.h"`.
- `key_pressed`, at the very top after `CON->swallowText = false;`:

```c
    if (!CON->active && hud_game_text_key(key)) return; /* the HUD's name field has the focus */
```

  Because modifiers return false, `isShiftDown` / `isCtrlDown` tracking still runs.
- `text_input`: after the `swallowText` block add `if (!CON->active && hud_game_text(text)) return;`.

- [ ] **Step 4: Placing clicks, Esc and the keyboard**

`src/input.c`, `io_handle_event`:
- Left button, after the `hud_game_mouse_down` line:

```c
            if (cc_placing()) { /* placing a new character: the click is for it, not the map */
                Hex h;
                if (hex_find_closest(gh->mousePos, &h)) cc_place(h);
                break;
            }
```

- Right button, after the `move_ui_active()` block:

```c
            if (cc_placing()) { /* back to the summary */
                cc_back();
                break;
            }
```

`key_pressed`, the Escape case:

```c
    case SDLK_ESCAPE:
        if (radial_is_open()) radial_game_close();
        else if (move_ui_active()) {
            move_ui_cancel();
            console_log("Move cancelled");
        } else if (cc_placing()) {
            cc_back();
        }
        break;
```

The Enter case, in the `if (!gh->mouseActive)` branch, before the simulated click:

```c
            if (cc_placing()) { /* keyboard-only placing */
                if (gh->isRShiftDown) cc_back();
                else if (gh->hasTileUnderMouse) cc_place(gh->tileUnderMouse);
                break;
            }
```

- [ ] **Step 5: Build and try it**

Run: `make -j && make test && ./toh --size 900x650`
Manual check: the bar says "+ Create character"; click it, type a name, Enter, Next, use the + buttons, Next,
Choose picture (a PNG), Next, Place; the picture follows the mouse with green tiles; click places it. Then the
"+" box at the bottom left of the bar starts it again; Cancel ends it.

- [ ] **Step 6: Commit**

```bash
git add src/graphics.c src/input.c src/hud_game.h src/hud_game.c
git commit -m "Character creation: picture dialog, typing into the name field, placing the character on the map

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 12: GM-only "Create character" in the tile menu

**Files:**
- Modify: `src/radial_config.c`, `src/radial_game.c`, `tests/core_test.c`

- [ ] **Step 1: Write the failing test**

In `test_radial_menu` (or a new small test called from `main` after it), add:

```c
    /* the GM's "Create character" on an empty tile starts creation with the tile chosen */
    reset_table();
    stub_client = false;
    const RadialMenu *tile = NULL;
    for (int i = 0; i < RADIAL_NMENUS; i++)
        if (!strcmp(RADIAL_MENUS[i].id, "tile")) tile = &RADIAL_MENUS[i];
    bool found = false;
    for (int i = 0; tile && i < tile->count; i++)
        if (!strcmp(tile->entries[i].id, "char.create")) {
            found = true;
            CHECK(!strcmp(tile->entries[i].visible_if, "gm") && !strcmp(tile->entries[i].enabled_if, "!occupied"));
        }
    CHECK(found);
```


Then the event, through the real adapter (as `test_action_menu` does), on the empty hex (6, 6):

```c
    gh->winW = 1000;
    gh->winH = 800;
    radial_game_init();
    radial_use_config(NULL, 0);
    Hex empty = { 6, 6 };
    CHECK(radial_game_open(hex_center(empty)) && !strcmp(radial_menu_id(), "tile"));
    CHECK(rad_click("ring.7")); /* the GM's 7th entry */
    const CharCreate *cc = cc_state();
    CHECK(cc->active && cc->hasTile && hex_eq(cc->tile, empty) && cc->step == CC_NAME);
    cc_cancel();
    rad_settle();
    /* a player doesn't see it */
    stub_client = true;
    CHECK(radial_game_open(hex_center(empty)));
    float px, py;
    CHECK(!radial_part_point("ring.7", &px, &py) && !radial_part_point("ring.6", &px, &py));
    radial_game_close();
    rad_settle();
    stub_client = false;
    reset_table();
```

In `test_radial_menu` the GM now sees 7 entries in the tile menu: change
`CHECK(!radial_part_point("ring.7", &x, &y) && !radial_part_point("parent.1", &x, &y));` to
`CHECK(radial_part_point("ring.7", &x, &y) && !radial_part_point("ring.8", &x, &y) && !radial_part_point("parent.1", &x, &y));`.

- [ ] **Step 2: Run it to see it fail**

Run: `make test`
Expected: `CHECK failed: found`.

- [ ] **Step 3: Implement**

`src/radial_config.c`, at the end of `TILE[]`:

```c
    ACTION("char.create", "Create character", HUD_ACCENT_GREEN, .visible_if = "gm", .enabled_if = "!occupied"),
```

and update the file's top comment: the tile menu also has the GM's "Create character" (a real entry).

`src/radial_game.c`: `#include "charcreate.h"`; in `on_event`'s `RADIAL_EV_ACTION` branch, before the `info`
check:

```c
        } else if (strcmp(ev->id, "char.create") == 0) { /* the creation steps, with this tile */
            Hex h = { ev->target.row, ev->target.col };
            cc_start(&h);
```

- [ ] **Step 4: Run the tests**

Run: `make test && make -j`
Expected: all suites `0 failed`.

- [ ] **Step 5: Commit**

```bash
git add src/radial_config.c src/radial_game.c tests/core_test.c
git commit -m "Tile menu: the GM's Create character on empty tiles starts the steps with that tile

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 13: End-to-end checks

**Files:**
- Modify: `tests/e2e_hud.sh`, `Makefile` (e2e target)
- Create: `tests/e2e_create.sh`

- [ ] **Step 1: GM flows in `tests/e2e_hud.sh`**

Add an `occupied` helper next to `red` / `dark`:

```bash
# is the tile at pixel X Y occupied? empty tiles are light grey (about 0.9), the test icon mostly black
occupied() {
    local v
    v=$(magick "$1" -crop 16x16+$(($2 - 8))+$(($3 - 8)) +repage -colorspace gray -format '%[fx:mean]' info:)
    awk -v v="$v" 'BEGIN { exit !(v < 0.8) }'
}
```

Before the final `echo "e2e HUD: OK"` add a second run:

```bash
# Creating characters through the bar (GM, empty board): the steps, placing, "+", Cancel and the tile menu
P="400 200"; R="520 300"   # where Mao is placed; the tile menu's tile for Orc
timeout 60 ./toh --size 900x650 --auto-file "$icon" \
    --exec "@wait 5" --exec "@hudrect cc.start $out/start" --exec "@clickhud cc.start" --exec "@wait 3" \
    --exec "@type Mao" --exec "@wait 2" --exec "@clickhud cc.next" --exec "@wait 3" \
    --exec "@clickhud cc.next" --exec "@wait 3" \
    --exec "@clickhud cc.str.plus" --exec "@wait 2" --exec "@clickhud cc.str.plus" --exec "@wait 2" \
    --exec "@clickhud cc.str.plus" --exec "@wait 2" --exec "@clickhud cc.next" --exec "@wait 3" \
    --exec "@clickhud cc.picture" --exec "@wait 3" --exec "@clickhud cc.next" --exec "@wait 3" \
    --exec "@clickhud cc.next" --exec "@wait 3" --exec "@move $P" --exec "@wait 3" --exec "@shot $out/placing.png" \
    --exec "@click $P" --exec "@wait 5" --exec "@shot $out/placed.png" \
    --exec "@hudrect plus $out/plus" --exec "@click $P" --exec ":c hp 0" \
    --exec "@clickhud plus" --exec "@wait 3" --exec "@type Gone" --exec "@clickhud cc.cancel" --exec "@wait 3" \
    --exec "@rclick $R" --exec "@wait 20" --exec "@clickradial ring.7" --exec "@wait 5" \
    --exec "@type Orc" --exec "@wait 2" --exec "@clickhud cc.next" --exec "@wait 3" --exec "@clickhud cc.next" \
    --exec "@wait 3" --exec "@clickhud cc.next" --exec "@wait 3" --exec "@clickhud cc.picture" --exec "@wait 3" \
    --exec "@clickhud cc.next" --exec "@wait 3" --exec "@clickhud cc.next" --exec "@wait 5" \
    --exec "@shot $out/preset.png" --exec "@logfile $out/create.log" \
    --exec "@quit" > "$out/toh-create.log" 2>&1 || fail "toh exited with status $? (see $out/toh-create.log)"

[ -s "$out/start" ] || fail "no Create character button on an empty board (see $out/toh-create.log)"
occupied "$out/placing.png" $P || fail "the picture does not follow the mouse while placing"
occupied "$out/placed.png" $P || fail "the character was not placed"
grep -q "Mao: 21/21 -> 0/21" "$out/create.log" || fail "Mao does not have STR 3's max HP 21 (see $out/create.log)"
[ -s "$out/plus" ] || fail "the GM has no + box with a character on the board"
grep -q "Character creation cancelled" "$out/create.log" || fail "Cancel did not end the creation"
! grep -q "Gone" "$out/create.log" || fail "the cancelled character exists"
occupied "$out/preset.png" $R || fail "the tile menu's Create character did not create on its tile"
```

(`@type Gone` goes into the name field only; it shows up in the log only if a character named Gone was made.)

- [ ] **Step 2: A player in `tests/e2e_create.sh`**

```bash
#!/usr/bin/env bash
# End-to-end check of a player creating a character through the bar: the GM declines the first request (the
# waiting line goes, Create character stays), accepts the second (the character is the player's).
# Opens two windows (needs Wayland or X11) and uses ImageMagick. Usage: tests/e2e_create.sh (or: make e2e)
set -euo pipefail
cd "$(dirname "$0")/.."
export TOH_SETTINGS= TOH_NO_PORTMAP=1 TOH_RELAY=

out=${KEEP:-$(mktemp -d)}
[ -n "${KEEP:-}" ] || trap 'rm -rf "$out"' EXIT
mkdir -p "$out"
icon=resources/images/toolIcons/area.png
port=${TOH_TEST_PORT:-41297}
fail() { echo "FAIL: $*" >&2; exit 1; }
occupied() {
    local v
    v=$(magick "$1" -crop 16x16+$(($2 - 8))+$(($3 - 8)) +repage -colorspace gray -format '%[fx:mean]' info:)
    awk -v v="$v" 'BEGIN { exit !(v < 0.8) }'
}
T="400 200"

# the steps for a character named $1, then Place on T, as --exec arguments in the array "steps"
make_steps() {
    steps=(--exec "@clickhud cc.start" --exec "@wait 3" --exec "@type $1" --exec "@wait 2"
        --exec "@clickhud cc.next" --exec "@wait 3" --exec "@clickhud cc.next" --exec "@wait 3"
        --exec "@clickhud cc.agi.plus" --exec "@wait 2" --exec "@clickhud cc.next" --exec "@wait 3"
        --exec "@clickhud cc.picture" --exec "@wait 3" --exec "@clickhud cc.next" --exec "@wait 3"
        --exec "@clickhud cc.next" --exec "@wait 3" --exec "@click $T" --exec "@wait 10")
}
make_steps Mao; mao=("${steps[@]}")
make_steps Kiri; kiri=("${steps[@]}")

TOH_JOINCODE_FILE="$out/code" timeout 90 ./toh --size 900x650 --port "$port" --auto-file "$icon" \
    --exec ":host --direct" --exec "@waitnet 1" \
    --exec "@waitfile $out/asked1" --exec "@wait 30" --exec "n" --exec "@touch $out/declined" \
    --exec "@waitfile $out/asked2" --exec "@wait 30" --exec "y" --exec "@touch $out/accepted" \
    --exec "@waitfile $out/client-done" --exec ":unhost" --exec "@wait 10" --exec ":q!" \
    > "$out/host.log" 2>&1 &
host=$!
for _ in $(seq 100); do [ -s "$out/code" ] && break; sleep 0.1; done
[ -s "$out/code" ] || fail "the host wrote no join code"
code=$(cat "$out/code")

timeout 90 ./toh --size 900x650 --auto-file "$icon" \
    --exec ":join $code Bob" --exec "@waitnet 1" --exec "@wait 10" --exec "@hudrect cc.start $out/start" \
    "${mao[@]}" --exec "@hudrect cc.wait.1 $out/waiting1" --exec "@touch $out/asked1" \
    --exec "@waitfile $out/declined" --exec "@wait 30" \
    --exec "@hudrect cc.wait.1 $out/after-decline" --exec "@hudrect cc.start $out/start2" \
    "${kiri[@]}" --exec "@touch $out/asked2" --exec "@waitfile $out/accepted" --exec "@wait 30" \
    --exec "@shot $out/created.png" --exec "@hudrect cc.start $out/start3" \
    --exec "@click $T" --exec "@wait 5" --exec "@hudrect tab $out/tab" --exec "@logfile $out/client-log.txt" \
    --exec "@touch $out/client-done" --exec "@wait 60" --exec ":q!" \
    > "$out/client.log" 2>&1 || fail "the client exited with status $? (see $out/client.log)"
wait "$host" || fail "the host exited with status $? (see $out/host.log)"

[ -s "$out/start" ] || fail "a player without characters has no Create character button"
[ -s "$out/waiting1" ] || fail "no waiting line after the request"
[ ! -s "$out/after-decline" ] || fail "the waiting line stayed after the GM declined"
[ -s "$out/start2" ] || fail "Create character is gone after the GM declined"
grep -q "The GM declined your character." "$out/client-log.txt" || fail "no decline message"
occupied "$out/created.png" $T || fail "the accepted character is not on the tile"
[ ! -s "$out/start3" ] || fail "Create character still shows although Bob owns a character"
[ -s "$out/tab" ] || fail "Bob does not see his new character in the HUD (not his?)"
echo "e2e create: OK"
```

Make it executable: `chmod +x tests/e2e_create.sh`.

`Makefile`, e2e target: add `./tests/e2e_create.sh` after `./tests/e2e_moves.sh`, and add "character creation"
to the comment above the target.

- [ ] **Step 3: Run them**

Run: `make -j && ./tests/e2e_hud.sh && ./tests/e2e_create.sh && make e2e`
Expected: `e2e HUD: OK`, `e2e create: OK`, and every other e2e script still OK. If a check fails, run with
`KEEP=/tmp/claude-1000/e2e ./tests/e2e_....sh` and look at the screenshots and logs there.

- [ ] **Step 4: Commit**

```bash
git add tests/e2e_hud.sh tests/e2e_create.sh Makefile
git commit -m "e2e: creating characters through the bar (GM steps, +, Cancel, tile menu; a player declined then accepted)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 14: Guides and README

**Files:**
- Modify: `playercontrols.md`, `gmcontrols.md`, `README.md`

- [ ] **Step 1: `playercontrols.md`**

Section 3 ("Your HP and stamina"), after the first paragraph add:

```markdown
Beside the HP bar are your character's **STR** (left) and **AGI** (right). Hover the tab to see its COG and SPI.
```

Replace section 7 ("Get a character") with:

```markdown
## 7. Get a character

**Create one in the bar:** while you have no character, the bar at the bottom says **+ Create character**. Click
it and go through the steps; **Cancel** stops at any step, **< Back** goes back without losing anything.

1. **Name**: type it (Enter = Next).
2. **Size**: Normal is chosen; hover a size to see how many tiles it takes.
3. **Attributes**: STR, AGI, COG and SPI with − and +, from −2 to 3. The line below shows what they give:
   Max HP 12 + 3 × STR, Stamina 8 + AGI, Speed AGI + 3 (at least 2), Initiative COG.
4. **Picture**: **Choose picture…** and pick a PNG (large pictures are shrunk to 512×512).
5. **Summary**: check it and click **Place**. The picture follows the mouse: green tiles = it fits, red = no room.
   Left click a green tile. Esc or a right click goes back to the summary.

The GM then accepts or declines; until then the bar shows "Waiting for the GM: <name>". The rest of the table
only sees your character once the GM says yes. It then belongs to you. Your attributes can't be changed
afterwards (only the GM can).

**Claim one that exists:** select a character that has no owner yet, then type `:claim`. The GM answers yes or
no.

**Or type it:** click an empty tile, then `:c` with the stats, e.g. `:c name Mao str 1 agi 2` (attributes
−2 to 3; they fill in Max HP, stamina, speed and initiative), then pick a picture.

Claims and new characters count together: at most 3 can wait for the GM at once.
```

(Keep the sentence about "No room on that tile" from the old section, attached to the typed way.)

- [ ] **Step 2: `gmcontrols.md`**

Section 4, replace "### Create" with:

```markdown
### Create

**In the bar.** With no character on the board, the bar at the bottom shows **+ Create character**; with
characters on it, the small **+** box at the bottom left of the bar starts the same steps (players never see
it, and it hides while you create). In the tile menu of an empty tile (right click, Drag tool) **Create
character** starts them with that tile already chosen. The steps, inside the bar (the tab shows `New character · 2/5`):

| Step | What you do |
|---|---|
| 1 Name | Type it (1 to 32 bytes; Enter = Next, Ctrl + V pastes). While the field has the focus, keys go to it, not to the map; Esc or a click elsewhere takes the focus away. |
| 2 Size | Wee to Gigantic; Normal is chosen. Hover for tiles and bulk. |
| 3 Attributes | STR, AGI, COG, SPI with − / +, −2 to 3; the line below shows Max HP, Stamina, Speed and Initiative (see **Attributes**) |
| 4 Picture | **Choose picture…** opens the PNG dialog; the preview shows the round cut |
| 5 Summary | **Place**: the picture follows the mouse, its tiles green where it fits and red where not; left click a green tile, Esc / right click goes back to the summary. From the tile menu the button is **Create** and puts it on that tile at once (no room check). |

**Cancel** is on every step ("Character creation cancelled"); **< Back** keeps what was entered. **Next** is
greyed out until there is a name ("Give the character a name") and a picture ("Choose a picture first"). Your
new character has full HP and stamina and no owner; give it away with `:c owner`, put it into a party with
`:c party`. A player's character made this way comes to you as a yes/no question (see section 14).

**Typed.** `:c [stats]` with no character selected creates one, then opens a file dialog for its image (PNG). It
is placed on:
- the selected tile, if exactly one is selected;
- every selected tile, if several are selected (the console asks first);
- otherwise the tile under the mouse.

Stats can be given right away, e.g. `:c name Mao size 3 str 1 agi 2` or `:c name Mao maxhealth 30 speed 6`.
Attributes fill in the stats they give unless you give those too. Without `size` a character is Normal (1).
**Set a speed:** a new character without AGI or `speed` has speed 0 and cannot move.
```

In "### Edit", add rows to the table:

```markdown
| `:c str <n>` | | STR; sets max HP to 12 + 3 × STR |
| `:c agi <n>` | | AGI; sets max stamina to 8 + AGI and speed to AGI + 3 (at least 2) |
| `:c cog <n>` | | COG; sets initiative to COG |
| `:c spi <n>` | | SPI (sets nothing yet) |
```

and add after the table:

```markdown
**Attributes.** Characters have the four attributes of Tales of Honor, −10 to 10 with `:c` (−2 to 3 in the
creation steps and in players' requests). Changing one recalculates the stats it gives; a stat given by hand in
the same command wins (`:c str 1 maxhealth 40` = STR 1, max HP 40) and holds until that attribute changes again.
Full HP and stamina stay full. The log shows e.g. `Mao: STR 1 -> 2, max HP 15 -> 18`. Older saves load with all
attributes at 0. Only you change attributes.
```

In the HUD bullet list of section 4 add:

```markdown
  - Beside the HP bar the character's STR (left) and AGI (right); hovering the tab shows its COG and SPI.
  - With no character to show (you: none on the board; a player: none of their own) the panel shows
    **+ Create character** instead (see **Create**); a player's waiting requests show under it as
    `Waiting for the GM: Mao`.
```

Section 10, "### The tile menu": after "and "GM action" is only shown to you." add
` **Create character** (only you, only on an empty tile) starts the creation steps for that tile (section 4).`

Section 14 (managing players / requests): where the yes/no question for a new character is described, say that
it now ends with the attributes (`... STR 1 AGI 2 COG 0 SPI 0`), and that the player's bar shows the request
until you answer. (Find it with `grep -n "new character" gmcontrols.md`.)

Section 17 (keyboard-only): add `While placing a new character, Enter places it on the cursor's tile and Right
Shift + Enter goes back to the summary.`

Section 19: change the bullet "ask for existing characters with `:claim`, or for a new one of their own with
`:c` (you answer y/n);" to "ask for existing characters with `:claim`, or for a new one of their own through the
bar's **Create character** or `:c` (attributes −2 to 3; you answer y/n);" and add to "Everything else is yours":
"changing attributes (`:c str` etc.) after a character exists, and the bar's + box and the tile menu's Create
character;".

- [ ] **Step 3: `README.md` Controls**

Add table rows:

```markdown
| + Create character (bar, when you have no character) / + box left of the bar (GM) | create a character in steps: name, size, attributes, picture, then place it |
```

and in the console commands paragraph after `party <name>` (GM), add
`str / agi / cog / spi n` (attributes; they set max HP, max stamina, speed, initiative),
and in the tile-menu row add "; the GM's Create character on empty tiles".

- [ ] **Step 4: Check the guides against the code**

Run: `grep -n "str\|agi\|Create character\|+ box\|plus" playercontrols.md gmcontrols.md README.md | head -40`
and read `src/console.c` (`ARGS_CHARACTER`, `cmd_help`), `src/input.c`, `src/session.c`, `src/ops.c`,
`src/tools.c` once more (CLAUDE.md's list): every new command, key and message is in `gmcontrols.md`, the
player-usable ones in `playercontrols.md`, nothing promised that isn't built.

Test mode: nothing to change (creation has no turn, cost or cooldown; the spec says so). Confirm the Test mode
section of `gmcontrols.md` needs no edit.

- [ ] **Step 5: Commit**

```bash
git add playercontrols.md gmcontrols.md README.md
git commit -m "Guides: creating characters in the bar, the GM's + and tile menu entry, attributes and :c str/agi/cog/spi

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 15: Final check

- [ ] **Step 1: Everything builds and passes**

Run: `make clean && make -j 2>&1 | grep -i warning; make test && make e2e`
Expected: no warnings from the changed files, every unit suite `0 failed`, every e2e script `OK`.

- [ ] **Step 2: Fuzz seeds**

Run: `make fuzz FUZZ_TIME=20` (needs clang with libFuzzer; skip with a note if it is not installed)
Expected: no crashes in `fuzz_savefmt` and `fuzz_netmsg` with the v9 / protocol 11 samples.

- [ ] **Step 3: Update the project memory**

Add the feature to the "Multiplayer progress" memory line once it is pushed (the user pushes; do not push
without being asked).
