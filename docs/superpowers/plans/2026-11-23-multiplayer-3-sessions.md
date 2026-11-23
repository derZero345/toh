# Multiplayer, Plan 3 of 4: Networking, port mapping and sessions — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The GM hosts the table with `:host`, players join with `:join <code> <name>`, and every change made by
anyone shows up on every screen, with the rules from Plan 1 enforced by the host.

**Architecture:** `src/net.c` runs one network thread per `Net` (host or client): a `poll()` loop, a nonce handshake,
a libsodium secretstream per direction, per-connection queues. It knows nothing about SDL; it wakes the main loop
through a callback (`SDL_PushEvent(gh->netEvent)`). `src/session.c` runs on the main thread: commands, the host's
authoritative op handling (check → apply → broadcast `OP_APPLIED`), the client side (apply what the host
broadcasts, detect desyncs), an image cache keyed by the BLAKE2b hash of the WebP bytes, dice rolled by the host and
claims answered through a generalized console prompt. `src/portmap.c` opens the router port (UPnP, then NAT-PMP) in
its own thread when miniupnpc and libnatpmp are installed, and collects the join code's addresses.

**Tech Stack:** C11, POSIX sockets + pthreads, libsodium, SDL3 (events, clipboard), optional miniupnpc / libnatpmp,
ImageMagick for the e2e test.

**Spec:** `docs/superpowers/specs/2026-11-21-multiplayer-design.md` (sections 1, 2.1, 2.3, 3, 5, 6, 7).

**Status:** executed 2026-11-23 on branch `multiplayer`: commits `3d18e2c` (net), `88d11cb` (sessions), `6e832a5`
(claims e2e, README) and the docs commit. Headers, the e2e script and the key snippets are below; the large
implementation files (`src/net.c`, `src/session.c`, `src/portmap.c`, `tests/net_test.c`) are in those commits and not
repeated here.

---

## Decisions (where the spec left room or had to change)

- **Clients keep their own zoom.** The spec lists "hex size" as table state but also says zoom stays local; in this
  code `hexSize` *is* the zoom. Snapshots replace background, entities, walls, initiative, waypoints and grid
  orientation; camera, zoom and theme stay (`apply(d, keep_view)` in `save.c`).
- **ROLL carries the results.** The host's `OP_APPLIED(ROLL)` has every die and the total in `NetOp.values`, so every
  screen shows the same dice markers and log line (`Alice rolled 2d6+3: 4 + 2 + 3 = 9`). Offline `:r` is unchanged.
- **`net.c` is instance-based** (`Net *`), not a singleton, so `tests/net_test.c` runs a host and clients in one
  process.
- **Port mapping is optional at build time.** The Makefile links miniupnpc / libnatpmp only when a test program
  against them links (`HAVE_PORTMAP`); otherwise the build prints a note and `:host` uses LAN / VPN addresses.
  The real UPnP / NAT-PMP code has **not been compiled yet** (the libraries were not installed during execution).
- **Join code addresses:** router's external IPv4 (unless private / CGNAT), global IPv6 (2000::/3), then LAN / VPN
  IPv4; `127.0.0.1` if nothing else. Up to 8.
- **Clients are never GM:** joining turns `:gm` mode off; `:gm` on a client is refused.
- **Too large tables:** `:host` encodes the table once and refuses to host over 64 MiB; a too large `:load` while
  hosting ends the session.
- **`NET_REJECT_FULL` is never sent** in practice: `net.c` already caps connections at 32, which matches the 33
  player slots (GM + 32).
- **Desync:** the client checks every broadcast op with `ops_check` as GM (structure only); a failure, an unknown
  image hash or a snapshot that does not load logs `Lost sync with the host, rejoining...` and rejoins once.
- **Test hooks:** `TOH_JOINCODE_FILE=<path>` makes the host write its join code to a file; script actions `@waitnet N`,
  `@waitfile FILE`, `@touch FILE` (20 s timeout, exit status 3).

## File structure

