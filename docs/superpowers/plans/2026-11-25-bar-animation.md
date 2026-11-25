# Animated HP and Stamina Bars Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The HP and stamina fills on the HP panel ease smoothly to new values, and a loss leaves a lighter trail
that holds for 0.3 s and then shrinks.

**Architecture:**
- **Animation module:** a small SDL-free module, `src/baranim.c`, does the maths in fractions of max and is
  unit tested.
- **Drawing:** `draw_hp_panels` keeps one `BarAnim` per bar on each `Entity` (memory only), steps them with the
  real frame time, and draws a solid and a light interval.
- **Redrawing:** `gfx_draw_frame` reports whether a bar is still moving, so the event-driven main loop keeps
  drawing until it stops.
- **Testing hook:** a script action `@waitbars` lets the e2e test wait until the bars are still.

**Tech Stack:** C11, SDL3, Vulkan 2D API (`src/draw.h`).

Spec: `docs/superpowers/specs/2026-11-25-bar-animation-design.md`.

**Conventions:**
- `make test` runs five test programs, each printing `N checks, 0 failed`.
- `make 2>&1 | grep -i warn` prints nothing.
- e2e (`./tests/e2e_net.sh`, `TOH_E2E_RELAY=1 ./tests/e2e_net.sh`, `./tests/e2e_save.sh`) needs a display and
  `make build/toh-relay`. The tiling window manager may resize windows; rerun once on a strange failure.
- Commit messages are plain sentences ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Guides describe only what is in the game (CLAUDE.md).

---

### Task 1: The animation module

**Files:** create `src/baranim.h` and `src/baranim.c`; modify `Makefile` (`CORE_SRC`) and `tests/core_test.c`.

- [ ] **Step 1: Failing test.** In `tests/core_test.c`, add `#include "baranim.h"` and this function, and call it
  from `main`:

```c
static bool near(float a, float b, float eps) { return fabsf(a - b) <= eps; }

static void test_bar_anim(void) {
    BarAnim a;
    bar_anim_reset(&a, 0.8f);
    CHECK(!bar_anim_step(&a, 0.8f, 0.1f) && a.shown == 0.8f && a.trail == 0.8f); /* at rest */

    /* a loss: fast ease-out, the trail holds 0.3 s, then everything stops */
    float t = 0;
    while (t < 0.25f - 1e-4f) { bar_anim_step(&a, 0.4f, 1.0f / 60); t += 1.0f / 60; }
    float done = (0.8f - a.shown) / 0.4f;
    CHECK(done > 0.9f && done < 0.99f);
    CHECK(a.trail == 0.8f);
    while (t < 0.29f) { bar_anim_step(&a, 0.4f, 1.0f / 60); t += 1.0f / 60; }
    CHECK(a.trail == 0.8f);
    bar_anim_step(&a, 0.4f, 0.1f);
    CHECK(a.trail < 0.8f && a.trail > a.shown);
    bool moving = true;
    for (int i = 0; i < 60 && moving; i++) moving = bar_anim_step(&a, 0.4f, 1.0f / 60); /* 1 s more */
    CHECK(!moving && a.shown == 0.4f && a.trail == 0.4f);

    /* a gain: no trail */
    bool trail_ok = true;
    for (int i = 0; i < 40; i++) {
        bar_anim_step(&a, 0.9f, 1.0f / 60);
        trail_ok = trail_ok && a.trail == a.shown;
    }
    CHECK(trail_ok && a.shown == 0.9f);

    /* a new target mid-way continues from where the fill is */
    bar_anim_reset(&a, 1.0f);
    for (int i = 0; i < 6; i++) bar_anim_step(&a, 0.0f, 1.0f / 60);
    float mid = a.shown;
    bar_anim_step(&a, 1.0f, 1.0f / 60);
    CHECK(mid > 0.05f && mid < 0.95f && a.shown > mid && a.shown - mid < 0.2f);

    /* independent of the frame rate */
    BarAnim fast, slow;
    bar_anim_reset(&fast, 1.0f);
    bar_anim_reset(&slow, 1.0f);
    for (int i = 0; i < 60; i++) bar_anim_step(&fast, 0.0f, 1.0f / 60);
    for (int i = 0; i < 6; i++) bar_anim_step(&slow, 0.0f, 1.0f / 6);
    CHECK(near(fast.shown, slow.shown, 0.01f));

    /* spans */
    float sl, sh, ll, lh;
    a = (BarAnim){ .shown = 0.4f, .trail = 0.7f };
    bar_anim_spans(&a, &sl, &sh, &ll, &lh);
    CHECK(sl == 0 && sh == 0.4f && ll == 0.4f && lh == 0.7f);
    a = (BarAnim){ .shown = -0.75f, .trail = -0.25f };
    bar_anim_spans(&a, &sl, &sh, &ll, &lh);
    CHECK(sl == -0.25f && sh == 0 && ll == -0.75f && lh == -0.25f);
    a = (BarAnim){ .shown = -0.2f, .trail = 0.3f };
    bar_anim_spans(&a, &sl, &sh, &ll, &lh);
    CHECK(sl == sh && ll == -0.2f && lh == 0.3f);

    /* the fraction a bar shows */
    CHECK(bar_fraction(10, 20) == 0.5f && bar_fraction(-30, 20) == -1.0f && bar_fraction(5, 0) == 0.0f);
}
```

  Check that `core_test.c` includes `<math.h>` (for `fabsf`); add it if not.

