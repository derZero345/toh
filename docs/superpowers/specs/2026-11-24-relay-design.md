# Relay on the home server (no router setup for the GM) — design

Extends the live multiplayer design (`2026-11-21-multiplayer-design.md`), which deliberately left relays out of
scope. That design needs the GM's PC to be reachable from the internet (UPnP, a manual port forward or a VPN);
UPnP fails on common routers (e.g. a FRITZ!Box refuses `AddPortMapping` with 403 until port sharing is allowed).

## Goal

1. **The GM sets up nothing.** `:host` works behind any router, because the GM's game only opens outgoing
   connections.
2. **The relay is dumb and blind.** It copies bytes between sockets. It never sees the session secret, so the
   existing end-to-end encryption (section 2.3 of the multiplayer design) stays as it is.
3. **Private use.** One relay on the project owner's TrueNAS server for their group. No accounts, no public
   service; limits only protect the server from overload.
4. **Direct hosting stays** as `:host --direct` (LAN, Tailscale, routers with UPnP).

## 1. User-facing behaviour

### Commands

| Command | Change |
|---|---|
| `:host [name]` | Hosts via the relay. Logs `Connecting to the relay <host:port>...`, then `Hosting via relay <host:port>. Join code copied to clipboard.` No port mapping. |
| `:host --direct [name]` | The current behaviour: listen on the port (`--port N`), UPnP / NAT-PMP, address join code. |
| `:join <code>` | Unchanged for the user. A v1 code connects directly, a v2 code goes through the relay. |

The relay address is compiled in from the Makefile variable `RELAY` (`host:port`, default port 41234). The
environment variable `TOH_RELAY=host:port` overrides it (`TOH_RELAY=` turns the relay off). If neither is set,
`:host` behaves like `:host --direct`, and `:join` with a relay code says that no relay is set.

### Log lines and errors

| Situation | Log line |
|---|---|
| relay unreachable on `:host` (first attempt) | `Could not reach the relay <host:port>. Use :host --direct to host without it.` (not hosting) |
| room already exists (same secret, practically never) | `The relay refused the room. Try :host again.` |
| relay full | `The relay is full (too many rooms). Try again later.` |
| relay lost while hosting | `Lost the relay; players cannot join until it is back. Retrying...` — connected players keep playing |
| relay back | `Relay connection restored.` |
| `:join`, relay unreachable | `Could not reach the relay <host:port>.` |
| `:join`, no such room | `No game with this join code is hosted right now. Ask the GM for a new one.` |
| `:join`, GM did not attach in time | `The host did not answer.` |
| version mismatch (relay or game too old) | `The relay speaks a different version. Update the game or the relay.` |

## 2. Network

### 2.1 Connection model

Every player is its own TCP connection on the GM's side, as before; only who opens it changes.

```
player ──JOIN room──▶ relay ◀──HOST room── GM   (control connection, stays open)
                      relay ──WAITING ticket, player address──▶ GM
                      relay ◀──ATTACH room, ticket── GM   (new connection per player)
                      relay links player ⇄ attach connection, sends OK to the player,
                      then copies bytes both ways until either side closes
```

After the link the client sends the `TOH1` greeting and everything continues exactly as in the multiplayer design
(handshake, secretstream, `HELLO`, messages). On the GM side, an attach connection that got its `OK` is treated
like an accepted connection: same `Conn`, same greeting / stream setup / hello timeout.

### 2.2 Room ID and join code v2

- Room ID: `crypto_generichash` (BLAKE2b), 16-byte output, key = secret, message = `"toh-room"`. The GM and the
  players compute it; the relay only sees the room ID.
- Join code v2: `toh1-` followed by Crockford base32 (same grouping as v1) of `u8 version (2) | 16 bytes secret`
  (about 40 characters with the group dashes). `JoinCode` gets a `relay` flag (true for v2, no addresses). `joincode_decode` accepts
  version 1 and 2; `joincode_encode` writes version 2 when `relay` is set.
- The relay address is not in the code: both sides use their compiled-in / `TOH_RELAY` address.

