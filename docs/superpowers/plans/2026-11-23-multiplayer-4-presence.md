# Multiplayer, Plan 4 of 4: Presence — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Everyone at the table sees what the others point at, in their color: selected tiles and characters with
a name label, the movement preview while Shift is held, and measurements / line / area / cone attacks.

**Architecture:** `NM_PRESENCE` gets its body in `netmsg.c` (tiles, entity ids, path, footprint, tool shapes in
zoom-independent world coordinates). `src/presence.c` turns the local state into a `NetPresence` and draws a list of
other people's. `session.c` sends the own presence after a frame was drawn when it changed (compared as encoded
bytes), at most every 50 ms; the host stores each player's latest presence, forwards it coalesced (also at most
every 50 ms per player) and sends everyone's current presence to a new player after `SESSION`.

**Tech Stack:** C11, the existing draw API (plus `dr_fill_polygon`), ImageMagick for the e2e check.

**Spec:** `docs/superpowers/specs/2026-11-21-multiplayer-design.md` section 4.

**Status:** executed 2026-11-23 on branch `multiplayer` in commit `ae27e03`; this document was written alongside the
code, like Plans 2 and 3.

---

## Decisions

- **World coordinates** for tool shapes: `(pixel - grid offset) / hexSize * 256` (`NET_WORLD_SCALE`), so each
  screen draws them with its own zoom and pan. Tiles, path and footprint travel as hexes.
- **Wire limits:** 4096 selected tiles (cut off), 256 entities, 4096 path tiles, 64 footprint tiles, 256 lines;
  world coordinates within ±10⁹, labels 0..10⁷ ft, cone start 0..359°, extent 0..360°.
- **Draw order:** remote presence is drawn right after the grid lines and before the local highlights, which live
  in the same grid layer, so the own selection stays on top (spec: "after the grid, before the local markers").
- **Labels** go above the topmost selected tile or entity (ties: leftmost), stack upwards per shared anchor, use
  the console font size and black or white text by luminance. People with only tools active get no label.
- **Change detection** compares the encoded message with the last one sent; it runs after every drawn frame, so
  it needs no hooks in the input code. Rejoining or a new `SESSION` forgets the last one, so the host gets it again.
- **Own move on a client** (Plan 3 note): when the own `OP_MOVE` comes back and nothing is selected, the client
  selects the moved character again (`io_select_entity`), like the offline reselect.

## File structure

| File | Status | Responsibility |
|---|---|---|
| `src/netmsg.h`, `src/netmsg.c` | modify | `NetHex`, `NetLine`, `NetCircle`, `NetPresence`; encode / decode / validate; `netpresence_copy` / `_free` |
| `src/presence.h`, `src/presence.c` | create | `presence_collect`, `presence_draw`, world coordinate conversion |
| `src/session.h`, `src/session.c` | modify | per-player presence, send / forward throttle, welcome, `session_tick`, `session_draw_presence` |
| `src/draw.h`, `src/draw.c` | modify | `dr_fill_polygon` |
| `src/graphics.c`, `src/main.c` | modify | draw call in the grid layer; tick after frames and in the idle timeout |
| `src/input.c`, `src/app.h` | modify | `io_select_entity` |
| `tests/netmsg_test.c`, `tests/e2e_net.sh` | modify | presence round trip and limits; both sides see the other's selection |

---

### Task 1: Wire format

- [x] **Step 1: Tests** (`tests/netmsg_test.c`): a sample presence with every part set round-trips, an empty one is
  valid, `netpresence_copy` is independent of the decoded message; too many tiles / entities / footprint tiles /
  lines, entity id 0, a tile out of range, a line out of range, a negative label or radius, cone start 360 and cone
  extent 361 are refused; truncated messages and a circle flag of 2 fail to decode.
- [x] **Step 2: Types** (`src/netmsg.h`):

```c
#define NETMSG_MAX_SEL_TILES    4096
#define NETMSG_MAX_SEL_ENTITIES 256
#define NETMSG_MAX_PATH         4096
#define NETMSG_MAX_FOOTPRINT    64
#define NETMSG_MAX_LINES        256
#define NETMSG_MAX_WORLD        1000000000
#define NETMSG_MAX_LABEL        10000000
#define NET_WORLD_SCALE         256

typedef struct { int32_t row, col; } NetHex;
typedef struct { int32_t ax, ay, bx, by; int32_t label; } NetLine;
typedef struct { int32_t x, y, radius; int32_t label; } NetCircle;
typedef struct {
    NetHex *tiles; uint32_t ntiles;
    uint32_t *entities; uint32_t nentities;
    NetHex *path; uint32_t npath;
    NetHex *footprint; uint32_t nfootprint;
    NetLine *lines; uint32_t nlines;
    bool has_circle; NetCircle circle;
    bool has_cone; NetCircle cone; int32_t cone_start, cone_extent;
} NetPresence;
/* NetMsg gains: NetPresence presence;  (PRESENCE: player + presence) */
void netpresence_copy(NetPresence *dst, const NetPresence *src);
void netpresence_free(NetPresence *p);
```

  Body: `u8 player`, hexes (`uv n`, `sv row, sv col` each), ids, path, footprint, `uv n` lines (5 `sv` each),
  `u8 has_circle` [+ 4 `sv`], `u8 has_cone` [+ 4 `sv` + start + extent]. Flags other than 0 / 1 are invalid.
