# Live multiplayer (GM hosts, players join) — design

Date: 2026-11-21
Status: approved in brainstorming, waiting for spec review

## Goal

Several people play on the same table live over the internet:

1. **Presence:** tiles (and entities) selected by another person are visible to everyone, in that person's color
   and labelled with their name.
2. **Roles:** the host is the game master (GM) and can do everything. Players can only act with their own
   character, plus a few harmless shared actions. All rules are checked by code, the GM never has to approve
   moves.
3. **No server in between:** the GM's game is the server. Players connect to it directly with a join code.

## 1. User-facing behaviour

### Commands

| Command | Who | Behaviour |
|---|---|---|
| `:host [name]` | anyone not in a session | Starts hosting as GM (`name` defaults to `GM`). Opens the router port, copies the join code to the clipboard and logs it. |
| `:unhost` | GM | Ends the session. Clients are told "GM closed the session". |
| `:join <code> <name>` | anyone not in a session | Connects to a host. On success the table is replaced by the host's table. |
| `:leave` | player | Disconnects. The table stays as an ordinary offline table. |
| `:players` | everyone | Lists connected people with role and color. |
| `:kick <name>` | GM | Disconnects a player. |
| `:claim` | player | Asks the GM for the selected unowned character (see 3.3). |
| `:c owner <name>` / `:c owner none` | GM | Sets or clears the owner of the selected characters directly. |

All of them get tab completion and `:h` help text. `--port N` overrides the default port 41234, and the
environment variable `TOH_NO_PORTMAP=1` skips UPnP / NAT-PMP (tests, LAN-only use).

### Hosting

`:host` does, in order:

1. Listen on TCP port 41234 on IPv4 and IPv6 (dual stack where available).
2. Try to forward the port on the router: UPnP IGD (miniupnpc) first, then NAT-PMP (libnatpmp). The mapping has
   a one-hour lease and is renewed every 30 minutes while hosting, and removed on `:unhost` and on quit.
3. Collect addresses: the external IPv4 from the router (if a mapping worked), all global IPv6 addresses of the
   machine (`getifaddrs`), and the primary LAN IPv4 as a last resort.
4. Build the join code (section 2.2), copy it to the clipboard (`SDL_SetClipboardText`) and log it.

Log messages tell the GM what happened, for example:

- `Hosting on port 41234. Router port opened via UPnP. Join code copied to clipboard.`
- `Hosting on port 41234. Router did not open the port: forward TCP 41234 to this computer, or use a VPN such as Tailscale. Join code copied (LAN/VPN address only).`
- `Your router is behind carrier-grade NAT (100.64.x.x): players outside your network cannot reach you without a VPN such as Tailscale.`

Hosting still works when no mapping is possible. The code then contains the LAN / VPN address.

### Joining

`:join <code> <name>` tries every address in the code, IPv6 first, then IPv4, with a 4-second timeout each.
Names are 1–24 printable UTF-8 characters with no spaces, and are compared case-insensitively.

Client messages:

| Situation | Message |
|---|---|
| connected | `Joined <GM name>'s table as <name>.` |
| bad code (format) | `That is not a valid join code.` |
| no address reachable | `Could not reach the host. Is the GM still hosting and the port open?` |
| wrong secret (old code) | `The host rejected the join code. Ask the GM for a new one.` |
| name taken | `The name "<name>" is already in use at this table.` |
| protocol mismatch | `The host runs a different version of ToH.` |
| kicked | `You were removed from the table by the GM.` |
| connection lost | `Connection to the host lost.` |
| host ended | `The GM closed the session.` |

### Session behaviour

- On join, the client's table is replaced by the host's (background, entities, walls, initiative, waypoints,
  grid orientation, hex size). Camera, zoom, theme, console, selection and clipboard stay local.
- When a client leaves or loses the connection, it keeps the last table as an ordinary offline table. Its
  unsaved-changes state is "modified", so `:q` asks before throwing it away.
- The unsaved-changes guard runs only on the host while in a session. A connected client's `:q` never asks.
- `:save` works on clients (a local copy). `:load` on a client is refused (`Only the GM can load a table`).
- When the GM `:load`s, every client gets the new table as a fresh snapshot.
- A player who reconnects with the same name gets their characters back (ownership is by name).
- When a player leaves, the GM gets a log line (`Alice left the table.`). Their characters stay owned.

## 2. Network

### 2.1 Transport

TCP, one connection per player, all to the host. Players never talk to each other.

A network thread (`net.c`) owns all sockets and runs a `poll()` loop. It:

- accepts connections and does the handshake,
- decrypts incoming frames, decodes them with `netmsg_decode` and passes the decoded message to the main thread
  through a locked queue, then wakes the main loop with an SDL user event (`netEvent`, like `dialogEvent`),