| File | Status | Responsibility |
|---|---|---|
| `src/net.h`, `src/net.c` | create | network thread, handshake, encryption, queues, limits |
| `src/session.h`, `src/session.c` | create | session state, commands, host / client dispatch, image cache, dice, claims |
| `src/portmap.h`, `src/portmap.c` | create | UPnP / NAT-PMP thread, lease renewal, join code addresses |
| `src/netmsg.h`, `src/netmsg.c` | modify | `NOP_ROLL` values (results) |
| `src/ops.h`, `src/ops.c` | modify | `OPR_ALREADY_OWNED`, `Op.remote`, `Op.keepIds`, session routing in `ops_submit` |
| `src/entity.c`, `src/app.h` | modify | `entity_register_as`, `entities_set_ids`, `App.netEvent`, prompt / dice / save prototypes |
| `src/savefmt.h`, `src/savefmt.c` | modify | `savefmt_encode_image`, `savefmt_decode_image` |
| `src/save.c` | modify | `save_encode_table`, `save_apply_snapshot`, `save_mark_modified`, client rules for `:load` / `:q` |
| `src/console.c`, `src/graphics.c` | modify | `console_confirm` (callback + context), `console_show_dice`, new commands and help |
| `src/main.c` | modify | `netEvent`, `--port`, `@waitnet` / `@waitfile` / `@touch`, `session_shutdown` on quit |
| `tests/net_test.c` | create | localhost exchange, wrong secret, junk, HELLO timeout, unreachable, free while connecting |
| `tests/core_test.c`, `tests/core_stubs.c` | modify | host ids, remote view state, client submit; session stubs |
| `tests/save_test.c`, `tests/netmsg_test.c` | modify | single images; ROLL results |
| `tests/e2e_net.sh` | create | host + client on localhost: join, moves, refusal, GM changes, dice, claim, quit guard |
| `Makefile`, `README.md` | modify | pthread, optional port mapping, `net_test`, `e2e_net.sh`; "Playing together" |

---

### Task 1: ROLL results on the wire

- [x] **Step 1:** In `tests/netmsg_test.c` give the sample `NOP_ROLL` three values, check they round-trip, that a roll
  without values round-trips, and that more than `NETMSG_MAX_ROLLS` values or a NaN are refused.
- [x] **Step 2:** `src/netmsg.h`: `#define NETMSG_MAX_ROLLS 1001`. `src/netmsg.c`: `op_ok` checks the values for
  `NOP_ROLL`; `put_op` / `get_op` write / read `uv count, f64 values` after the dice string.
- [x] **Step 3:** `make build/netmsg_test && ./build/netmsg_test` → `424 checks, 0 failed`.

### Task 2: `net.c`

- [x] **Step 1: Interface** (`src/net.h`):