### 2.3 Relay protocol

Plain bytes on TCP, before the encrypted protocol. All integers little endian. Every message starts with the
magic `"TOHR"` and a `u8` type. Version 1.

| Type | Direction | Body |
|---|---|---|
| `1 HOST` | GM → relay (control) | `u8 version`, `16 room` |
| `2 JOIN` | player → relay | `u8 version`, `16 room` |
| `3 ATTACH` | GM → relay (new connection) | `u8 version`, `16 room`, `u32 ticket` |
| `4 STATUS` | relay → any | `u8 code`: 0 OK, 1 room taken, 2 no such room, 3 full, 4 bad version, 5 timeout, 6 bad request |
| `5 WAITING` | relay → GM (control) | `u32 ticket`, `u8 len`, `len` bytes player address text (≤ 63) |
| `6 PING` | both on control | none; the relay sends one every 20 s, the GM answers with a `PING` |

- `HOST` → `STATUS`. After OK the control connection only carries `WAITING` and `PING`.
- `JOIN` → nothing until the GM attaches; then `STATUS OK` and the link. Otherwise `STATUS` with an error and close.
- `ATTACH` → `STATUS OK` and the link, or an error and close. Tickets are unique (a counter) and single use; the
  room ID is what only secret holders know.
- A connection that sends anything else, or no complete first message within 5 s, is closed (`bad request`).

### 2.4 Relay limits

- 64 rooms, 32 waiting or linked players per room (matches `NET_MAX_CONNS`), 1024 sockets in total. The relay
  raises its file descriptor limit to fit (up to the hard limit, else it lowers the socket limit); when `accept`
  still runs out of descriptors it stops accepting for 100 ms instead of spinning.
- "Source address" for the limits below: the IPv4 address, or the /64 prefix of an IPv6 address (one IPv6 host
  easily has a whole /64). Logs and `WAITING` carry the full address.
