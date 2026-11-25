# Player characters: ownership rings and creation requests

Status: approved design, 2026-11-25.

Two multiplayer features:
1. Players can see which characters are theirs, and everyone can see who owns what.
2. A player can create their own character (stats and picture). The GM sees the stats and the picture and
   accepts or declines.

## 1. Ownership rings

- **When:** only while hosting or joined (`session_is_host()` or a joined client). Offline nothing changes.
- **What:**
  - Every character with a non-empty `owner` gets a circle outline around its picture in the owner's color.
  - The circle has the same centre and diameter as the drawn picture (`entity_draw_size(e) * hexSize`).
  - Thickness: `max(2, hexSize / 15)` pixels.
  - On a player's own screen, rings of characters they own are drawn twice as thick.
- **Colors:**
  - Each owner's color comes from the session's player list. On the host that is `S.players`; on a client it is
    the list from `NM_SESSION`, kept up to date by `PLAYER_JOINED` / `PLAYER_LEFT`.
  - Owner names are compared case-insensitively, like `is_owner`.
  - An owner who is not at the table gets grey (`0x9AA0A6`).
  - Characters without an owner get no ring.
- **Code:**
  - `session.c` exposes `bool session_owner_color(const char *owner, uint32_t *color, bool *mine)`. It returns
    false when no ring should be drawn: offline, or an empty owner.
  - `graphics.c` draws the rings after `draw_entities()`, before the presence layer, so rings sit under the
    selection highlights.
- Everyone's screen shows the same rings because ownership is already part of the shared table. No new network data
  is needed.

## 2. Creating a character as a player

### Player side
- A joined player with **no entity selected** types `:c [stats]`. This is the same syntax as the GM, e.g.
  `:c speed 6 maxhealth 20 armorclass 12`.
- Tile:
  - the selected tile, if exactly one is selected;
  - otherwise the tile under the mouse;
  - more than one selected tile: "Select one tile".
- The file dialog asks for a PNG.
- The client cuts it to a circle (as now), scales it down to fit 512×512 when it is larger, and encodes it as
  lossless WebP.
- It then sends one `NM_CREATE` message. The log says "Asked the GM to create your character".
- With an entity selected, `:c` stays GM-only (editing): "Only the GM can do that".
- `:c owner` and `:c delete` stay GM-only.

### Wire: `NM_CREATE` (client → host only)
- Body: `seq` (varint) | character spec | `data`.
  - The spec is a `NetSpec` of type character without the image hash, so the field order is that of `NetSpec`.
  - `data` is the WebP bytes, at most `NET_CREATE_MAX_BLOB` = 1 MiB.
- The decoder checks the spec with the same rules as `spec_ok` and the blob length.
- The host computes the image hash itself from `data`.
- `NET_PROTOCOL_VERSION` goes from 1 to 2. A client or host with the other version gets the existing
  `NET_REJECT_VERSION` message, "The host runs a different version of ToH".

### Host checks on arrival
The host checks these in order; the first failure sends `NM_OP_REJECTED` with the request's `seq`:

| Check | Reason |
|---|---|
| The WebP decodes and is at most 512×512 | `OPR_INVALID` |
| Every tile the character would cover is free (`character_occupied_tiles` + `entity_at`) | new `OPR_TILE_TAKEN` "That tile is taken" |
| The player has fewer than `CLAIMS_PER_PLAYER` (3) requests waiting (claims and creations together) | `OPR_CLAIM_PENDING` |

After that the request joins the queue.

### The request queue
- `Claim` becomes `Request`, of one of two kinds:
  - claim: player, entity;
  - create: player, spec, tile, texture holding the decoded picture, hash.
- `ask_next_claim` becomes `ask_next_request`. A create request asks:
  `"<player> wants to create a character at [r|c]: size S, max health H, AC A, speed V, initiative I. Allow? (y/n)"`.
- When a player leaves, their waiting requests are dropped, as claims are now. The textures are released.

### GM preview
- While a create request's question is open, the GM's screen draws its picture at 50% alpha on the requested tile,
  at the size the character will have.
- Nothing is sent to anyone for the preview. It disappears when the question is answered or dropped.

### Answer
- **y:**
  - The host checks again that the player is still at the table and the tiles are still free. If either fails, it
    logs the reason on the GM's console and sends the player `OPR_TILE_TAKEN` or nothing (if they left).
  - Then the host:
    1. adds the texture to the image cache;
    2. submits an `OP_SPAWN` with the spec (health = max health), applied and broadcast like the GM's own spawns,
       which also sends the image to every player;
    3. submits an `OP_SET_OWNER` for the new id with the player's name.
  - The player gets the log line "The GM accepted your character".
- **n:** the player gets "The GM declined your character".

## 3. Not in scope

- Players still cannot edit or delete characters, including their own.
- No per-player limit on created characters beyond the GM's y/n.
- Walls cannot be requested.

## 4. Tests

- `tests/netmsg_test.c`:
  - `NM_CREATE` round trip;
  - rejections: a wall spec, an empty blob, a blob over 1 MiB, bad stats, trailing bytes;
  - a fuzz corpus sample.
- `tests/core_test.c`: `ops_result_text(OPR_TILE_TAKEN)`.
- `tests/e2e_net.sh`:
  - Bob creates a character on an empty tile with `:c speed 6` and the test icon.
  - The GM answers `y`.
  - The host and client screenshots show the character on that tile.
  - On the client's screenshot, Bob's characters have a blue ring and the GM's do not.
  - Then Bob moves the new character.
- Docs: `playercontrols.md`, `gmcontrols.md` and the README's multiplayer section describe the rings, player
  `:c`, the preview and the shared limit of 3 waiting requests.