```c
#ifndef TOH_NET_H
#define TOH_NET_H

/*
 * TCP connections between the host and its players, encrypted with libsodium.
 *
 * Each Net runs one network thread (poll loop) that accepts / connects, does the handshake,
 * encrypts and decrypts frames and decodes messages with netmsg_decode. The main thread
 * sends with net_send (never blocks) and collects events with net_next. After new events
 * the thread calls the wake callback (main.c pushes an SDL event). POSIX sockets and pthreads,
 * no SDL, so tests can run a host and a client in one process.
 *
 * Wire: client sends "TOH1" | 24-byte nonce, host answers "TOH1" | 24-byte nonce. Both derive
 * k = BLAKE2b-512(key = secret, "toh-session" | client nonce | host nonce): first half for
 * client->host, second half for host->client. Then frames: u32 length (LE) | ciphertext; the
 * first frame in each direction is the crypto_secretstream_xchacha20poly1305 header, every
 * further frame is one pushed message.
 * Design: docs/superpowers/specs/2026-11-21-multiplayer-design.md, section 2.
 */

#include "netmsg.h"

#define NET_MAX_CONNS          32      /* host: connections, handshakes included */
#define NET_HELLO_TIMEOUT_MS   5000    /* host: time until the first message */
#define NET_CONNECT_TIMEOUT_MS 4000    /* client: per address */
#define NET_MAX_QUEUE          (64u << 20) /* bytes waiting to be sent to one connection */
#define NET_HANDSHAKES_PER_MIN 5       /* host: per source address */

typedef struct Net Net;

typedef enum { NET_EV_CONNECTED, NET_EV_MESSAGE, NET_EV_CLOSED } NetEventType;

typedef enum {
    NET_END_NORMAL,      /* net_close, or the peer closed cleanly after we asked */
    NET_END_LOST,        /* the peer closed or the connection broke */
    NET_END_UNREACHABLE, /* client: no address in the join code answered */
    NET_END_BAD_DATA,    /* a frame did not decrypt or a message did not decode */
    NET_END_TIMEOUT,     /* host: no message within NET_HELLO_TIMEOUT_MS */
    NET_END_OVERFLOW,    /* the peer does not read: more than NET_MAX_QUEUE bytes pending */
} NetEnd;

typedef struct {
    NetEventType type;
    int conn;       /* connection id, > 0; a client's connection to the host is always 1 */
    NetMsg msg;     /* NET_EV_MESSAGE; msg.data points into frame */
    uint8_t *frame; /* owned by the event */
    NetEnd end;     /* NET_EV_CLOSED */
    char addr[64];  /* peer address, for log lines */
} NetEvent;

typedef void (*NetWakeFn)(void *ctx);

/* Listens on port (0 = any free port) on IPv6 and IPv4. Returns NULL and fills err on failure. */
Net *net_host(uint16_t port, const uint8_t secret[NET_SECRET_SIZE], NetWakeFn wake, void *ctx, char *err, size_t errcap);
/* Connects to the addresses of code in the background (IPv6 first). The result is a
 * NET_EV_CONNECTED or NET_EV_CLOSED (NET_END_UNREACHABLE) event for connection 1. */
Net *net_join(const JoinCode *code, NetWakeFn wake, void *ctx);
uint16_t net_port(const Net *n); /* host: the port it listens on */

/* Queues m for conn. Returns false when m cannot be encoded or conn is not open. */
bool net_send(Net *n, int conn, const NetMsg *m);
/* Sends what is queued for conn (at most 2 s more), then closes it. A NET_EV_CLOSED follows. */
void net_close(Net *n, int conn);

/* Takes the next event. Free it with net_event_free. */
bool net_next(Net *n, NetEvent *ev);
void net_event_free(NetEvent *ev);

/* Flushes queued messages (at most 1 s), closes everything and stops the thread. NULL is fine. */
void net_free(Net *n);

#endif
```

- [x] **Step 2: Tests** (`tests/net_test.c`): a host on port 0 and a client on 127.0.0.1 exchange HELLO, a 5 MiB
  SNAPSHOT and ten LOG lines in order; invalid messages and unknown connections are refused by `net_send`; `net_close`
  flushes first (client sees CLOSE, then `NET_END_LOST`; host sees `NET_END_NORMAL`). Wrong secret: the host drops the
  connection with `NET_END_BAD_DATA` and delivers no message. A peer sending HTTP is dropped at once; a silent peer
  after `NET_HELLO_TIMEOUT_MS`; neither reaches the main thread. A closed port gives `NET_END_UNREACHABLE`; `net_free`
  during a 4 s connect returns in under 1.5 s. Makefile: `build/net_test` (with `-pthread`) in `make test`.
- [x] **Step 3: Implement** `src/net.c` (commit `3d18e2c`). Key points: host state `CS_GREET` → `CS_HEADER` →
  `CS_OPEN` (announce `NET_EV_CONNECTED`); frames over `NET_MAX_FRAME + ABYTES` or a header frame of the wrong size end
  the connection; decrypt or decode failure ends it with `NET_END_BAD_DATA`; queued output over `NET_MAX_QUEUE` ends it
  with `NET_END_OVERFLOW`; 5 handshakes per address and minute; wake callback only when the event queue was empty.
- [x] **Step 4:** `./build/net_test` → `49 checks, 0 failed`; also clean under `-fsanitize=thread` and
  `-fsanitize=address,undefined`.