- encrypts and sends frames queued by the main thread. The queue is per connection with a cap of 64 MiB; a client
  whose queue overflows is disconnected (it is not reading).

The main thread never blocks on the network. All table changes still happen on the main thread only.

Frames: `u32 length | ciphertext` (little endian). The maximum frame is 64 MiB, which bounds snapshots and images
(a larger table cannot be hosted; `:host` / `:load` logs that).

### 2.2 Join code

`toh1-` followed by Crockford base32 (no padding, grouped by `-` every 4 characters for readability, groups
ignored on input) of:

```
u8  version (1)
u16 port
16 bytes secret (randombytes_buf)
u8  address count (1..8)
{ u8 family (4 | 6), 4 or 16 bytes address }...
```

A typical code with one IPv4 and one IPv6 address is about 75 characters. `:host` makes a new secret each time,
so old codes stop working when the GM hosts again.

### 2.3 Handshake and encryption (libsodium)

1. Client connects and sends `"TOH1" | 24-byte client nonce`.
2. Host replies with `"TOH1" | 24-byte server nonce`.
3. Both derive two 32-byte keys: `k = BLAKE2b-512(key = secret, msg = "toh-session" | client nonce | server nonce)`,
   first half for client→host, second half for host→client.
4. Each direction is a `crypto_secretstream_xchacha20poly1305` stream. The sender's first frame is the stream
   header. Every frame after that is one `push`.
5. The client's first message is `HELLO { protocol version, name }`.

A peer without the secret cannot produce a frame that decrypts, so the host drops it before any message is
parsed. The host closes a connection that has not completed `HELLO` within 5 seconds, allows at most 5 handshakes
per source address per minute and at most 32 connections in total.

### 2.4 Messages (`netmsg.c`)

All messages are `u8 type | body`, with integers as varints like the save state. The decoder checks every length,
count and value against fixed limits, like `savefmt.c`, and never trusts IDs (unknown IDs are rejected by
`ops_check`, not by the decoder).

| Type | Direction | Body |
|---|---|---|
| `HELLO` | C→H | protocol version, name |
| `WELCOME` | H→C | your player id, then `SNAPSHOT` follows |
| `REJECT` | H→C | reason code (name taken, version, kicked, full) then close |
| `SNAPSHOT` | H→C | savefmt v2 file bytes (the table) |
| `SESSION` | H→C | entity ids in snapshot order, player list (id, name, color, role) |
| `PLAYER_JOINED` / `PLAYER_LEFT` | H→C | player id, name, color |
| `OP` | C→H | client sequence number, op |
| `OP_APPLIED` | H→C | player id of the author, op (broadcast to everyone, including the author) |
| `OP_REJECTED` | H→C | client sequence number, reason code |
| `PRESENCE` | C→H, H→C | player id (H→C only), presence data (section 4) |
| `IMAGE` | C→H, H→C | 32-byte BLAKE2b hash of the WebP data, image kind, WebP bytes |
| `LOG` | H→C | text line shown in the console (dice results, joins, claim answers) |
| `CLOSE` | H→C | reason (host ended, kicked) |

The protocol version starts at 1. Host and client must have the same version.

**Images in ops.** Ops never carry pixels. They reference images by hash. Before sending an op that references an
image the receiver has not seen yet, the sender sends an `IMAGE` message with it (WebP, encoded with the savefmt
rules: lossless for icons and walls, quality 90 for backgrounds). Every peer keeps a per-session cache
hash → `Texture`, filled by the snapshot and by `IMAGE` messages; the host checks that the WebP decodes and that
the hash matches before caching. Pasting 20 goblins therefore sends the goblin image once. An op that references
an unknown hash is a structural error.

**Ids of new entities.** Spawn and paste ops sent by clients carry no ids. The host assigns them in `ops_apply`
and the broadcast `OP_APPLIED` includes the assigned ids, so every peer uses the same ids.

## 3. Operations and permissions

### 3.1 Entity identity and ownership

- `Entity` gets `uint32_t id` (unique, never reused during a session; assigned by the host, `App.nextEntityId`)
  and `char owner[25]` (player name, empty = no owner).
- The save format moves to **version 2**: characters store the owner name (empty string allowed). Version 1 files
  still load, with no owners. Entity ids are not saved; they are assigned on load and sent in `SESSION`.
- Players are identified by a session-local `uint8_t` player id. The GM is player 0. Ownership checks compare the
  author's name with `owner`.

### 3.2 The op layer (`ops.c`)

Every change to the table goes through:

```c
typedef struct Op { OpType type; ... } Op;      /* tagged union, one variant per op */
void   ops_submit(const Op *op);                 /* called by input.c / console.c / game.c / save.c */
OpCode ops_check(const Op *op, int author);      /* OP_OK or a rejection reason */
void   ops_apply(const Op *op, int author);      /* the only code that changes table state */
```