- [ ] **Step 2:** `make build/core_test` fails (no `baranim.h`).

- [ ] **Step 3: Implement.**

`src/baranim.h`:

```c
/* Smooth HP / stamina bar animation (no SDL). Values are fractions of the frame width, -1..1. */
#pragma once
#include <stdbool.h>

typedef struct {
    float shown;  /* where the solid fill ends */
    float trail;  /* where the light "just lost" part ends; never below shown */
    float target; /* the value the fill is moving to */
    float hold;   /* seconds the trail still waits before it shrinks */
} BarAnim;

#define BAR_ANIM_TAU 0.083f /* ease-out time constant: about 95 % after 0.25 s */
#define BAR_ANIM_HOLD 0.3f  /* the trail waits this long after a loss */
#define BAR_ANIM_SNAP 0.004f

float bar_fraction(int value, int max); /* value / max clamped to -1..1; 0 when max <= 0 */
void bar_anim_reset(BarAnim *a, float target);
bool bar_anim_step(BarAnim *a, float target, float dt); /* true while still moving */
void bar_anim_spans(const BarAnim *a, float *solid_lo, float *solid_hi, float *light_lo, float *light_hi);
```

`src/baranim.c`:

```c
#include "baranim.h"

#include <math.h>

float bar_fraction(int value, int max) {
    if (max <= 0) return 0;
    float f = (float)value / (float)max;
    return f < -1 ? -1 : f > 1 ? 1 : f;
}

void bar_anim_reset(BarAnim *a, float target) {
    *a = (BarAnim){ .shown = target, .trail = target, .target = target };
}

/* moves v towards to by the frame-rate independent ease-out; snaps when close */
static float ease(float v, float to, float dt) {
    v += (to - v) * (1 - expf(-dt / BAR_ANIM_TAU));
    return fabsf(to - v) < BAR_ANIM_SNAP ? to : v;
}

bool bar_anim_step(BarAnim *a, float target, float dt) {
    if (target < a->target) a->hold = BAR_ANIM_HOLD; /* a loss (re)starts the trail's wait */
    a->target = target;
    a->shown = ease(a->shown, target, dt);
    if (a->trail < a->shown) a->trail = a->shown; /* gains have no trail */
    if (a->hold > 0) {
        a->hold -= dt;
        if (a->hold < 0) a->hold = 0;
    } else {
        a->trail = ease(a->trail, a->shown, dt);
    }
    return a->shown != a->target || a->trail != a->shown;
}

void bar_anim_spans(const BarAnim *a, float *solid_lo, float *solid_hi, float *light_lo, float *light_hi) {
    *light_lo = a->shown;
    *light_hi = a->trail;
    if (a->shown >= 0) *solid_lo = 0, *solid_hi = a->shown;
    else if (a->trail <= 0) *solid_lo = a->trail, *solid_hi = 0;
    else *solid_lo = *solid_hi = 0;
}
```

  Notes:
  - The hold uses the frame's whole `dt` for waiting; leftover time is not carried over. With 16 ms frames this
    is at most one frame late, which nobody sees.
  - If the "0.29 s still holds" test is flaky because of float sums, loosen the loop bound in the test, never
    the hold logic.