- [x] **Step 5: Commit** `Add net: encrypted connections between host and players`.

### Task 3: Core support for remote ops

- [x] **Step 1: Tests** in `tests/core_test.c`: `test_host_ids` (spawn with `keepIds` uses the host's ids, later local
  ids are larger, `entities_set_ids` refuses wrong counts and duplicates), `test_remote_view_state` (a remote move or
  delete of another entity keeps the local path and selection; a remote move of the selected entity clears its
  highlights), `test_client_submit` (a client sends checked ops without applying them and sends nothing for refused
  ones). `tests/core_stubs.c` gets `session_is_client` / `session_is_host` / `session_my_name` / `session_send_op` /
  `session_broadcast_op` stubs.
- [x] **Step 2: Implement:** `OPR_ALREADY_OWNED`; `Op.remote`, `Op.keepIds`; `apply_spawn` uses the given ids;
  `apply_move` / `apply_delete` respect `remote`; `ops_local_author` returns the player's name on a client;
  `ops_submit`:

```c
    if (session_is_client()) {
        session_send_op(op); /* applied when the host broadcasts it */
        return OPR_OK;
    }
    ops_apply(op);
    if (session_is_host()) session_broadcast_op(op);
    return OPR_OK;
```

  `entity.c`: `entity_register_as(e, id)` and `entities_set_ids(ids, n)`.
- [x] **Step 3:** `./build/core_test` → `159 checks, 0 failed`.

### Task 4: Images and snapshots

- [x] **Step 1:** `tests/save_test.c` `test_single_image`: lossless round trip is pixel-exact, truncated data and bad
  sizes / kinds are refused. `src/savefmt.c`: `savefmt_encode_image` (wraps `encode_webp`) and
  `savefmt_decode_image`. `./build/save_test` → `104 checks, 0 failed`.
- [x] **Step 2:** `src/save.c`: `save_encode_table` (shared by `:save` and snapshots; caches WebP data on textures),
  `save_apply_snapshot` (keeps camera, zoom, theme; forgets the current file name), `save_mark_modified`;
  `:load` / the load dialog are refused on clients; `:q` never asks on a client; a host's `:load` calls
  `session_table_replaced()`.

### Task 5: Console prompt, dice and commands

- [x] **Step 1:** Replace `console_display_confirm` / `gfx_spawn_character_confirmed` with
  `bool console_confirm(const char *question, ConfirmFn fn, void *ctx)` and `console_confirm_busy()`; after the
  answer the console calls `session_prompt_free()` so queued claims get asked. Spawn confirmation becomes a callback.
- [x] **Step 2:** Move the dice markers into `console_show_dice(rolls, n, total)`; `cmd_roll` calls `session_roll`
  first.
- [x] **Step 3:** Commands `:host [name]`, `:unhost`, `:join <code> <name>`, `:leave`, `:players`, `:kick <name>`,
  `:claim` (completion names and `:h` texts); `:gm` refused on clients.

### Task 6: `session.c`, `portmap.c`, `main.c`, Makefile

- [x] **Step 1: Interfaces:**

```c
#ifndef TOH_SESSION_H
#define TOH_SESSION_H

/*
 * Multiplayer sessions: the GM hosts (:host), players join (:join) with a join code.
 * The host's table is authoritative: clients send their ops, the host checks and applies
 * them and broadcasts every applied op to everyone (OP_APPLIED), so all tables stay identical.
 * All of this runs on the main thread; net.c does the networking in the background.
 * Design: docs/superpowers/specs/2026-11-21-multiplayer-design.md, sections 1-3 and 5.
 */

#include "ops.h"

/* The GM is player 0 and always gold; players get the first free palette color. */
#define SESSION_GM_COLOR 0xE0B000u

void session_init(void);     /* after gfx_init: registers the wake-up event */
void session_shutdown(void); /* on quit: ends the session, removes the port mapping */
void session_poll(void);     /* on gh->netEvent: handles everything the network thread delivered */
void session_set_port(uint16_t port); /* --port */

bool        session_is_host(void);
bool        session_is_client(void); /* joining or joined */
const char *session_my_name(void);
/* @waitnet: host: at least n players joined; client: joined. */
bool        session_ready(int n);

/* Called by ops_submit. */
void session_send_op(const Op *op);      /* client: send to the host */
void session_broadcast_op(const Op *op); /* host: after applying the GM's own op */

void session_table_replaced(void); /* host: after :load, every player gets the new table */
bool session_roll(const char *spec); /* :r in a session: rolled by the host. false = offline */

/* Commands */
void session_cmd_host(const char *name);
void session_cmd_unhost(void);
void session_cmd_join(const char *code, const char *name);
void session_cmd_leave(void);
void session_cmd_players(void);
void session_cmd_kick(const char *name);
void session_cmd_claim(void);

/* The console's yes/no prompt became free: asks the next queued claim. */
void session_prompt_free(void);

#endif
```