Behaviour of `ops_submit`:

- **Offline:** `ops_check` with author = GM, then `ops_apply`. The game behaves exactly as it does today.
- **Host:** `ops_check` with the GM as author, `ops_apply`, broadcast `OP_APPLIED`.
- **Client:** `ops_check` locally (instant feedback, no round trip for an illegal move), then send `OP`. The
  client does **not** apply it yet. The host runs `ops_check` again (authoritative), applies it and broadcasts
  `OP_APPLIED` to everyone; the client applies the op when it arrives. If the host rejects it, the client logs
  the reason.

Because the host orders all ops, every client applies the same sequence and stays identical to the host.

The existing code that changes table state directly (`game_move_character`, spawning, deleting, stat changes,
initiative, waypoints, background, grid orientation, teleport, paste, load) is changed to build an `Op` and call
`ops_submit`. The bodies of those functions become the `ops_apply` cases.

### 3.3 Op list and rules

| Op | Comes from | GM | Player |
|---|---|---|---|
| `MOVE id → hex` | Shift + click, keyboard move | always (with `:gm` on: no range limit, as today) | see "Move rules" below |
| `TELEPORT id → hex` | `:tp` | yes | no |
| `SPAWN_CHARACTER` (tiles, stats, image) | `:c` | yes | no |
| `SPAWN_WALL` (tiles, image) | `:w` | yes | no |
| `PASTE` (entities, offset) | Ctrl + V | yes | no |
| `DELETE ids` | `:c delete`, `:w delete`, `:e delete` | yes | no |
| `SET_STAT id field value` | `:c size/maxhealth/armorclass/speed/initiative` | yes | no |
| `SET_ICON id image` | `:c icon` | yes | no |
| `SET_BACKGROUND image` / clear | `:b` | yes | no |
| `GRID_ORIENTATION` | `:g` | yes | no |
| `WAYPOINT_CREATE` / `WAYPOINT_DELETE` | `:tp waypoint …` | yes | no |
| `INIT_ADD` / `INIT_REMOVE` / `INIT_CLEAR` / `INIT_SHOW` | `:i …` | yes | no |
| `INIT_NEXT` | `:i next` | yes | only if the current turn's character is theirs |
| `ROLL spec` | `:r` | yes | yes |
| `CLAIM id` | `:claim` | — | yes, if the character is unowned (needs GM answer) |
| `SET_OWNER id name` | `:c owner …` | yes | no |
| `LOAD` (snapshot) | `:load` | yes | no |

**Move rules for players.** A `MOVE` is accepted when all of these hold:

1. The entity exists, is a character and its owner is the author.
2. Initiative is empty, **or** the current turn's entity is this character (and the turn has started).
3. The target is in the character's movement range from its current tile, as the green range computes it today:
   `astar_range(tile, speed, entity)` (walls and other characters block; footprint of the character's size fits).

**Dice.** For `ROLL` the host rolls (`dice_roll` on the host) and broadcasts the result as a `LOG` line to
everyone: `Alice rolled 2d6+3: 4 + 2 + 3 = 9`. Players cannot fake results. The dice markers that `:r` shows
today are shown on every screen.

**Claims.** `:claim` with exactly one selected unowned character sends `CLAIM`. The host logs
`Alice wants to claim Goblin #3 [4|9]. Allow? (y/n)` and uses the console confirm prompt. Claims arriving while a
prompt is open are queued and asked one after another. `y` applies `SET_OWNER`, `n` sends the player
`The GM declined your claim.` The console's confirm prompt is generalized from the hard-wired
`gfx_spawn_character_confirmed` to a callback with context, so spawn confirmations and claims share it.

**Rejection reasons** (logged on the client): `Not your character`, `Out of range`, `Not <name>'s turn`,
`Only the GM can do that`, `That character already has an owner`, `That entity no longer exists`.

### 3.4 Local actions (never ops)

Selecting tiles / entities, panning, zooming, theme, `:d` debug, console and toolbox size, `:cl` (clears the
console), copying to the clipboard, `:save`, `:h`, `:gm` on the host. `:gm` on a client is refused
(`Only the GM can do that`). Tools (measure, line, area, cone) stay local state and are shown to others through
presence (section 4), so they need no ops.

## 4. Presence

### 4.1 Data

Each person's presence is:

- selected tiles (`selectedTiles`, at most 4096; longer selections are cut off for others),
- selected entity ids,
- movement preview: the path and footprint shown while Left Shift is held,
- tools: the current tool mode with its measurement chain (finished and in-progress segments) or the line /
  area / cone attack in progress.

A client sends `PRESENCE` only when this changes, at most 20 times per second (coalesced). The host keeps the
latest presence per player and forwards it to everyone else; the GM's presence is sent the same way. A new
client gets everyone's current presence after the snapshot. Presence is never saved and is dropped when the
person leaves.

### 4.2 Colors

The GM is always gold (`#E0B000`). Players get the first free color of a fixed palette:
blue `#3A7BFF`, red `#E5484D`, green `#2DA44E`, purple `#8E4EC6`, orange `#F76B15`, cyan `#12A5B5`,
pink `#D6409F`, lime `#7CB518`. The ninth and later players reuse colors in order. Colors are assigned by the
host and sent in `SESSION` / `PLAYER_JOINED`.

### 4.3 Drawing

Remote presence is drawn after the grid and before the local markers and selection, so your own view stays on top:

- **Selected tiles:** hexagon fill in the person's color at 35 % alpha and a 2 px outline in the full color. When
  several people select the same tile, each outline is drawn inset by an extra 3 px so all stay visible.
- **Name label:** a rounded pill in the person's color with the name in white or black (whichever contrasts
  more), above the topmost selected tile (ties: leftmost). Labels of several people on the same tile stack
  upwards. The label scales with the console font size, not with zoom, so it stays readable.