`Makefile`: append `src/baranim.c` to `CORE_SRC`. The game build already uses `$(wildcard src/*.c)`.

- [ ] **Step 4:** `make && make test`: no warnings, all `0 failed`.
- [ ] **Step 5:** Commit: "Bar animation: ease-out slide and a trail for losses".

---

### Task 2: Animate the panel bars

**Files:** `src/app.h`, `src/graphics.c`, `src/main.c`

- [ ] **Step 1: State.** In `src/app.h`:
  - add `#include "baranim.h"`;
  - in `Entity`, after `int maxStamina, stamina;`, add:

```c
    BarAnim hpAnim, staminaAnim; /* HP panel animation; memory only */
    unsigned panelFrame;         /* the last frame the HP panel showed this entity (0 = never) */
```

  - change `void gfx_draw_frame(const char *screenshot_path);` to return `bool`, with the comment
    `/* true while something (the HP panel bars) is still moving */`;
  - add `bool gfx_bars_moving(void);`.

  Check where entities are created (`entity_new_character` / calloc) so the new fields start zeroed.

- [ ] **Step 2: Drawing** in `src/graphics.c`, `draw_hp_panels`. Add file-level state above the function:

```c
static unsigned panel_frame = 1;   /* counts drawn frames; 0 means "never shown" on entities */
static Uint64 panel_last_ms;       /* time of the previous frame */
static bool bars_moving;           /* some bar moved in the previous frame */

bool gfx_bars_moving(void) { return bars_moving; }

/* draws one bar's fill: a light "just lost" part and the solid part */
static void draw_bar_fill(const BarAnim *a, float frame_x, float frame_w, float y, float h, Color solid,
                          Color light) {
    float sl, sh, ll, lh;
    bar_anim_spans(a, &sl, &sh, &ll, &lh);
    if (lh > ll) dr_fill_rect(frame_x + ll * frame_w, y, (lh - ll) * frame_w, h, light);
    if (sh > sl) dr_fill_rect(frame_x + sl * frame_w, y, (sh - sl) * frame_w, h, solid);
}
```

  At the start of `draw_hp_panels`, before the early return, so the time also advances when no panel is shown:

```c
    Uint64 now = SDL_GetTicks();
    float dt = bars_moving ? (float)(now - panel_last_ms) / 1000.0f : 0; /* idle: start at this frame */
    if (dt > 0.1f) dt = 0.1f;
    panel_last_ms = now;
    unsigned frame = ++panel_frame;
    bars_moving = false;
```

  `Entity *` is needed to update the state: `hp_panel_characters` fills an `EntityList`. If its items are
  `const Entity *`, cast or change the local to `Entity *`.

  Per shown panel, before drawing the bars:

```c
        float hpf = bar_fraction(e->health, e->maxHealth), stf = bar_fraction(e->stamina, e->maxStamina);
        if (e->panelFrame != frame - 1) { /* not shown in the previous frame: no animation */
            bar_anim_reset(&e->hpAnim, hpf);
            bar_anim_reset(&e->staminaAnim, stf);
        }
        e->panelFrame = frame;
        if (bar_anim_step(&e->hpAnim, hpf, dt)) bars_moving = true;
        if (e->maxStamina > 0 && bar_anim_step(&e->staminaAnim, stf, dt)) bars_moving = true;
```

  Replace the HP fill line `if (b.fill_w > 0) dr_fill_rect(b.fill_x, by, b.fill_w, bar_h, red);` with
  `draw_bar_fill(&e->hpAnim, frame_x, frame_w, by, bar_h, red, light_red);`. Replace the stamina fill line with
  `draw_bar_fill(&e->staminaAnim, frame_x, frame_w, sy, sh, green, light_green);`.
  Colours: `light_red = { 235, 130, 130, 255 }`, `light_green = { 150, 215, 150, 255 }`.
  - Keep `hp_bar_layout` for the injured and critical lines; they use the real value.
  - The `HpBar s` for stamina is then only used for its fill. Remove it if unused, so there are no warnings.

  `gfx_draw_frame` returns `bars_moving` at the end.