```c
#ifndef TOH_PORTMAP_H
#define TOH_PORTMAP_H

/*
 * Opening the host's port on the router (UPnP IGD via miniupnpc, then NAT-PMP via libnatpmp)
 * and collecting the addresses for the join code. The mapping runs in its own thread: router
 * discovery takes seconds. The lease is one hour and renewed every 30 minutes; portmap_stop
 * removes it. Built without the libraries (no TOH_HAVE_PORTMAP), portmap_available() is false.
 */

#include "netmsg.h"

#define PORTMAP_LEASE_S 3600
#define PORTMAP_RENEW_S 1800

typedef struct Portmap Portmap;

typedef struct {
    bool done;           /* the first attempt finished */
    const char *method;  /* "UPnP", "NAT-PMP" or NULL when no mapping worked */
    bool have_external;  /* external IPv4 address from the router */
    uint8_t external[4];
    bool cgnat;          /* the router's external address is private or shared (100.64/10) */
} PortmapStatus;

bool     portmap_available(void);
Portmap *portmap_start(uint16_t port, void (*wake)(void *), void *ctx);
bool     portmap_status(Portmap *pm, PortmapStatus *out); /* thread-safe copy; false for NULL */
void     portmap_stop(Portmap *pm);                       /* removes the mapping; NULL is fine */

/* Addresses for the join code: the router's external IPv4 (st may be NULL), the machine's global
 * IPv6 addresses, then its LAN / VPN IPv4 addresses; 127.0.0.1 when there is nothing else. */
int portmap_collect_addrs(const PortmapStatus *st, NetAddr *out, int max);

/* 100.64.0.0/10 or a private (RFC 1918) address */
bool portmap_is_cgnat(const uint8_t a[4]);

#endif
```

- [x] **Step 2: Implement** `src/session.c` and `src/portmap.c` (commit `88d11cb`). Host flow: `NET_EV_CONNECTED` →
  slot; first message must be `HELLO` (else disconnect), version mismatch → `REJECT(VERSION)`, name taken
  (case-insensitive) → `REJECT(NAME_TAKEN)`, otherwise `WELCOME`, `SNAPSHOT`, `SESSION`, `PLAYER_JOINED` to the others.
  Client ops: `ROLL` → host rolls; `CLAIM` → checked and queued for the GM's y/n; other ops → `op_from_net` (unknown
  image → disconnect), `ops_check` as the player, `OP_REJECTED` or apply + `broadcast_applied` (sends missing
  `IMAGE`s per player first). Client flow: `HELLO` on connect; `SNAPSHOT` → `save_apply_snapshot` + cache; `SESSION`
  → `entities_set_ids`, player list, `Joined <GM>'s table as <name>.`; `OP_APPLIED` → `op_from_net`, `keepIds`,
  desync check, apply; messages for every `REJECT` / `CLOSE` reason and lost connections.
- [x] **Step 3: `main.c`:** `session_init()` after `save_init()`, `session_poll()` on `app.netEvent`, `--port N`,
  `@waitnet` / `@waitfile` / `@touch`, `session_shutdown()` before `gfx_shutdown()`, return `exit_status`.