- At most 2 rooms per source address (a further `HOST` gets `STATUS full`).
- At most 5 `JOIN` per source address per minute (moved from the GM: behind the relay every player has the
  relay's address); over the limit the answer is `STATUS full`. `net.c` skips its own per-address check for relay
  connections.
- A `JOIN` waits at most 10 s for its `ATTACH` (`STATUS timeout`).
- A room ends when its control connection closes or misses two pings (60 s). Waiting players get `no such room`;
  linked players stay linked (their sockets are independent).
- Linked sockets: when one side closes or errors, the relay shuts the other side down after sending what it has
  buffered (at most 256 KiB per direction; reading pauses while the buffer is full).
- A client that half-closes (`shutdown` for writing) after a request still receives the answering `STATUS`.

### 2.5 GM side (`net.c`)

- New `net_host_relay(relay, secret, wake, ctx)`: returns at once; the network thread connects (IPv6 first,
  `getaddrinfo`), sends `HOST` and waits for `STATUS` (4 s). The outcome arrives as a `NET_EV_RELAY` event
  (`up`, or `end` = why not: `NET_END_UNREACHABLE`, `NET_END_ROOM_TAKEN`, `NET_END_RELAY_FULL`, `NET_END_VERSION`,
  `NET_END_LOST`). The session stops hosting when the first attempt fails. Connecting to the relay blocks the
  network thread (at most 4 s per address), which only matters while the relay is down and nobody can play anyway.
- On `WAITING`: open a new connection to the relay, send `ATTACH`, wait for `STATUS OK`, then treat it as an
  accepted connection with the address from `WAITING` as its log address. A failed attach only drops that player.
- Control connection lost (or silent for 70 s): report it with `NET_EV_RELAY` (`up = false`), retry `HOST` every
  5 s with the same room ID, `NET_EV_RELAY` (`up = true`) when back. Events are sent only when the state changes. If the relay answers "room taken" on a retry (old room
  not expired yet), keep retrying.
- New `net_join_relay(relay, secret, wake, ctx)` for v2 codes: connect to the relay, send `JOIN`, wait up to 12 s
  for `STATUS`, then continue as `net_join` does. Errors end with `NET_EV_CLOSED` and new `NetEnd` values `NET_END_NO_ROOM`, `NET_END_RELAY_FULL`,
  `NET_END_TIMEOUT` (reused), `NET_END_VERSION`.

## 3. Code structure

### New files

| File | Purpose |
|---|---|
| `src/relaymsg.c/.h` | Encode / decode of the relay messages and the room ID helper. Plain C, used by the game and the relay. |
| `relay/relay.c/.h` | The relay: `relay_start(limits, log)` runs a poll loop in a thread, `relay_stop`. POSIX only (no SDL, no libsodium), so tests run it in-process. Outside `src/` so the game does not link it. |
| `relay/main.c` | `main` of `toh-relay`: `--port N` (default 41234), logs to stdout, stops on SIGTERM / SIGINT. |
| `tests/relay_test.c` | relaymsg and relay tests with raw sockets. |
| `deploy/relay/compose.yaml` | TrueNAS custom app (see section 4). |
| `deploy/relay/README.md` | Deployment and FRITZ!Box steps. |

The room ID needs BLAKE2b, so it lives in the game (`netmsg.c`, next to the join code), not in `relaymsg.c`.

### Changes

- `netmsg.c/.h`: join code v2, `JoinCode.relay`, `joincode_room_id`.
- `net.c/.h`: section 2.5.
- `session.c`: reads `TOH_RELAY` / the compiled default in `session_init`; `:host` / `--direct` choice, relay log
  lines, `NET_EV_RELAY`, new `NetEnd` texts; port mapping only for direct hosting.
- `console.c`: `:host [--direct] [name]` and its help text.
- `Makefile`: `RELAY ?=` define, `relay` target (static `build/toh-relay`: `-static`, no other libraries),
  `relay_test` in `make test`.

## 4. Deployment on TrueNAS

- `make relay`, then `scp build/toh-relay` to `/mnt/Main/apps/toh-relay/` on the NAS.
- Custom app (Apps → Discover → Install via YAML) from `deploy/relay/compose.yaml`: image `debian:stable-slim`,
  bind mount `/mnt/Main/apps/toh-relay:/relay:ro`, command `/relay/toh-relay --port 41234`, host networking (a
  published port would hide the players' IPv6 addresses behind Docker's proxy and break the rate limit),
  `restart: unless-stopped`, runs as the unprivileged `apps` user (568).
- Updating: copy the new binary, restart the app.
- FRITZ!Box, once: Internet → Permit Access → Port Sharing → the NAS → new sharing TCP 41234, IPv4 and IPv6. The
  MyFRITZ! name of the NAS becomes the `RELAY` default.

## 5. Error handling

- Every relay failure ends in a log line from section 1; none of them crashes or blocks the main thread.
- A misbehaving client of the relay only affects its own socket.
- The relay never parses encrypted data; once linked it only copies bytes.

## 6. Testing

- **Unit (`make test`):** relaymsg round trip and rejection of short / bad messages; join code v2 round trip,
  v1 still decodes; room ID is stable for a secret and differs between secrets.
- **Integration (`net_test`):** relay thread + GM + two players on loopback: handshake and message exchange through
  the relay; `JOIN` for an unknown room → `NET_END_NO_ROOM`; the same secret hosted twice → `NET_END_ROOM_TAKEN`;
  GM that never attaches → `NET_END_TIMEOUT`; no relay → `NET_END_UNREACHABLE`; relay stopped and restarted →
  `NET_EV_RELAY` down (linked players are disconnected with it) then up, a new player can join after.
- **Fuzz (`make fuzz`):** `fuzz_netmsg` also decodes relay messages (first byte picks the parser).
- **End to end (`tests/e2e_net.sh`):** runs `toh-relay` on a free port and sets `TOH_RELAY` for the GM and the
  players; the existing direct test stays with `--direct`.

## Out of scope

- A public relay, accounts or authentication at the relay (private use only).
- Running the table on the server without the GM (headless host).
- NAT hole punching.