- [ ] **Step 3: Main loop** in `src/main.c`:
  - `gfx_draw_frame(screenshot);` becomes `bool animating = gfx_draw_frame(screenshot);`, declared before the loop
    so the next iteration sees it;
  - the idle check `if (!redraw && !scrolling && !scripted)` also needs `!animating`;
  - after the tick loop, `if (animating) redraw = true;`.

  While animating, the loop then draws a frame per iteration; FIFO present paces it to the display. When nothing
  moves, it is idle as before.

- [ ] **Step 4: Script action `@waitbars`** in `src/main.c`:
  - add `WAIT_BARS` to the `script_cond` kind enum;
  - in `script_cond_met`, `WAIT_BARS` is met when `!gfx_bars_moving()`;
  - in `run_script_line`, `@waitbars` sets `script_cond.kind = WAIT_BARS` with the usual 20 s deadline;
  - document it in the header comment: `@waitbars        wait (at most 20 s) until the HP panel bars stopped moving`.

  `@waitbars` right after a command may run before the next frame has seen the change. Use it as
  `@wait 2` then `@waitbars`.

- [ ] **Step 5:** `make && make test`; no warnings.

- [ ] **Step 6: Look.** With `S` set to the scratchpad
  (`/tmp/claude-1000/-home-zero345-school-PRIN-Projects-javaHerbelProjectToH-toh/6eea57fd-d5ae-42db-806e-2d22119ae81f/scratchpad`):

```bash
./toh --size 600x400 --auto-file resources/images/toolIcons/area.png \
  --exec "@click 300 200" --exec ':c name Mao h 20 ms 9' --exec "@click 300 200" --exec "@wait 5" \
  --exec ":c damage 8" --exec ":c spend 4" --exec "@wait 6" --exec "@shot $S/anim-mid.png" \
  --exec "@wait 2" --exec "@waitbars" --exec "@shot $S/anim-end.png" \
  --exec ":c damage 20" --exec "@wait 8" --exec "@shot $S/anim-neg.png" --exec ":q!"
```

  Read the PNGs and check:
  - `anim-mid.png`: the red fill is near 12/20 with a light red part up to about 20/20, and the green fill near
    5/9 with a light green part;
  - `anim-end.png`: no light parts; red at 12/20, green at 5/9;
  - `anim-neg.png`: HP −8/20, with the light red part left of the frame.

  Also check that the game goes idle afterwards: run without a script, damage once, and `top` shows near-0 % CPU
  after a second. Or reason it through from the code: `animating` becomes false.

- [ ] **Step 7:** Commit: "HP and stamina bars slide to new values, with a short trail on losses".

---

### Task 3: e2e and docs

**Files:** `tests/e2e_net.sh`, `gmcontrols.md`, `README.md`

- [ ] **Step 1: e2e.** Every screenshot whose panel pixels are checked must wait for the bars:
  - the host: `@wait 10" --exec "@shot $out/host-hp-full.png"` becomes
    `@wait 10" --exec "@waitbars" --exec "@shot $out/host-hp-full.png"`;
  - the host's `host-hp.png` after `@waitfile $out/hp-damaged`: add `@waitbars` after its `@wait 30`;
  - the client's `client-hp.png`: add `@waitbars` after its `@wait 30`.

  Check any other screenshot that `red()` / `green()` sample (`grep -n 'red \|green ' tests/e2e_net.sh`) the
  same way. Run both network modes and `./tests/e2e_save.sh`. All must be OK. Never loosen the pixel checks.

- [ ] **Step 2: Docs.**
  - `gmcontrols.md`, HP panel description: add one sentence: "When HP or stamina changes, the bar slides to the
    new value; a loss leaves a lighter part for a moment that shows how much was lost."
  - `README.md`: the Test hook paragraph lists `@waitnet`, `@waitfile` and `@touch`. Add `@waitbars` (waits until
    the HP panel bars stop moving).
  - `playercontrols.md`: no change.
  - `grep -n -i 'coming soon' gmcontrols.md playercontrols.md README.md` prints nothing.

- [ ] **Step 3:** Commit: "e2e waits for the bar animation; docs".