- [x] **Step 4: Makefile:** `-pthread`; `HAVE_PORTMAP` detection adds `-DTOH_HAVE_PORTMAP` and the libraries.
- [x] **Step 5:** `make` (no warnings), `make test` (104 / 159 / 424 / 49 checks, 0 failed).

### Task 7: End-to-end test

- [x] **Step 1:** `tests/e2e_net.sh`:

```bash
#!/usr/bin/env bash
# End-to-end check of a multiplayer session: a host (GM) and a client (Bob) on localhost.
# Opens two windows (needs Wayland or X11) and uses ImageMagick to look at screenshots.
# The two programs synchronize with @touch / @waitfile.
# Usage: tests/e2e_net.sh   (or: make e2e)
set -euo pipefail
cd "$(dirname "$0")/.."

out=${KEEP:-$(mktemp -d)}
[ -n "${KEEP:-}" ] || trap 'rm -rf "$out"' EXIT
icon=resources/images/toolIcons/area.png
port=${TOH_TEST_PORT:-41299}
fail() { echo "FAIL: $*" >&2; exit 1; }
export TOH_NO_PORTMAP=1

# Is the tile at pixel X Y occupied in a screenshot? Empty tiles are light grey (about 0.9),
# the test icon is mostly black (about 0.65).
occupied() {
    local v
    v=$(magick "$1" -crop 16x16+$(($2 - 8))+$(($3 - 8)) +repage -colorspace gray -format '%[fx:mean]' info:)
    awk -v v="$v" 'BEGIN { exit !(v < 0.8) }'
}

# Tiles (pixel coordinates): A = Bob's character, B = the GM's, C = spawned after Bob joined.
A="300 200"; A2="300 260"    # Bob moves A one tile down
B="380 260"; B2="380 320"    # Bob tries to move B (refused); the GM moves B to B3
B3="440 260"
C="200 320"; C2="200 380"    # Bob claims C, the GM agrees, Bob moves it

TOH_JOINCODE_FILE="$out/code" timeout 90 ./toh --size 600x400 --port "$port" --auto-file "$icon" \
    --exec "@click $A" --exec ":c speed 6" --exec "@click $A" --exec ":c owner Bob" --exec "@click $A" \
    --exec "@click $B" --exec ":c speed 6" --exec "@click $B" \
    --exec ":host" \
    --exec "@waitnet 1" \
    --exec "@waitfile $out/client-moved" --exec "@wait 30" \
    --exec "@shot $out/host-moved.png" \
    --exec "@down lshift" --exec "@click $B3" --exec "@up lshift" \
    --exec "@click $C" --exec ":c speed 6" \
    --exec ":r 2d6" \
    --exec "@touch $out/host-changed" \
    --exec "@waitfile $out/claimed" --exec "@wait 30" --exec "y" --exec "@touch $out/granted" \
    --exec "@waitfile $out/client-checked" --exec "@wait 30" \
    --exec "@shot $out/host-claim.png" \
    --exec ":unhost" --exec "@wait 10" --exec ":q!" \
    > "$out/host.log" 2>&1 &
host=$!

for _ in $(seq 100); do [ -s "$out/code" ] && break; sleep 0.1; done
[ -s "$out/code" ] || fail "the host wrote no join code"
code=$(cat "$out/code")
[[ $code == toh1-* ]] || fail "unexpected join code: $code"

# The client's final :q must be refused (after the session ended the table counts as modified);
# :q! then quits.
timeout 90 ./toh --size 600x400 \
    --exec ":join $code Bob" --exec "@waitnet 1" --exec "@wait 10" \
    --exec "@shot $out/client-joined.png" \
    --exec "@click $A" --exec "@down lshift" --exec "@click $A2" --exec "@up lshift" --exec "@wait 30" \
    --exec "@click $B" --exec "@down lshift" --exec "@click $B2" --exec "@up lshift" --exec "@wait 30" \
    --exec "@shot $out/client-moved.png" --exec "@touch $out/client-moved" \
    --exec "@waitfile $out/host-changed" --exec "@wait 30" \
    --exec "@shot $out/client-changed.png" \
    --exec "@click $C" --exec ":claim" --exec "@touch $out/claimed" \
    --exec "@waitfile $out/granted" --exec "@wait 30" \
    --exec "@down lshift" --exec "@click $C2" --exec "@up lshift" --exec "@wait 30" \
    --exec "@shot $out/client-claim.png" --exec "@touch $out/client-checked" \
    --exec "@wait 60" \
    --exec ":q" --exec "@shot $out/client-quit-refused.png" --exec ":q!" \
    > "$out/client.log" 2>&1 || fail "the client exited with status $? (see $out/client.log)"
wait "$host" || fail "the host exited with status $? (see $out/host.log)"

# the client got the host's table
occupied "$out/client-joined.png" $A || fail "the client does not show Bob's character"
occupied "$out/client-joined.png" $B || fail "the client does not show the GM's character"
# Bob's legal move reached the host; his move of the GM's character did not
occupied "$out/host-moved.png" $A2 || fail "Bob's move did not show on the host"
! occupied "$out/host-moved.png" $A || fail "Bob's character is still at its old tile on the host"
occupied "$out/host-moved.png" $B || fail "the GM's character moved although Bob may not move it"
! occupied "$out/host-moved.png" $B2 || fail "Bob moved the GM's character"
# the GM's move and new character reached the client (the image too)
occupied "$out/client-changed.png" $B3 || fail "the GM's move did not show on the client"
occupied "$out/client-changed.png" $C || fail "the character the GM spawned did not show on the client"
# the GM granted Bob's claim, so Bob could move the new character
occupied "$out/host-claim.png" $C2 || fail "Bob could not move the character he claimed"
! occupied "$out/host-claim.png" $C || fail "the claimed character is still at its old tile"
[ -f "$out/client-quit-refused.png" ] || fail ":q quit the client although the session ended"

echo "e2e net: OK"
```

  `make e2e` runs it after `e2e_save.sh`.