- [x] **Step 3:** `./build/netmsg_test` → `446 checks, 0 failed`; `make fuzz` still runs clean.

### Task 2: Collecting and drawing

- [x] **Step 1: Interface** (`src/presence.h`):

```c
#ifndef TOH_PRESENCE_H
#define TOH_PRESENCE_H

/*
 * Presence (spec section 4): what each person at the table is pointing at. presence_collect turns the
 * local selection, movement preview and tools into a NetPresence; presence_draw shows other people's
 * presence in their colors. Positions travel as tiles or as world coordinates (NET_WORLD_SCALE per hex
 * size, relative to the grid origin), so every screen draws them with its own zoom and pan.
 */

#include "netmsg.h"

typedef struct {
    const NetPresence *p;
    uint32_t color; /* 0xRRGGBB */
    const char *name;
} RemotePresence;

/* Fills out from the local state; free with netpresence_free. */
void presence_collect(NetPresence *out);

/* Draws everyone in list (in that order; later people's outlines are inset when tiles are shared). */
void presence_draw(const RemotePresence *list, int n);

/* World coordinate conversions (exposed for tests). */
int32_t presence_world_x(double px);
int32_t presence_world_y(double py);
double  presence_px_x(int32_t wx);
double  presence_px_y(int32_t wy);

#endif
```

- [x] **Step 2: `presence_collect`:** `selectedTiles` (only coordinates the save format allows, at most 4096), the
  entities on `selectedEntityTiles` (deduplicated), `entityPreviewTiles` as footprint and `path` when the preview is
  shown, every measurement with a line (`measure_line` / `measure_length`), the line attack, the area attack (centre,
  radius, feet) and the cone when its radius exceeds one hex.
- [x] **Step 3: `presence_draw`:** per person, selected tiles filled at 35 % alpha with a 2 px outline, inset by
  3 px for every earlier person on the same visible tile (count array over the visible range); characters get a
  3 px ring, other entities outlined tiles; dashed path (`hexSize / 4` dashes) and footprint outline; tools drawn
  like the local ones in the person's color; then the name pill.
- [x] **Step 4:** `dr_fill_polygon` in `draw.c` (wraps `fill_convex`).

### Task 3: Session

- [x] **Step 1:** `Player` gets `presence`, `presenceDirty`, `presenceSent`; `player_clear` frees it. Session state
  keeps the last sent encoding.
- [x] **Step 2:** Host: `NM_PRESENCE` from a welcomed player replaces their presence and marks it dirty;
  `session_tick` forwards dirty ones when 50 ms have passed since that player's last forward. `welcome` sends every
  other person's presence after `PLAYER_JOINED`. The GM's own presence is broadcast and kept in `players[0]`.
- [x] **Step 3:** Client: `NM_PRESENCE` replaces the sender's presence (never our own); `SESSION` forgets the last
  sent presence. Own `OP_MOVE` reselects the character.
- [x] **Step 4:** `session_presence_changed()` after each drawn frame, `session_tick()` after it and before idling
  (the idle timeout shrinks to the pending send slot); `session_draw_presence()` in `draw_grid` after the lines.

### Task 4: Tests and docs

- [x] **Step 1:** `tests/e2e_net.sh`: after joining, the GM selects empty tile P and Bob empty tile Q (sync files
  `client-joined`, `host-selected`, `client-selected`, `host-saw`); a `tint` helper compares the red and blue
  channel means of a 12×12 crop: Bob sees P gold, the GM sees Q blue, nobody sees their own selection as presence
  and P is untinted before the GM selects it.
- [x] **Step 2:** `make test` (104 / 159 / 446 / 49), `make e2e` (both OK), the net e2e with an ASan/UBSan build
  (no reports), `make fuzz FUZZ_TIME=20` (no crashes). A manual two-window run showed a measurement, a cone, a
  selected character with ring and label, the dashed preview path and footprint identically on the client.
- [x] **Step 3:** README ("Playing together", source layout), this plan, memory note.

## Not covered by automated tests

- Inset outlines and stacked labels when several people select the same tile; the presence of 9+ players
  (colors repeat); the 4096-tile cut-off on a real selection.
- Bandwidth: a 4096-tile selection is about 25 KB per update; at 20 updates per second and many players this is
  noticeable but within the 64 MiB queue cap.