- **Selected entity:** a 3 px ring in the person's color around the character, plus the name label.
- **Movement preview:** the path as a dashed line in the person's color and the footprint outlined at its end.
- **Tools:** the measurement chain, line, area or cone drawn like today's local tools but in the person's color,
  with their length labels.

Your own selection and tools look exactly as they do today.

## 5. Error handling

- A message that fails to decode, or an op from a client that fails a structural check (unknown op type, bad
  sizes), disconnects that client with a host log line. The host keeps running.
- `ops_check` rejections are normal and only send `OP_REJECTED`.
- Port mapping failure is not an error (see Hosting). A mapping that cannot be removed on quit (crash) expires
  after its one-hour lease.
- If the host cannot encode a snapshot (`savefmt_encode` fails or the frame limit is exceeded), the joining
  client gets `REJECT` and the GM a log line.
- A client that receives an `OP_APPLIED` it cannot apply (unknown entity id) treats it as a desync: it logs
  `Lost sync with the host, rejoining…` and rejoins automatically once with the same code and name.

## 6. Code structure

### New files

| File | Content |
|---|---|
| `src/netmsg.c/.h` | Message and op encoding / decoding, join code encode / decode. Plain C, libsodium only (like `savefmt.c`). |
| `src/net.c/.h` | Sockets, network thread, handshake, encryption, send / receive queues, SDL wake-up event. |
| `src/portmap.c/.h` | UPnP / NAT-PMP mapping, external address, lease renewal, CGNAT detection. |
| `src/session.c` | Session state (role, players, colors, presence), command handlers for `:host` etc., message dispatch. |
| `src/ops.c` | `Op`, `ops_submit`, `ops_check`, `ops_apply`. |

### Changes to existing files

- `app.h`: `Entity.id`, `Entity.owner`, `App.nextEntityId`, session / presence structs, op and session prototypes.
- `entity.c`, `game.c`, `graphics.c`, `input.c`, `console.c`: table changes go through `ops_submit`; new commands;
  generalized confirm prompt; remote presence drawing in `graphics.c`.
- `savefmt.c/.h`, `save.c`: version 2 with owner names; save / load in a session (snapshot broadcast, client
  restrictions); load from memory for snapshots.
- `main.c`: `netEvent` handling, `--port`, network shutdown on quit; new `--exec` actions for tests
  (`@waitnet N` waits until N players are connected or the client has joined).
- `Makefile`: `miniupnpc` and `libnatpmp` (pkg-config, falling back to `-lminiupnpc -lnatpmp`), new tests.
- `README.md`: multiplayer section, new commands, new dependencies.

## 7. Testing

- **Unit (`make test`):** netmsg round trips for every message and op; join code round trip and rejection of bad
  codes; savefmt v2 round trip with owners and v1 compatibility; `ops_check` rule tests on a small table built
  without a window (own vs. other character, turn rule on / off, out of range, blocked by wall, GM override,
  `INIT_NEXT` permission, claims of owned characters).
- **Fuzz (`make fuzz`):** a second libFuzzer target for `netmsg_decode` and the join code parser.
- **End-to-end (`make e2e`):** `tests/e2e_net.sh` starts a host and a client on localhost with
  `TOH_NO_PORTMAP=1` and `--exec` scripts. It checks by screenshot comparison that the client's legal move shows
  on the host, that an illegal move (another player's character, out of turn) is refused, and that the GM's
  selection shows on the client in gold with the label "GM".

## Out of scope

- Hidden / GM-only entities and fog of war.
- Chat (dice and log lines cover basic messages).
- NAT hole punching or relays (they need a server).
- Windows builds of the network code (written against POSIX sockets behind the small `net.c` interface).
- Spectators, several GMs, handing the GM role to someone else.
- Undo.