- [x] **Step 2:** `make e2e` → `e2e save/load: OK (…)` and `e2e net: OK`. Also run against a build with
  `CFLAGS="-O1 -g -fsanitize=address,undefined"`: no sanitizer reports (with leak detection on, only libdbus leaks).
- [x] **Step 3: Commit** `Add multiplayer sessions: :host, :join and op replication` and
  `Test claims in the session e2e test; README: playing together`.

### Task 8: Docs

- [x] **Step 1:** README: dependencies table (optional miniupnpc / libnatpmp), "Playing together", source layout,
  tests paragraph, test hook actions.
- [x] **Step 2:** This plan; memory note.

---

## Not verified yet

- **Router port mapping** (`TOH_HAVE_PORTMAP` code in `src/portmap.c`) is unbuilt: install miniupnpc and libnatpmp,
  run `make`, then `:host` on a network with a UPnP router. Expected API: miniupnpc ≥ 2.2.8
  (`UPNP_GetValidIGD` with `wanaddr`, guarded by `MINIUPNPC_API_VERSION >= 18`).
- Not covered by automated tests: `:kick`, `:leave`, the desync rejoin, name-taken / version rejections, a host
  `:load` during a session, players joining over the internet (only localhost).

## Notes for Plan 4 (presence)

- `NM_PRESENCE` is still reserved in `netmsg.c`; define its body there (tiles ≤ 4096, entity ids, movement preview
  path + footprint, tool state). Tool positions are pixel `Vec2`s today; send hexes or world coordinates instead.
- `session.c` already has per-player colors (`Player.color`, `SESSION_GM_COLOR`) and the player list on every client.
- Throttle: coalesce presence to at most 20 messages per second per person; the host forwards the latest presence
  of each person and sends everyone's current presence to a new player after `SESSION`.
- `input.c`'s move code reselects the entity "at its new position" right after `ops_submit`; on a client the move
  is applied only when the host's broadcast arrives, so the selection ends up empty. Plan 4 can reselect when the
  own move comes back (`OP_APPLIED` with `player == myId`).
