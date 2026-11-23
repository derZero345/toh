# Multiplayer, Plan 2 of 4: Wire format (netmsg) and join codes — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Encode and decode every network message, every op and the join code, with the same strict limits on
both sides, so Plan 3 only has to move bytes over sockets.

**Architecture:** A new module `src/netmsg.c` (plain C, libsodium + `savefmt.c` for name rules, no SDL, no
`App`) defines wire structs (`NetMsg`, `NetOp`, `NetSpec`, `NetPlayer`, `JoinCode`). Ops on the wire name images by
a 32-byte BLAKE2b hash instead of `Texture *`; Plan 3's `session.c` converts `NetOp` ↔ `Op`. One shared validator
(`msg_ok`) runs before encoding and after decoding, like `savefmt.c`'s `validate`. The varint / string helpers move
from `savefmt.c` into a header-only `src/wire.h` so both formats share them.

**Tech Stack:** C11, libsodium (BLAKE2b), make, libFuzzer (clang).

**Spec:** `docs/superpowers/specs/2026-11-21-multiplayer-design.md` (sections 2.2, 2.4, 3.3, 7).

**Status:** executed 2026-11-23 on branch `multiplayer` (commits `c0a4c9a`, `7f7701f` and the docs commit). The code
blocks below are the final committed versions.

---

## Decisions (where the spec left room)

- **PRESENCE is reserved, not encoded.** Its body depends on Plan 4's drawing code (tool state uses pixel `Vec2`s
  today). `NM_PRESENCE` keeps its number; `netmsg_encode` / `netmsg_decode` refuse it until Plan 4 defines it.
- **Ops on the wire are `NetOp`, not `Op`.** `Op` borrows `Texture *` and lives in `app.h` (SDL). `NetOpType` has the
  same values as `OpType` (checked by `_Static_assert` in `tests/core_test.c`), followed by `NOP_ROLL` and `NOP_CLAIM`,
  which Plan 3 handles in the session code (they can become `OP_ROLL` / `OP_CLAIM` with the same values).
- **Message types start at 1** (`0` is invalid). Reject reasons, close reasons and `OP_REJECTED` reasons start at 1.
  `OP_REJECTED` carries an `OpResult` (1..31).
- **SNAPSHOT and IMAGE data run to the end of the message** and are borrowed from the decoded buffer (no copy of up
  to 64 MiB). The buffer must outlive the decoded `NetMsg`.
- **Spawn ids:** a client's `NOP_SPAWN` has no ids; the host's broadcast has exactly one per spec.
- **Value limits mirror `savefmt.c`:** coordinates within ±1 000 000, speed ≤ 1000, names by `savefmt_owner_valid`,
  waypoint names < 32 bytes, finite initiative values, entity ids ≠ 0, ≤ 100 000 ids per list. Counts are also
  checked against the remaining bytes so a tiny message cannot make the decoder allocate much memory.
- **Join code:** `toh1-` + Crockford base32 (groups of 4), payload as in the spec (port little endian). The decoder
  accepts any case, ignores `-`, reads `O`→`0`, `I`/`L`→`1`, and requires canonical padding (fewer than 5 zero
  bits). A code with one IPv6 and one IPv4 address is 89 characters (the spec's "about 75" was an estimate).
- **HELLO's protocol version is not checked by the decoder**, so the host can answer a mismatch with
  `REJECT(NET_REJECT_VERSION)` instead of dropping the connection.

## File structure

| File | Status | Responsibility |
|---|---|---|
| `src/wire.h` | create | `Buf` writer, `Rd` reader, varints, f64, UTF-8 strings (moved from `savefmt.c`) |
| `src/savefmt.c` | modify | uses `wire.h` |
| `src/netmsg.h`, `src/netmsg.c` | create | wire structs, `netmsg_encode/decode/free`, `netmsg_image_hash`, `joincode_encode/decode` |
| `tests/netmsg_test.c` | create | round trips, truncation, invalid values, join codes; writes fuzz seeds |
| `tests/fuzz_netmsg.c` | create | libFuzzer target: decode → encode → decode → encode must be stable |
| `tests/core_test.c` | modify | `_Static_assert`s: `NetOpType` = `OpType`, `NSTAT_*` = `STAT_*`, name sizes |
| `Makefile` | modify | `build/netmsg_test` in `make test`, `build/fuzz_netmsg` in `make fuzz` |
| `README.md` | modify | tests paragraph, source layout |

---

### Task 1: Move the buffer and reader helpers into `src/wire.h`

**Files:** Create `src/wire.h`; modify `src/savefmt.c`, `Makefile`.

- [x] **Step 1: Baseline.** Run `make test`. Expected: `97 checks, 0 failed` and `144 checks, 0 failed`.
- [x] **Step 2: Move the code.** Cut everything in `src/savefmt.c` from `/* ---- growable output buffer` up to (not
  including) `bool savefmt_owner_valid` into `src/wire.h`, mark every function `static inline`, and add
  `#include "wire.h"` after `#include "savefmt.h"` in `savefmt.c`. In `get_str` drop the `n > SAVEFMT_MAX_STR`
  test: every caller's `cap` is at most 256, so `n >= cap` already enforces it, and `netmsg.c` needs longer strings.
- [x] **Step 3: Makefile.** Add `src/wire.h` to the prerequisites of `build/save_test`.
- [x] **Step 4: Verify.** `make` (no warnings) and `make test` (same counts as step 1).
- [x] **Step 5: Commit.** `git commit -m "Move the save format's buffer and reader helpers into wire.h"`

Resulting `src/wire.h`:

```c
#ifndef TOH_WIRE_H
#define TOH_WIRE_H

/*
 * Byte buffer writer and bounds-checked reader shared by the save format (savefmt.c) and the
 * network messages (netmsg.c): little-endian integers, LEB128 / zig-zag varints, f64, UTF-8 strings.
 * Header-only (static inline), no dependencies besides libc.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---- growable output buffer --------------------------------------------------------- */

typedef struct {
    uint8_t *p;
    size_t len, cap;
    bool oom;
} Buf;

static inline void buf_reserve(Buf *b, size_t extra) {
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

static inline void put_bytes(Buf *b, const void *src, size_t n) {
    buf_reserve(b, n);
    if (b->oom) return;
    if (n) memcpy(b->p + b->len, src, n);
    b->len += n;
}

static inline void put_u8(Buf *b, uint8_t v) { put_bytes(b, &v, 1); }

static inline void put_u16(Buf *b, uint16_t v) {
    uint8_t t[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    put_bytes(b, t, 2);
}

static inline void put_u32(Buf *b, uint32_t v) {
    uint8_t t[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    put_bytes(b, t, 4);
}

/* unsigned LEB128 */
static inline void put_uv(Buf *b, uint32_t v) {
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
static inline void put_sv(Buf *b, int32_t v) { put_uv(b, ((uint32_t)v << 1) ^ (v < 0 ? 0xFFFFFFFFu : 0)); }

static inline void put_f64(Buf *b, double v) {
    uint64_t u;
    memcpy(&u, &v, 8);
    put_u32(b, (uint32_t)u);
    put_u32(b, (uint32_t)(u >> 32));
}

static inline void put_str(Buf *b, const char *s) {
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

static inline bool rd_need(Rd *r, size_t n) {
    if (r->bad || n > r->len - r->pos) {
        r->bad = true;
        return false;
    }
    return true;
}

static inline uint8_t get_u8(Rd *r) { return rd_need(r, 1) ? r->p[r->pos++] : 0; }

static inline uint16_t get_u16(Rd *r) {
    if (!rd_need(r, 2)) return 0;
    uint16_t v = (uint16_t)(r->p[r->pos] | r->p[r->pos + 1] << 8);
    r->pos += 2;
    return v;
}

static inline uint32_t get_u32(Rd *r) {
    if (!rd_need(r, 4)) return 0;
    const uint8_t *q = r->p + r->pos;
    r->pos += 4;
    return (uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24;
}

static inline uint32_t get_uv(Rd *r) {
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

static inline int32_t get_sv(Rd *r) {
    uint32_t u = get_uv(r);
    uint32_t x = (u >> 1) ^ (0u - (u & 1));
    int32_t v;
    memcpy(&v, &x, 4);
    return v;
}

static inline double get_f64(Rd *r) {
    uint64_t lo = get_u32(r), hi = get_u32(r);
    uint64_t u = lo | hi << 32;
    double v;
    memcpy(&v, &u, 8);
    return v;
}

static inline bool utf8_valid(const char *s, size_t n) {
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
static inline void get_str(Rd *r, char *out, size_t cap) {
    uint32_t n = get_uv(r);
    if (r->bad || n >= cap || !rd_need(r, n) || !utf8_valid((const char *)r->p + r->pos, n)) {
        r->bad = true;
        out[0] = '\0';
        return;
    }
    memcpy(out, r->p + r->pos, n);
    out[n] = '\0';
    r->pos += n;
}

#endif
```

### Task 2: The interface, `src/netmsg.h`

**Files:** Create `src/netmsg.h`.

- [x] **Step 1: Write the header:**

```c
#ifndef TOH_NETMSG_H
#define TOH_NETMSG_H

/*
 * Network messages and join codes: encoding, decoding and validation.
 *
 * Plain C11 on top of libsodium and savefmt.c (for player-name rules); it knows nothing about
 * SDL, sockets or the App struct. net.c (plan 3) frames and encrypts the bytes, session.c
 * converts between NetOp and the game's Op (images by hash instead of Texture pointers).
 * Design: docs/superpowers/specs/2026-11-21-multiplayer-design.md, sections 2.2 and 2.4.
 *
 * Message layout: u8 type | body. Integers are LEB128 varints (signed ones zig-zag), strings
 * are varint length + UTF-8, doubles are 8 bytes little endian, like the save state.
 * The decoder checks every length, count and value against the limits below and rejects
 * trailing bytes. It does not check ids against a table: ops_check does that.
 */

#include "savefmt.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NET_PROTOCOL_VERSION 1
#define NET_DEFAULT_PORT     41234
#define NET_MAX_FRAME        (64u << 20)            /* one encrypted frame */
#define NETMSG_MAX_BLOB      (NET_MAX_FRAME - 1024) /* SNAPSHOT / IMAGE payload */
#define NETMSG_MAX_TEXT      1024                   /* LOG text, bytes incl. the terminating NUL */
#define NETMSG_MAX_PLAYERS   64                     /* players in SESSION */
#define NETMSG_MAX_DICE      64                     /* ROLL spec, bytes incl. the terminating NUL */
#define NETMSG_MAX_REASON    31                     /* OP_REJECTED reason codes are 1..31 */
#define NET_HASH_SIZE        32                     /* BLAKE2b-256 of an image's WebP bytes */
#define NET_SECRET_SIZE      16
#define NET_MAX_ADDRS        8
#define NET_COLOR_MAX        0xFFFFFFu              /* colors are 0xRRGGBB */

typedef enum {
    NM_HELLO = 1,     /* C->H protocol, name */
    NM_WELCOME,       /* H->C player: the client's id */
    NM_REJECT,        /* H->C reason (NET_REJECT_*) */
    NM_SNAPSHOT,      /* H->C data: savefmt file bytes */
    NM_SESSION,       /* H->C entity_ids (snapshot order), players */
    NM_PLAYER_JOINED, /* H->C info */
    NM_PLAYER_LEFT,   /* H->C info */
    NM_OP,            /* C->H seq, op */
    NM_OP_APPLIED,    /* H->C player: author, op (spawn ops carry the assigned ids) */
    NM_OP_REJECTED,   /* H->C seq, reason (an OpResult, 1..NETMSG_MAX_REASON) */
    NM_PRESENCE,      /* reserved; plan 4 defines the body. Encoding and decoding fail until then. */
    NM_IMAGE,         /* H->C hash, image_kind (SAVE_IMG_*), data: WebP bytes */
    NM_LOG,           /* H->C text */
    NM_CLOSE,         /* H->C reason (NET_CLOSE_*) */
    NM_COUNT
} NetMsgType;

enum { NET_REJECT_NAME_TAKEN = 1, NET_REJECT_VERSION, NET_REJECT_FULL, NET_REJECT_BAD_NAME,
       NET_REJECT_KICKED, NET_REJECT_SNAPSHOT, NET_REJECT_MAX = NET_REJECT_SNAPSHOT };
enum { NET_CLOSE_HOST_ENDED = 1, NET_CLOSE_KICKED, NET_CLOSE_MAX = NET_CLOSE_KICKED };
enum { NET_ROLE_GM = 0, NET_ROLE_PLAYER = 1 };

/* Op types on the wire. The first ones have the same values as OpType (tests/core_test.c checks
 * it); NOP_ROLL and NOP_CLAIM are handled by the host's session code (plan 3). */
typedef enum {
    NOP_MOVE,             /* id, row, col */
    NOP_SPAWN,            /* specs; ids: none (client) or one per spec (assigned by the host) */
    NOP_DELETE,           /* ids */
    NOP_SET_STATS,        /* ids, stat_mask (STAT_* bits, 1..31), the masked values */
    NOP_SET_ICON,         /* ids, image */
    NOP_SET_BACKGROUND,   /* has_image, image */
    NOP_GRID_ORIENTATION,
    NOP_WAYPOINT_SET,     /* name, row, col */
    NOP_WAYPOINT_DELETE,  /* name */
    NOP_INIT_ADD,         /* ids, values (one finite value per id) */
    NOP_INIT_REMOVE,      /* ids */
    NOP_INIT_CLEAR,
    NOP_INIT_SHOW,
    NOP_INIT_NEXT,        /* id (0 = no current turn) */
    NOP_SET_OWNER,        /* ids, name ("" = no owner) */
    NOP_ROLL,             /* dice: "NdM[+K|-K]" (characters 0-9 d D + -) */
    NOP_CLAIM,            /* id */
    NOP_COUNT
} NetOpType;

/* NetOp.stat_mask bits, the same values as STAT_* in ops.h. Values follow in this order. */
enum { NSTAT_SIZE = 1, NSTAT_MAXHEALTH = 2, NSTAT_AC = 4, NSTAT_SPEED = 8, NSTAT_INITIATIVE = 16, NSTAT_ALL = 31 };

/* An entity created by NOP_SPAWN. Walls carry only type, image and tile. */
typedef struct {
    uint8_t type; /* SAVE_ENT_* */
    uint8_t image[NET_HASH_SIZE];
    int32_t row, col;
    int32_t size, max_health, health, attack, ac, speed, initiative;
} NetSpec;

typedef struct {
    uint8_t type; /* NOP_* */
    uint32_t id;
    int32_t row, col;
    uint32_t *ids; /* never 0 */
    uint32_t nids;
    double *values;
    uint32_t nvalues;
    NetSpec *specs;
    uint32_t nspecs;
    bool has_image;
    uint8_t image[NET_HASH_SIZE];
    uint8_t stat_mask;
    int32_t size, max_health, ac, speed, initiative;
    char name[SAVEFMT_NAME_MAX]; /* waypoint or player name */
    char dice[NETMSG_MAX_DICE];
} NetOp;

typedef struct {
    uint8_t id;
    uint8_t role; /* NET_ROLE_* */
    uint32_t color;
    char name[SAVEFMT_OWNER_MAX];
} NetPlayer;

/* One message. Only the fields of its type are used; the rest stay zero.
 * After netmsg_decode the arrays belong to the message (netmsg_free) and data points into
 * the decoded buffer, so that buffer has to outlive the message. */
typedef struct {
    uint8_t type; /* NM_* */
    uint32_t protocol;                 /* HELLO */
    char name[SAVEFMT_OWNER_MAX];      /* HELLO */
    uint8_t player;                    /* WELCOME, OP_APPLIED */
    uint8_t reason;                    /* REJECT, OP_REJECTED, CLOSE */
    uint32_t seq;                      /* OP, OP_REJECTED */
    NetOp op;                          /* OP, OP_APPLIED */
    NetPlayer info;                    /* PLAYER_JOINED, PLAYER_LEFT */
    uint32_t *entity_ids;              /* SESSION, never 0 */
    uint32_t nentity_ids;
    NetPlayer *players;                /* SESSION */
    uint32_t nplayers;
    uint8_t hash[NET_HASH_SIZE];       /* IMAGE */
    uint8_t image_kind;                /* IMAGE: SAVE_IMG_* */
    const uint8_t *data;               /* SNAPSHOT, IMAGE: 1..NETMSG_MAX_BLOB bytes */
    size_t data_len;
    char text[NETMSG_MAX_TEXT];        /* LOG */
} NetMsg;

/* Encodes m into a new buffer (*out, free with free()). Returns false when m breaks a rule
 * the decoder checks (it would be rejected) or when out of memory. */
bool netmsg_encode(const NetMsg *m, uint8_t **out, size_t *out_len);
/* Decodes one whole message. On failure *out is left empty (netmsg_free is still safe). */
bool netmsg_decode(const uint8_t *buf, size_t len, NetMsg *out);
void netmsg_free(NetMsg *m);

/* Hash that names an image on the wire: BLAKE2b-256 of its WebP bytes. */
void netmsg_image_hash(const uint8_t *webp, size_t len, uint8_t out[NET_HASH_SIZE]);

/* ---- join codes ----------------------------------------------------------------------------- */

#define JOINCODE_VERSION 1
#define JOINCODE_MAX_LEN 320 /* buffer size for joincode_encode, incl. the terminating NUL */

typedef struct {
    uint8_t family; /* 4 or 6 */
    uint8_t addr[16]; /* network byte order; IPv4 uses the first 4 bytes */
} NetAddr;

typedef struct {
    uint16_t port; /* not 0 */
    uint8_t secret[NET_SECRET_SIZE];
    uint8_t naddrs; /* 1..NET_MAX_ADDRS */
    NetAddr addrs[NET_MAX_ADDRS];
} JoinCode;

/* "toh1-" + Crockford base32 in groups of 4 separated by '-'. out needs JOINCODE_MAX_LEN bytes.
 * Returns false when c is invalid. */
bool joincode_encode(const JoinCode *c, char out[JOINCODE_MAX_LEN]);
/* Accepts any case, ignores '-' after the prefix and reads O as 0 and I / L as 1. */
bool joincode_decode(const char *s, JoinCode *out);

#endif
```

### Task 3: Failing tests and build targets

**Files:** Create `tests/netmsg_test.c`; modify `Makefile`.

- [x] **Step 1: Write the tests.** What they cover: every message type and every op type round-trips with stable
  bytes; key fields survive; every message without its last byte, with an extra byte, or cut at any length fails;
  the encoder refuses each broken rule (bad names, reasons, colors, roles, sizes, ids 0, spawn id counts, stat masks,
  speeds, NaN / infinite values, dice characters, non-UTF-8 text, PRESENCE, unknown types); hand-made byte strings
  are refused; `netmsg_image_hash` equals `crypto_generichash`; join codes round-trip, ignore case / dashes /
  look-alikes, fit the buffer at 8 addresses, and refuse wrong prefixes, foreign characters, wrong lengths,
  trailing spaces and non-zero padding.

```c
/* Unit tests for src/netmsg.c. Run with `make test`.
 * `build/netmsg_test <dir>` additionally writes sample inputs for the fuzzer into dir. */
#include "netmsg.h"

#include <math.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;

#define CHECK(cond) do {                                                       \
        checks++;                                                              \
        if (!(cond)) {                                                         \
            failures++;                                                        \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                      \
    } while (0)

/* ---- sample messages ------------------------------------------------------------------- */

static uint32_t ids3[3] = { 7, 300, 70000 };
static double values3[3] = { 0.5, -2.25, 19.0 };
static uint32_t entity_ids[4] = { 1, 2, 5, 1u << 31 };
static NetPlayer players[2] = {
    { .id = 0, .role = NET_ROLE_GM, .color = 0xE0B000, .name = "GM" },
    { .id = 3, .role = NET_ROLE_PLAYER, .color = 0x3A7BFF, .name = "Ålice" },
};
static const uint8_t blob[5] = { 'R', 'I', 'F', 'F', 0 };
static NetSpec specs2[2];

static void fill_hash(uint8_t h[NET_HASH_SIZE], uint8_t seed) {
    for (int i = 0; i < NET_HASH_SIZE; i++) h[i] = (uint8_t)(seed + i * 7);
}

/* One op of every type, with every field it uses set to a non-default value. */
static NetOp sample_op(int type) {
    NetOp o = { .type = (uint8_t)type };
    switch (type) {
    case NOP_MOVE:
        o.id = 42; o.row = -5; o.col = 1000000;
        break;
    case NOP_SPAWN:
        specs2[0] = (NetSpec){ .type = SAVE_ENT_CHARACTER, .row = 3, .col = -4, .size = 1, .max_health = 30,
                               .health = 12, .attack = 5, .ac = 15, .speed = 6, .initiative = -1 };
        fill_hash(specs2[0].image, 1);
        specs2[1] = (NetSpec){ .type = SAVE_ENT_WALL, .row = -1000000, .col = 9 };
        fill_hash(specs2[1].image, 2);
        o.specs = specs2; o.nspecs = 2;
        o.ids = ids3; o.nids = 2; /* as broadcast by the host */
        break;
    case NOP_DELETE:
    case NOP_INIT_REMOVE:
        o.ids = ids3; o.nids = 3;
        break;
    case NOP_SET_STATS:
        o.ids = ids3; o.nids = 1;
        o.stat_mask = 1 | 4 | 16; o.size = -2; o.ac = 21; o.initiative = 4;
        break;
    case NOP_SET_ICON:
        o.ids = ids3; o.nids = 2;
        o.has_image = true; fill_hash(o.image, 9);
        break;
    case NOP_SET_BACKGROUND:
        o.has_image = true; fill_hash(o.image, 11);
        break;
    case NOP_WAYPOINT_SET:
        strcpy(o.name, "camp fire"); o.row = 12; o.col = -13;
        break;
    case NOP_WAYPOINT_DELETE:
        strcpy(o.name, "camp fire");
        break;
    case NOP_INIT_ADD:
        o.ids = ids3; o.nids = 3; o.values = values3; o.nvalues = 3;
        break;
    case NOP_INIT_NEXT:
        o.id = 300;
        break;
    case NOP_SET_OWNER:
        o.ids = ids3; o.nids = 2; strcpy(o.name, "Ålice");
        break;
    case NOP_ROLL:
        strcpy(o.dice, "2d6+3");
        break;
    case NOP_CLAIM:
        o.id = 70000;
        break;
    default: /* NOP_GRID_ORIENTATION, NOP_INIT_CLEAR, NOP_INIT_SHOW: no body */
        break;
    }
    return o;
}

/* A message of type t (op messages use op type op_type). */
static NetMsg sample_msg(int t, int op_type) {
    NetMsg m = { .type = (uint8_t)t };
    switch (t) {
    case NM_HELLO: m.protocol = NET_PROTOCOL_VERSION; strcpy(m.name, "Bob"); break;
    case NM_WELCOME: m.player = 3; break;
    case NM_REJECT: m.reason = NET_REJECT_NAME_TAKEN; break;
    case NM_SNAPSHOT: m.data = blob; m.data_len = sizeof(blob); break;
    case NM_SESSION: m.entity_ids = entity_ids; m.nentity_ids = 4; m.players = players; m.nplayers = 2; break;
    case NM_PLAYER_JOINED:
    case NM_PLAYER_LEFT: m.info = players[1]; break;
    case NM_OP: m.seq = 99; m.op = sample_op(op_type); break;
    case NM_OP_APPLIED: m.player = 3; m.op = sample_op(op_type); break;
    case NM_OP_REJECTED: m.seq = 99; m.reason = 2; break;
    case NM_IMAGE: fill_hash(m.hash, 5); m.image_kind = SAVE_IMG_LOSSY; m.data = blob; m.data_len = 4; break;
    case NM_LOG: strcpy(m.text, "Alice rolled 2d6+3: 4 + 2 + 3 = 9"); break;
    case NM_CLOSE: m.reason = NET_CLOSE_KICKED; break;
    }
    return m;
}

/* ---- helpers ---------------------------------------------------------------------------- */

static bool encode(const NetMsg *m, uint8_t **buf, size_t *len) { return netmsg_encode(m, buf, len); }

/* Encodes m, decodes it and encodes the result again: both encodings must be equal. */
static bool round_trip(const NetMsg *m, NetMsg *decoded) {
    uint8_t *a = NULL, *b = NULL;
    size_t alen = 0, blen = 0;
    bool ok = encode(m, &a, &alen) && netmsg_decode(a, alen, decoded) && encode(decoded, &b, &blen) &&
              alen == blen && memcmp(a, b, alen) == 0;
    free(a);
    free(b);
    return ok;
}

/* Encodes m, lets edit change the bytes and returns whether the result still decodes. */
static bool decodes_after(const NetMsg *m, void (*edit)(uint8_t **buf, size_t *len)) {
    uint8_t *buf;
    size_t len;
    if (!encode(m, &buf, &len)) return false;
    edit(&buf, &len);
    NetMsg d;
    bool ok = netmsg_decode(buf, len, &d);
    netmsg_free(&d);
    free(buf);
    return ok;
}

static void append_zero(uint8_t **buf, size_t *len) {
    *buf = realloc(*buf, *len + 1);
    (*buf)[(*len)++] = 0;
}

static void drop_last(uint8_t **buf, size_t *len) {
    (void)buf;
    (*len)--;
}

/* ---- message tests ---------------------------------------------------------------------- */

static void test_every_message_round_trips(void) {
    for (int t = NM_HELLO; t < NM_COUNT; t++) {
        if (t == NM_PRESENCE || t == NM_OP || t == NM_OP_APPLIED) continue;
        NetMsg m = sample_msg(t, 0), d;
        CHECK(round_trip(&m, &d));
        CHECK(d.type == t);
        netmsg_free(&d);
    }
    for (int t = NM_OP; t <= NM_OP_APPLIED; t++)
        for (int o = 0; o < NOP_COUNT; o++) {
            NetMsg m = sample_msg(t, o), d;
            bool ok = round_trip(&m, &d);
            if (!ok) fprintf(stderr, "op message %d with op %d does not round-trip\n", t, o);
            CHECK(ok);
            CHECK(d.op.type == o);
            netmsg_free(&d);
        }
}

static void test_fields_survive(void) {
    NetMsg m = sample_msg(NM_HELLO, 0), d;
    CHECK(round_trip(&m, &d) && d.protocol == NET_PROTOCOL_VERSION && strcmp(d.name, "Bob") == 0);
    netmsg_free(&d);

    m = sample_msg(NM_SESSION, 0);
    CHECK(round_trip(&m, &d) && d.nentity_ids == 4 && d.entity_ids[3] == 1u << 31 && d.nplayers == 2);
    CHECK(d.players[1].id == 3 && d.players[1].role == NET_ROLE_PLAYER && d.players[1].color == 0x3A7BFF &&
          strcmp(d.players[1].name, "Ålice") == 0);
    netmsg_free(&d);

    m = sample_msg(NM_OP_APPLIED, NOP_SPAWN);
    CHECK(round_trip(&m, &d) && d.player == 3 && d.op.nspecs == 2 && d.op.nids == 2 && d.op.ids[1] == 300);
    CHECK(d.op.specs[0].type == SAVE_ENT_CHARACTER && d.op.specs[0].col == -4 && d.op.specs[0].initiative == -1 &&
          d.op.specs[0].health == 12 && memcmp(d.op.specs[0].image, specs2[0].image, NET_HASH_SIZE) == 0);
    CHECK(d.op.specs[1].type == SAVE_ENT_WALL && d.op.specs[1].row == -1000000 && d.op.specs[1].speed == 0);
    netmsg_free(&d);

    m = sample_msg(NM_OP, NOP_SET_STATS);
    CHECK(round_trip(&m, &d) && d.seq == 99 && d.op.stat_mask == 21 && d.op.size == -2 && d.op.ac == 21 &&
          d.op.initiative == 4 && d.op.speed == 0);
    netmsg_free(&d);

    m = sample_msg(NM_OP, NOP_INIT_ADD);
    CHECK(round_trip(&m, &d) && d.op.nvalues == 3 && d.op.values[1] == -2.25);
    netmsg_free(&d);

    m = sample_msg(NM_OP, NOP_MOVE);
    CHECK(round_trip(&m, &d) && d.op.id == 42 && d.op.row == -5 && d.op.col == 1000000);
    netmsg_free(&d);

    /* decoded data points into the decoded buffer, so keep it until the checks are done */
    m = sample_msg(NM_IMAGE, 0);
    uint8_t *buf;
    size_t len;
    CHECK(encode(&m, &buf, &len) && netmsg_decode(buf, len, &d));
    CHECK(d.image_kind == SAVE_IMG_LOSSY && d.data_len == 4 && d.data == buf + len - 4 &&
          memcmp(d.data, blob, 4) == 0 && memcmp(d.hash, m.hash, NET_HASH_SIZE) == 0);
    netmsg_free(&d);
    free(buf);

    m = sample_msg(NM_LOG, 0);
    CHECK(round_trip(&m, &d) && strcmp(d.text, m.text) == 0);
    netmsg_free(&d);

    m = sample_msg(NM_OP, NOP_ROLL);
    CHECK(round_trip(&m, &d) && strcmp(d.op.dice, "2d6+3") == 0);
    netmsg_free(&d);

    /* a spawn from a client carries no ids */
    m = sample_msg(NM_OP, NOP_SPAWN);
    m.op.nids = 0;
    CHECK(round_trip(&m, &d) && d.op.nids == 0 && d.op.ids == NULL);
    netmsg_free(&d);

    /* a background op without an image clears the background */
    m = sample_msg(NM_OP, NOP_SET_BACKGROUND);
    m.op.has_image = false;
    memset(m.op.image, 0, NET_HASH_SIZE);
    CHECK(round_trip(&m, &d) && !d.op.has_image);
    netmsg_free(&d);
}

static void test_truncated_and_trailing(void) {
    for (int t = NM_HELLO; t < NM_COUNT; t++) {
        if (t == NM_PRESENCE) continue;
        for (int o = 0; o < ((t == NM_OP || t == NM_OP_APPLIED) ? NOP_COUNT : 1); o++) {
            NetMsg m = sample_msg(t, o);
            /* SNAPSHOT and IMAGE data run to the end of the message, so extra bytes are data */
            if (t != NM_SNAPSHOT && t != NM_IMAGE) CHECK(!decodes_after(&m, append_zero));
            /* every message without its last byte is invalid, except a longer-than-1-byte blob */
            if (t != NM_SNAPSHOT && t != NM_IMAGE) CHECK(!decodes_after(&m, drop_last));

            uint8_t *buf;
            size_t len;
            CHECK(encode(&m, &buf, &len));
            for (size_t cut = 0; cut + 1 < len; cut++) {
                NetMsg d;
                bool ok = netmsg_decode(buf, cut, &d);
                netmsg_free(&d);
                if (ok && t != NM_SNAPSHOT && t != NM_IMAGE) {
                    fprintf(stderr, "message %d (op %d) cut to %zu bytes decodes\n", t, o, cut);
                    CHECK(!ok);
                }
            }
            free(buf);
        }
    }
}

/* Each case breaks one rule; both the encoder and the decoder must refuse it. */
static void check_invalid(NetMsg m, const char *what) {
    uint8_t *buf = NULL;
    size_t len;
    bool encoded = netmsg_encode(&m, &buf, &len);
    free(buf);
    if (encoded) fprintf(stderr, "encoder accepted: %s\n", what);
    CHECK(!encoded);
}

static void test_encoder_refuses_invalid(void) {
    NetMsg m;
    m = sample_msg(NM_HELLO, 0); strcpy(m.name, "two words"); check_invalid(m, "name with a space");
    m = sample_msg(NM_HELLO, 0); m.name[0] = '\0'; check_invalid(m, "empty name");
    m = sample_msg(NM_REJECT, 0); m.reason = 0; check_invalid(m, "reject reason 0");
    m = sample_msg(NM_REJECT, 0); m.reason = NET_REJECT_MAX + 1; check_invalid(m, "reject reason too big");
    m = sample_msg(NM_CLOSE, 0); m.reason = NET_CLOSE_MAX + 1; check_invalid(m, "close reason too big");
    m = sample_msg(NM_OP_REJECTED, 0); m.reason = NETMSG_MAX_REASON + 1; check_invalid(m, "op reason too big");
    m = sample_msg(NM_SNAPSHOT, 0); m.data_len = 0; check_invalid(m, "empty snapshot");
    m = sample_msg(NM_SNAPSHOT, 0); m.data_len = NETMSG_MAX_BLOB + 1; check_invalid(m, "snapshot too large");
    m = sample_msg(NM_IMAGE, 0); m.image_kind = 2; check_invalid(m, "unknown image kind");
    m = sample_msg(NM_PLAYER_JOINED, 0); m.info.color = 0x1000000; check_invalid(m, "color too big");
    m = sample_msg(NM_PLAYER_JOINED, 0); m.info.role = 2; check_invalid(m, "unknown role");
    m = sample_msg(NM_SESSION, 0); m.nplayers = NETMSG_MAX_PLAYERS + 1; check_invalid(m, "too many players");
    m = sample_msg(NM_SESSION, 0); entity_ids[0] = 0; check_invalid(m, "entity id 0"); entity_ids[0] = 1;
    m = sample_msg(NM_PRESENCE, 0); check_invalid(m, "presence (reserved)");
    m = sample_msg(0, 0); check_invalid(m, "type 0");
    m = sample_msg(NM_COUNT, 0); check_invalid(m, "type NM_COUNT");
    m = sample_msg(NM_OP, NOP_COUNT); check_invalid(m, "op type NOP_COUNT");
    m = sample_msg(NM_OP, NOP_MOVE); m.op.row = SAVEFMT_MAX_COORD + 1; check_invalid(m, "row out of range");
    m = sample_msg(NM_OP, NOP_MOVE); m.op.id = 0; check_invalid(m, "move id 0");
    m = sample_msg(NM_OP, NOP_CLAIM); m.op.id = 0; check_invalid(m, "claim id 0");
    m = sample_msg(NM_OP, NOP_DELETE); ids3[1] = 0; check_invalid(m, "id 0 in list"); ids3[1] = 300;
    m = sample_msg(NM_OP, NOP_DELETE); m.op.nids = SAVEFMT_MAX_ENTITIES + 1; check_invalid(m, "too many ids");
    m = sample_msg(NM_OP, NOP_SPAWN); m.op.nids = 1; check_invalid(m, "spawn ids != specs");
    m = sample_msg(NM_OP, NOP_SPAWN); specs2[0].speed = SAVEFMT_MAX_SPEED + 1; check_invalid(m, "speed too big");
    m = sample_msg(NM_OP, NOP_SPAWN); specs2[1].type = 2; check_invalid(m, "unknown entity type");
    m = sample_msg(NM_OP, NOP_SET_STATS); m.op.stat_mask = 0; check_invalid(m, "empty stat mask");
    m = sample_msg(NM_OP, NOP_SET_STATS); m.op.stat_mask = 32; check_invalid(m, "unknown stat bit");
    m = sample_msg(NM_OP, NOP_SET_STATS); m.op.stat_mask = 8; m.op.speed = SAVEFMT_MAX_SPEED + 1; check_invalid(m, "stat speed too big");
    m = sample_msg(NM_OP, NOP_SET_ICON); m.op.has_image = false; check_invalid(m, "icon without image");
    m = sample_msg(NM_OP, NOP_INIT_ADD); m.op.nvalues = 2; check_invalid(m, "values != ids");
    m = sample_msg(NM_OP, NOP_INIT_ADD); values3[2] = NAN; check_invalid(m, "NaN initiative"); values3[2] = 19.0;
    m = sample_msg(NM_OP, NOP_INIT_ADD); values3[2] = INFINITY; check_invalid(m, "infinite initiative"); values3[2] = 19.0;
    m = sample_msg(NM_OP, NOP_WAYPOINT_SET); m.op.name[0] = '\0'; check_invalid(m, "empty waypoint name");
    m = sample_msg(NM_OP, NOP_SET_OWNER); strcpy(m.op.name, "a\tb"); check_invalid(m, "owner with a tab");
    m = sample_msg(NM_OP, NOP_ROLL); strcpy(m.op.dice, ""); check_invalid(m, "empty dice spec");
    m = sample_msg(NM_OP, NOP_ROLL); strcpy(m.op.dice, "2d6; rm"); check_invalid(m, "dice spec with other characters");
    m = sample_msg(NM_LOG, 0); strcpy(m.text, "\xff\xfe"); check_invalid(m, "log text not UTF-8");
}

/* Hand-made byte strings the decoder must refuse although each length is in bounds. */
static void test_decoder_refuses_bytes(void) {
    static const struct { const char *what; uint8_t bytes[16]; size_t len; } cases[] = {
        { "empty", { 0 }, 0 },
        { "type 0", { 0 }, 1 },
        { "type NM_COUNT", { NM_COUNT }, 1 },
        { "presence", { NM_PRESENCE, 0 }, 2 },
        { "welcome without player", { NM_WELCOME }, 1 },
        { "hello name length past the end", { NM_HELLO, 1, 9, 'B', 'o', 'b' }, 6 },
        { "hello name too long", { NM_HELLO, 1, 25, 'a' }, 4 },
        { "varint longer than 32 bits", { NM_OP_REJECTED, 0xff, 0xff, 0xff, 0xff, 0x7f, 1 }, 7 },
        { "op type past the end", { NM_OP, 1, NOP_COUNT }, 3 },
        { "id count larger than the message", { NM_OP, 1, NOP_DELETE, 0x80, 0x80, 0x04, 1 }, 7 },
        { "snapshot without data", { NM_SNAPSHOT }, 1 },
        { "close reason 0", { NM_CLOSE, 0 }, 2 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        NetMsg d;
        bool ok = netmsg_decode(cases[i].bytes, cases[i].len, &d);
        if (ok) fprintf(stderr, "decoder accepted: %s\n", cases[i].what);
        CHECK(!ok);
        netmsg_free(&d);
    }
}

static void test_image_hash(void) {
    uint8_t a[NET_HASH_SIZE], b[NET_HASH_SIZE];
    netmsg_image_hash(blob, sizeof(blob), a);
    crypto_generichash(b, sizeof(b), blob, sizeof(blob), NULL, 0);
    CHECK(memcmp(a, b, NET_HASH_SIZE) == 0);
}

/* ---- join codes -------------------------------------------------------------------------- */

static JoinCode sample_code(void) {
    JoinCode c = { .port = NET_DEFAULT_PORT, .naddrs = 2 };
    for (int i = 0; i < NET_SECRET_SIZE; i++) c.secret[i] = (uint8_t)(i * 37 + 1);
    c.addrs[0] = (NetAddr){ .family = 6, .addr = { 0x20, 0x01, 0x0d, 0xb8, [15] = 1 } };
    c.addrs[1] = (NetAddr){ .family = 4, .addr = { 203, 0, 113, 7 } };
    return c;
}

static bool codes_equal(const JoinCode *a, const JoinCode *b) {
    if (a->port != b->port || a->naddrs != b->naddrs || memcmp(a->secret, b->secret, NET_SECRET_SIZE)) return false;
    for (int i = 0; i < a->naddrs; i++) {
        int n = a->addrs[i].family == 4 ? 4 : 16;
        if (a->addrs[i].family != b->addrs[i].family || memcmp(a->addrs[i].addr, b->addrs[i].addr, (size_t)n)) return false;
    }
    return true;
}

static void test_joincode_round_trip(void) {
    JoinCode c = sample_code(), d;
    char s[JOINCODE_MAX_LEN];
    CHECK(joincode_encode(&c, s));
    CHECK(strncmp(s, "toh1-", 5) == 0);
    CHECK(strlen(s) > 60 && strlen(s) < 90); /* the spec promises about 75 characters */
    CHECK(joincode_decode(s, &d) && codes_equal(&c, &d));

    /* groups of 4 separated by '-' */
    for (size_t i = 5; s[i]; i++) CHECK(((i - 5) % 5 == 4) == (s[i] == '-'));

    /* case, dashes and the look-alike letters do not matter */
    char t[JOINCODE_MAX_LEN * 2];
    size_t n = 0;
    for (size_t i = 0; s[i]; i++) {
        char ch = s[i];
        if (i >= 5 && ch == '-') continue;          /* drop the groups */
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        if (i >= 5 && ch == '0') ch = 'o';
        if (i >= 5 && ch == '1') ch = (i % 2) ? 'I' : 'l';
        t[n++] = ch;
        if (i > 5 && i % 3 == 0) t[n++] = '-';      /* other groups */
    }
    t[n] = '\0';
    t[0] = 'T';
    CHECK(joincode_decode(t, &d) && codes_equal(&c, &d));

    /* the largest code fits the buffer */
    JoinCode big = sample_code();
    big.naddrs = NET_MAX_ADDRS;
    for (int i = 0; i < NET_MAX_ADDRS; i++) big.addrs[i] = (NetAddr){ .family = 6, .addr = { 0xfd, (uint8_t)i } };
    CHECK(joincode_encode(&big, s) && strlen(s) < JOINCODE_MAX_LEN && joincode_decode(s, &d) && codes_equal(&big, &d));
}

static void test_joincode_rejects(void) {
    JoinCode c = sample_code(), d;
    char s[JOINCODE_MAX_LEN], t[JOINCODE_MAX_LEN + 8];

    JoinCode bad = c; bad.port = 0;                 CHECK(!joincode_encode(&bad, s));
    bad = c; bad.naddrs = 0;                        CHECK(!joincode_encode(&bad, s));
    bad = c; bad.naddrs = NET_MAX_ADDRS + 1;        CHECK(!joincode_encode(&bad, s));
    bad = c; bad.addrs[1].family = 5;               CHECK(!joincode_encode(&bad, s));

    CHECK(joincode_encode(&c, s));
    CHECK(!joincode_decode("", &d));
    CHECK(!joincode_decode("toh1-", &d));
    CHECK(!joincode_decode("hello", &d));
    snprintf(t, sizeof(t), "toh2%s", s + 4);        CHECK(!joincode_decode(t, &d));  /* other prefix */
    snprintf(t, sizeof(t), "%sU", s);               CHECK(!joincode_decode(t, &d));  /* not in the alphabet */
    snprintf(t, sizeof(t), "%s0", s);               CHECK(!joincode_decode(t, &d));  /* one character too many */
    snprintf(t, sizeof(t), "%s", s);
    t[strlen(t) - 1] = '\0';                        CHECK(!joincode_decode(t, &d));  /* one character missing */
    snprintf(t, sizeof(t), "%s ", s);               CHECK(!joincode_decode(t, &d));  /* trailing space */

    /* The sample's last character holds 1 data bit and 4 padding bits, which must be zero:
     * of all 32 symbols only 0 (00000) and G (10000) decode. */
    snprintf(t, sizeof(t), "%s", s);
    size_t last = strlen(t) - 1;
    const char *alphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    for (int v = 0; v < 32; v++) {
        t[last] = alphabet[v];
        CHECK(joincode_decode(t, &d) == ((v & 15) == 0));
    }
}

/* ---- fuzzer seeds ------------------------------------------------------------------------ */

/* Fuzzer input: first byte 0 = message, 1 = join code (see tests/fuzz_netmsg.c). */
static void write_seed(const char *dir, const char *name, uint8_t kind, const void *p, size_t n) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fputc(kind, f);
    fwrite(p, 1, n, f);
    fclose(f);
}

static void write_seeds(const char *dir) {
    for (int t = NM_HELLO; t < NM_COUNT; t++) {
        if (t == NM_PRESENCE) continue;
        for (int o = 0; o < ((t == NM_OP) ? NOP_COUNT : 1); o++) {
            NetMsg m = sample_msg(t, o);
            uint8_t *buf;
            size_t len;
            if (!netmsg_encode(&m, &buf, &len)) { fprintf(stderr, "cannot encode seed %d/%d\n", t, o); exit(1); }
            char name[32];
            snprintf(name, sizeof(name), "msg-%02d-%02d", t, o);
            write_seed(dir, name, 0, buf, len);
            free(buf);
        }
    }
    JoinCode c = sample_code();
    char s[JOINCODE_MAX_LEN];
    joincode_encode(&c, s);
    write_seed(dir, "joincode", 1, s, strlen(s));
}

int main(int argc, char **argv) {
    if (sodium_init() < 0) return 1;
    test_every_message_round_trips();
    test_fields_survive();
    test_truncated_and_trailing();
    test_encoder_refuses_invalid();
    test_decoder_refuses_bytes();
    test_image_hash();
    test_joincode_round_trip();
    test_joincode_rejects();
    if (argc > 1) write_seeds(argv[1]);
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
```

- [x] **Step 2: Makefile.** Add before `build/core_test`:

```make
NETMSG_SRC  := src/netmsg.c src/savefmt.c
NETMSG_HDR  := src/netmsg.h src/savefmt.h src/wire.h

build/netmsg_test: tests/netmsg_test.c $(NETMSG_SRC) $(NETMSG_HDR) | build
	$(CC) $(TEST_CFLAGS) $(CFLAGS) -o $@ tests/netmsg_test.c $(NETMSG_SRC) $(TEST_LIBS)
```

  and make `test` depend on `build/netmsg_test` and run `./build/netmsg_test` last.

- [x] **Step 3: Run it.** `make build/netmsg_test`. Expected: `No rule to make target 'src/netmsg.c'`.

### Task 4: Implement `src/netmsg.c`

**Files:** Create `src/netmsg.c`.

- [x] **Step 1: Write the implementation:**

```c
/* Network message and join code encoding, decoding and validation. See netmsg.h. */
#include "netmsg.h"
#include "wire.h"

#include <math.h>
#include <sodium.h>

/* ---- validation (shared by encoder and decoder) ------------------------------------------ */

static bool coord_ok(int32_t v) { return v >= -SAVEFMT_MAX_COORD && v <= SAVEFMT_MAX_COORD; }

static bool text_ok(const char *s, size_t cap) {
    size_t n = strnlen(s, cap);
    return n < cap && utf8_valid(s, n);
}

static bool ids_ok(const uint32_t *ids, uint32_t n) {
    if (n > SAVEFMT_MAX_ENTITIES || (n && !ids)) return false;
    for (uint32_t i = 0; i < n; i++)
        if (ids[i] == 0) return false;
    return true;
}

static bool dice_ok(const char *s) {
    size_t n = strnlen(s, NETMSG_MAX_DICE);
    if (n == 0 || n >= NETMSG_MAX_DICE) return false;
    for (size_t i = 0; i < n; i++)
        if (!strchr("0123456789dD+-", s[i])) return false;
    return true;
}

static bool spec_ok(const NetSpec *s) {
    if (s->type > SAVE_ENT_WALL || !coord_ok(s->row) || !coord_ok(s->col)) return false;
    return s->type != SAVE_ENT_CHARACTER || s->speed <= SAVEFMT_MAX_SPEED;
}

static bool op_ok(const NetOp *o) {
    if (o->type >= NOP_COUNT || !ids_ok(o->ids, o->nids)) return false;
    if (o->nvalues > SAVEFMT_MAX_ENTITIES || o->nspecs > SAVEFMT_MAX_ENTITIES) return false;
    switch (o->type) {
    case NOP_MOVE:
        return o->id != 0 && coord_ok(o->row) && coord_ok(o->col);
    case NOP_SPAWN:
        if (o->nspecs && !o->specs) return false;
        if (o->nids != 0 && o->nids != o->nspecs) return false;
        for (uint32_t i = 0; i < o->nspecs; i++)
            if (!spec_ok(&o->specs[i])) return false;
        return true;
    case NOP_SET_STATS:
        if (o->stat_mask == 0 || o->stat_mask > NSTAT_ALL) return false;
        return !(o->stat_mask & NSTAT_SPEED) || o->speed <= SAVEFMT_MAX_SPEED;
    case NOP_SET_ICON:
        return o->has_image;
    case NOP_WAYPOINT_SET:
        if (!coord_ok(o->row) || !coord_ok(o->col)) return false;
        /* fall through */
    case NOP_WAYPOINT_DELETE:
        return o->name[0] != '\0' && text_ok(o->name, sizeof(o->name));
    case NOP_INIT_ADD:
        if (o->nvalues != o->nids || (o->nvalues && !o->values)) return false;
        for (uint32_t i = 0; i < o->nvalues; i++)
            if (!isfinite(o->values[i])) return false;
        return true;
    case NOP_SET_OWNER:
        return o->name[0] == '\0' || savefmt_owner_valid(o->name);
    case NOP_ROLL:
        return dice_ok(o->dice);
    case NOP_CLAIM:
        return o->id != 0;
    default: /* DELETE, INIT_REMOVE (ids only), SET_BACKGROUND, INIT_NEXT, GRID / CLEAR / SHOW */
        return true;
    }
}

static bool player_ok(const NetPlayer *p) {
    return p->role <= NET_ROLE_PLAYER && p->color <= NET_COLOR_MAX && savefmt_owner_valid(p->name);
}

static bool blob_ok(const NetMsg *m) { return m->data && m->data_len >= 1 && m->data_len <= NETMSG_MAX_BLOB; }

static bool msg_ok(const NetMsg *m) {
    switch (m->type) {
    case NM_HELLO:         return savefmt_owner_valid(m->name);
    case NM_WELCOME:       return true;
    case NM_REJECT:        return m->reason >= 1 && m->reason <= NET_REJECT_MAX;
    case NM_CLOSE:         return m->reason >= 1 && m->reason <= NET_CLOSE_MAX;
    case NM_OP_REJECTED:   return m->reason >= 1 && m->reason <= NETMSG_MAX_REASON;
    case NM_SNAPSHOT:      return blob_ok(m);
    case NM_IMAGE:         return blob_ok(m) && m->image_kind <= SAVE_IMG_LOSSY;
    case NM_PLAYER_JOINED:
    case NM_PLAYER_LEFT:   return player_ok(&m->info);
    case NM_OP:
    case NM_OP_APPLIED:    return op_ok(&m->op);
    case NM_LOG:           return text_ok(m->text, sizeof(m->text));
    case NM_SESSION:
        if (!ids_ok(m->entity_ids, m->nentity_ids)) return false;
        if (m->nplayers < 1 || m->nplayers > NETMSG_MAX_PLAYERS || !m->players) return false;
        for (uint32_t i = 0; i < m->nplayers; i++)
            if (!player_ok(&m->players[i])) return false;
        return true;
    default:               return false; /* 0, NM_PRESENCE (plan 4), unknown */
    }
}

/* ---- encoding ----------------------------------------------------------------------------- */

static void put_ids(Buf *b, const uint32_t *ids, uint32_t n) {
    put_uv(b, n);
    for (uint32_t i = 0; i < n; i++) put_uv(b, ids[i]);
}

static void put_player(Buf *b, const NetPlayer *p) {
    put_u8(b, p->id);
    put_u8(b, p->role);
    put_uv(b, p->color);
    put_str(b, p->name);
}

static void put_op(Buf *b, const NetOp *o) {
    put_u8(b, o->type);
    switch (o->type) {
    case NOP_MOVE:
        put_uv(b, o->id);
        put_sv(b, o->row);
        put_sv(b, o->col);
        break;
    case NOP_SPAWN:
        put_uv(b, o->nspecs);
        for (uint32_t i = 0; i < o->nspecs; i++) {
            const NetSpec *s = &o->specs[i];
            put_u8(b, s->type);
            put_bytes(b, s->image, NET_HASH_SIZE);
            put_sv(b, s->row);
            put_sv(b, s->col);
            if (s->type == SAVE_ENT_CHARACTER) {
                put_sv(b, s->size);
                put_sv(b, s->max_health);
                put_sv(b, s->health);
                put_sv(b, s->attack);
                put_sv(b, s->ac);
                put_sv(b, s->speed);
                put_sv(b, s->initiative);
            }
        }
        put_ids(b, o->ids, o->nids);
        break;
    case NOP_DELETE:
    case NOP_INIT_REMOVE:
        put_ids(b, o->ids, o->nids);
        break;
    case NOP_SET_STATS: {
        put_ids(b, o->ids, o->nids);
        put_u8(b, o->stat_mask);
        const int32_t v[5] = { o->size, o->max_health, o->ac, o->speed, o->initiative };
        for (int i = 0; i < 5; i++)
            if (o->stat_mask & (1 << i)) put_sv(b, v[i]);
        break;
    }
    case NOP_SET_ICON:
        put_ids(b, o->ids, o->nids);
        put_bytes(b, o->image, NET_HASH_SIZE);
        break;
    case NOP_SET_BACKGROUND:
        put_u8(b, o->has_image);
        if (o->has_image) put_bytes(b, o->image, NET_HASH_SIZE);
        break;
    case NOP_WAYPOINT_SET:
        put_str(b, o->name);
        put_sv(b, o->row);
        put_sv(b, o->col);
        break;
    case NOP_WAYPOINT_DELETE:
        put_str(b, o->name);
        break;
    case NOP_INIT_ADD:
        put_ids(b, o->ids, o->nids);
        for (uint32_t i = 0; i < o->nvalues; i++) put_f64(b, o->values[i]);
        break;
    case NOP_INIT_NEXT:
    case NOP_CLAIM:
        put_uv(b, o->id);
        break;
    case NOP_SET_OWNER:
        put_ids(b, o->ids, o->nids);
        put_str(b, o->name);
        break;
    case NOP_ROLL:
        put_str(b, o->dice);
        break;
    default: /* GRID_ORIENTATION, INIT_CLEAR, INIT_SHOW */
        break;
    }
}

bool netmsg_encode(const NetMsg *m, uint8_t **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    if (!msg_ok(m)) return false;
    Buf b = { 0 };
    put_u8(&b, m->type);
    switch (m->type) {
    case NM_HELLO:
        put_uv(&b, m->protocol);
        put_str(&b, m->name);
        break;
    case NM_WELCOME:
        put_u8(&b, m->player);
        break;
    case NM_REJECT:
    case NM_CLOSE:
        put_u8(&b, m->reason);
        break;
    case NM_SNAPSHOT:
        put_bytes(&b, m->data, m->data_len);
        break;
    case NM_SESSION:
        put_ids(&b, m->entity_ids, m->nentity_ids);
        put_uv(&b, m->nplayers);
        for (uint32_t i = 0; i < m->nplayers; i++) put_player(&b, &m->players[i]);
        break;
    case NM_PLAYER_JOINED:
    case NM_PLAYER_LEFT:
        put_player(&b, &m->info);
        break;
    case NM_OP:
        put_uv(&b, m->seq);
        put_op(&b, &m->op);
        break;
    case NM_OP_APPLIED:
        put_u8(&b, m->player);
        put_op(&b, &m->op);
        break;
    case NM_OP_REJECTED:
        put_uv(&b, m->seq);
        put_u8(&b, m->reason);
        break;
    case NM_IMAGE:
        put_bytes(&b, m->hash, NET_HASH_SIZE);
        put_u8(&b, m->image_kind);
        put_bytes(&b, m->data, m->data_len);
        break;
    case NM_LOG:
        put_str(&b, m->text);
        break;
    }
    if (b.oom) {
        free(b.p);
        return false;
    }
    *out = b.p;
    *out_len = b.len;
    return true;
}

/* ---- decoding ----------------------------------------------------------------------------- */

/* Reads a count of items that take at least min_bytes each, so a short message cannot
 * make the decoder allocate a lot of memory. */
static uint32_t get_count(Rd *r, uint32_t max, size_t min_bytes) {
    uint32_t n = get_uv(r);
    if (r->bad) return 0;
    if (n > max || (size_t)n * min_bytes > r->len - r->pos) {
        r->bad = true;
        return 0;
    }
    return n;
}

static void get_bytes(Rd *r, void *dst, size_t n) {
    if (!rd_need(r, n)) {
        memset(dst, 0, n);
        return;
    }
    memcpy(dst, r->p + r->pos, n);
    r->pos += n;
}

/* calloc that marks the reader bad when out of memory; NULL for n == 0. */
static void *get_array(Rd *r, uint32_t n, size_t size) {
    if (r->bad || n == 0) return NULL;
    void *p = calloc(n, size);
    if (!p) r->bad = true;
    return p;
}

static uint32_t *get_ids(Rd *r, uint32_t *n_out) {
    uint32_t n = get_count(r, SAVEFMT_MAX_ENTITIES, 1);
    uint32_t *ids = get_array(r, n, sizeof(uint32_t));
    for (uint32_t i = 0; i < n && !r->bad; i++) ids[i] = get_uv(r);
    *n_out = ids ? n : 0;
    return ids;
}

static void get_player(Rd *r, NetPlayer *p) {
    p->id = get_u8(r);
    p->role = get_u8(r);
    p->color = get_uv(r);
    get_str(r, p->name, sizeof(p->name));
}

static void get_op(Rd *r, NetOp *o) {
    o->type = get_u8(r);
    switch (o->type) {
    case NOP_MOVE:
        o->id = get_uv(r);
        o->row = get_sv(r);
        o->col = get_sv(r);
        break;
    case NOP_SPAWN: {
        uint32_t n = get_count(r, SAVEFMT_MAX_ENTITIES, 1 + NET_HASH_SIZE + 2);
        o->specs = get_array(r, n, sizeof(NetSpec));
        o->nspecs = o->specs ? n : 0;
        for (uint32_t i = 0; i < o->nspecs && !r->bad; i++) {
            NetSpec *s = &o->specs[i];
            s->type = get_u8(r);
            get_bytes(r, s->image, NET_HASH_SIZE);
            s->row = get_sv(r);
            s->col = get_sv(r);
            if (s->type == SAVE_ENT_CHARACTER) {
                s->size = get_sv(r);
                s->max_health = get_sv(r);
                s->health = get_sv(r);
                s->attack = get_sv(r);
                s->ac = get_sv(r);
                s->speed = get_sv(r);
                s->initiative = get_sv(r);
            }
        }
        o->ids = get_ids(r, &o->nids);
        break;
    }
    case NOP_DELETE:
    case NOP_INIT_REMOVE:
        o->ids = get_ids(r, &o->nids);
        break;
    case NOP_SET_STATS: {
        o->ids = get_ids(r, &o->nids);
        o->stat_mask = get_u8(r);
        int32_t v[5] = { 0 };
        for (int i = 0; i < 5; i++)
            if (o->stat_mask & (1 << i)) v[i] = get_sv(r);
        o->size = v[0];
        o->max_health = v[1];
        o->ac = v[2];
        o->speed = v[3];
        o->initiative = v[4];
        break;
    }
    case NOP_SET_ICON:
        o->ids = get_ids(r, &o->nids);
        o->has_image = true;
        get_bytes(r, o->image, NET_HASH_SIZE);
        break;
    case NOP_SET_BACKGROUND: {
        uint8_t has = get_u8(r);
        if (has > 1) r->bad = true;
        o->has_image = has == 1;
        if (o->has_image) get_bytes(r, o->image, NET_HASH_SIZE);
        break;
    }
    case NOP_WAYPOINT_SET:
        get_str(r, o->name, sizeof(o->name));
        o->row = get_sv(r);
        o->col = get_sv(r);
        break;
    case NOP_WAYPOINT_DELETE:
        get_str(r, o->name, sizeof(o->name));
        break;
    case NOP_INIT_ADD:
        o->ids = get_ids(r, &o->nids);
        if (!r->bad && (size_t)o->nids * 8 > r->len - r->pos) r->bad = true;
        o->values = get_array(r, o->nids, sizeof(double));
        o->nvalues = o->values ? o->nids : 0;
        for (uint32_t i = 0; i < o->nvalues && !r->bad; i++) o->values[i] = get_f64(r);
        break;
    case NOP_INIT_NEXT:
    case NOP_CLAIM:
        o->id = get_uv(r);
        break;
    case NOP_SET_OWNER:
        o->ids = get_ids(r, &o->nids);
        get_str(r, o->name, sizeof(o->name));
        break;
    case NOP_ROLL:
        get_str(r, o->dice, sizeof(o->dice));
        break;
    case NOP_GRID_ORIENTATION:
    case NOP_INIT_CLEAR:
    case NOP_INIT_SHOW:
        break;
    default:
        r->bad = true;
        break;
    }
}

/* SNAPSHOT and IMAGE data: the rest of the message. */
static void get_blob(Rd *r, NetMsg *m) {
    if (r->bad) return;
    m->data = r->p + r->pos;
    m->data_len = r->len - r->pos;
    r->pos = r->len;
}

bool netmsg_decode(const uint8_t *buf, size_t len, NetMsg *out) {
    memset(out, 0, sizeof(*out));
    Rd r = { .p = buf, .len = len };
    out->type = get_u8(&r);
    switch (out->type) {
    case NM_HELLO:
        out->protocol = get_uv(&r);
        get_str(&r, out->name, sizeof(out->name));
        break;
    case NM_WELCOME:
        out->player = get_u8(&r);
        break;
    case NM_REJECT:
    case NM_CLOSE:
        out->reason = get_u8(&r);
        break;
    case NM_SNAPSHOT:
        get_blob(&r, out);
        break;
    case NM_SESSION: {
        out->entity_ids = get_ids(&r, &out->nentity_ids);
        uint32_t n = get_count(&r, NETMSG_MAX_PLAYERS, 4);
        out->players = get_array(&r, n, sizeof(NetPlayer));
        out->nplayers = out->players ? n : 0;
        for (uint32_t i = 0; i < out->nplayers && !r.bad; i++) get_player(&r, &out->players[i]);
        break;
    }
    case NM_PLAYER_JOINED:
    case NM_PLAYER_LEFT:
        get_player(&r, &out->info);
        break;
    case NM_OP:
        out->seq = get_uv(&r);
        get_op(&r, &out->op);
        break;
    case NM_OP_APPLIED:
        out->player = get_u8(&r);
        get_op(&r, &out->op);
        break;
    case NM_OP_REJECTED:
        out->seq = get_uv(&r);
        out->reason = get_u8(&r);
        break;
    case NM_IMAGE:
        get_bytes(&r, out->hash, NET_HASH_SIZE);
        out->image_kind = get_u8(&r);
        get_blob(&r, out);
        break;
    case NM_LOG:
        get_str(&r, out->text, sizeof(out->text));
        break;
    default:
        r.bad = true;
        break;
    }
    if (r.bad || r.pos != r.len || !msg_ok(out)) {
        netmsg_free(out);
        return false;
    }
    return true;
}

void netmsg_free(NetMsg *m) {
    free(m->op.ids);
    free(m->op.values);
    free(m->op.specs);
    free(m->entity_ids);
    free(m->players);
    memset(m, 0, sizeof(*m));
}

void netmsg_image_hash(const uint8_t *webp, size_t len, uint8_t out[NET_HASH_SIZE]) {
    crypto_generichash(out, NET_HASH_SIZE, webp, len, NULL, 0);
}

/* ---- join codes ----------------------------------------------------------------------------- */

static const char B32[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"; /* Crockford */
static const char JOIN_PREFIX[] = "toh1";

#define JOIN_MAX_PAYLOAD (1 + 2 + NET_SECRET_SIZE + 1 + NET_MAX_ADDRS * 17)

static int b32_value(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c == 'O') c = '0';
    if (c == 'I' || c == 'L') c = '1';
    const char *p = c ? strchr(B32, c) : NULL;
    return p ? (int)(p - B32) : -1;
}

static bool code_ok(const JoinCode *c) {
    if (c->port == 0 || c->naddrs < 1 || c->naddrs > NET_MAX_ADDRS) return false;
    for (int i = 0; i < c->naddrs; i++)
        if (c->addrs[i].family != 4 && c->addrs[i].family != 6) return false;
    return true;
}

bool joincode_encode(const JoinCode *c, char out[JOINCODE_MAX_LEN]) {
    out[0] = '\0';
    if (!code_ok(c)) return false;
    uint8_t p[JOIN_MAX_PAYLOAD];
    size_t n = 0;
    p[n++] = JOINCODE_VERSION;
    p[n++] = (uint8_t)c->port;
    p[n++] = (uint8_t)(c->port >> 8);
    memcpy(p + n, c->secret, NET_SECRET_SIZE);
    n += NET_SECRET_SIZE;
    p[n++] = c->naddrs;
    for (int i = 0; i < c->naddrs; i++) {
        size_t alen = c->addrs[i].family == 4 ? 4 : 16;
        p[n++] = c->addrs[i].family;
        memcpy(p + n, c->addrs[i].addr, alen);
        n += alen;
    }

    size_t o = 0;
    memcpy(out, JOIN_PREFIX, 4);
    o = 4;
    uint32_t acc = 0;
    int bits = 0, chars = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i < n) {
            acc = (acc << 8) | p[i];
            bits += 8;
        } else if (bits > 0) { /* pad the last group of bits with zeros */
            acc <<= 5 - bits;
            bits = 5;
        }
        while (bits >= 5) {
            if (chars++ % 4 == 0) out[o++] = '-'; /* "toh1-" and then groups of 4 */
            out[o++] = B32[(acc >> (bits - 5)) & 31];
            bits -= 5;
        }
    }
    out[o] = '\0';
    return true;
}

bool joincode_decode(const char *s, JoinCode *out) {
    memset(out, 0, sizeof(*out));
    if (!s || strnlen(s, 2 * JOINCODE_MAX_LEN) >= 2 * JOINCODE_MAX_LEN) return false;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != JOIN_PREFIX[i]) return false;
    }

    uint8_t p[JOIN_MAX_PAYLOAD];
    size_t n = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (const char *q = s + 4; *q; q++) {
        if (*q == '-') continue;
        int v = b32_value(*q);
        if (v < 0) return false;
        acc = (acc << 5) | (uint32_t)v;
        bits += 5;
        if (bits >= 8) {
            if (n == sizeof(p)) return false;
            p[n++] = (uint8_t)(acc >> (bits - 8));
            bits -= 8;
        }
    }
    /* canonical: fewer than 5 padding bits, all zero */
    if (bits >= 5 || (acc & ((1u << bits) - 1)) != 0) return false;

    Rd r = { .p = p, .len = n };
    if (get_u8(&r) != JOINCODE_VERSION) return false;
    out->port = get_u16(&r);
    get_bytes(&r, out->secret, NET_SECRET_SIZE);
    out->naddrs = get_u8(&r);
    if (r.bad || out->naddrs < 1 || out->naddrs > NET_MAX_ADDRS) return false;
    for (int i = 0; i < out->naddrs; i++) {
        out->addrs[i].family = get_u8(&r);
        if (out->addrs[i].family != 4 && out->addrs[i].family != 6) return false;
        get_bytes(&r, out->addrs[i].addr, out->addrs[i].family == 4 ? 4 : 16);
    }
    if (r.bad || r.pos != r.len || !code_ok(out)) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    return true;
}
```

- [x] **Step 2: Run the tests.** `make build/netmsg_test && ./build/netmsg_test`. Expected: `421 checks, 0 failed`.
- [x] **Step 3: Check the numbering against the game.** In `tests/core_test.c` include `netmsg.h` and add after the
  includes:

```c
/* The wire format (netmsg.h) numbers ops and stats like ops.h, so session.c can convert by value. */
_Static_assert((int)NOP_MOVE == (int)OP_MOVE && (int)NOP_SPAWN == (int)OP_SPAWN && (int)NOP_DELETE == (int)OP_DELETE &&
               (int)NOP_SET_STATS == (int)OP_SET_STATS && (int)NOP_SET_ICON == (int)OP_SET_ICON &&
               (int)NOP_SET_BACKGROUND == (int)OP_SET_BACKGROUND && (int)NOP_GRID_ORIENTATION == (int)OP_GRID_ORIENTATION &&
               (int)NOP_WAYPOINT_SET == (int)OP_WAYPOINT_SET && (int)NOP_WAYPOINT_DELETE == (int)OP_WAYPOINT_DELETE &&
               (int)NOP_INIT_ADD == (int)OP_INIT_ADD && (int)NOP_INIT_REMOVE == (int)OP_INIT_REMOVE &&
               (int)NOP_INIT_CLEAR == (int)OP_INIT_CLEAR && (int)NOP_INIT_SHOW == (int)OP_INIT_SHOW &&
               (int)NOP_INIT_NEXT == (int)OP_INIT_NEXT && (int)NOP_SET_OWNER == (int)OP_SET_OWNER && (int)NOP_ROLL == (int)OP_COUNT,
               "NetOpType must match OpType");
_Static_assert((int)NSTAT_SIZE == (int)STAT_SIZE && (int)NSTAT_MAXHEALTH == (int)STAT_MAXHEALTH && (int)NSTAT_AC == (int)STAT_AC &&
               (int)NSTAT_SPEED == (int)STAT_SPEED && (int)NSTAT_INITIATIVE == (int)STAT_INITIATIVE, "stat bits must match");
_Static_assert((int)OPR_INVALID <= (int)NETMSG_MAX_REASON, "OpResult must fit OP_REJECTED");
_Static_assert(PLAYER_NAME_MAX == SAVEFMT_OWNER_MAX && WAYPOINT_NAME_MAX == SAVEFMT_NAME_MAX, "name sizes");
```

  (The `(int)` casts avoid `-Wenum-compare`.) Run `make` and `make test`: no warnings, `0 failed` three times.

### Task 5: Fuzz target

**Files:** Create `tests/fuzz_netmsg.c`; modify `Makefile`.

- [x] **Step 1: Write the target:**

```c
/* libFuzzer target for the network message decoder and the join code parser.
 * The first input byte picks the parser: even = netmsg_decode, odd = joincode_decode.
 * Seeds come from `build/netmsg_test <dir>`. */
#include "netmsg.h"

#include <stdlib.h>
#include <string.h>

static void fuzz_message(const uint8_t *data, size_t size) {
    NetMsg m;
    if (netmsg_decode(data, size, &m)) {
        /* everything the decoder accepts must encode again, and that encoding must be stable */
        uint8_t *a, *b;
        size_t alen, blen;
        if (!netmsg_encode(&m, &a, &alen)) abort();
        NetMsg m2;
        if (!netmsg_decode(a, alen, &m2)) abort();
        if (!netmsg_encode(&m2, &b, &blen)) abort();
        if (alen != blen || memcmp(a, b, alen) != 0) abort();
        free(a);
        free(b);
        netmsg_free(&m2);
    }
    netmsg_free(&m);
}

static void fuzz_joincode(const uint8_t *data, size_t size) {
    char *s = malloc(size + 1);
    if (!s) return;
    memcpy(s, data, size);
    s[size] = '\0';
    JoinCode c, d;
    if (joincode_decode(s, &c)) {
        char out[JOINCODE_MAX_LEN];
        if (!joincode_encode(&c, out) || !joincode_decode(out, &d)) abort();
        char again[JOINCODE_MAX_LEN];
        if (!joincode_encode(&d, again) || strcmp(out, again) != 0) abort();
    }
    free(s);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0) return 0;
    if (data[0] & 1) fuzz_joincode(data + 1, size - 1);
    else fuzz_message(data + 1, size - 1);
    return 0;
}
```

- [x] **Step 2: Makefile.** Add `build/fuzz_netmsg` (same flags as `build/fuzz_savefmt`, sources `$(NETMSG_SRC)`)
  and make `fuzz` seed `build/fuzz-corpus-net` with `./build/netmsg_test build/fuzz-corpus-net` and run both
  fuzzers for `FUZZ_TIME` seconds each.
- [x] **Step 3: Run.** `make fuzz FUZZ_TIME=60`. Expected: two `Done … runs in 61 second(s)` lines, no crash.
- [x] **Step 4: Commit** Tasks 2–5: `git commit -m "Add netmsg: network message, op and join code encoding"`

### Task 6: Docs and final verification

- [x] **Step 1: README.** Tests paragraph: `make test` also runs the network message tests, `make fuzz` fuzzes both
  decoders. Source layout: add rows for `src/netmsg.c` and `src/wire.h`.
- [x] **Step 2: Sanitizers.** `make test CFLAGS="-O1 -g -fsanitize=address,undefined"` (delete `build/*_test` first so
  they are rebuilt). Expected: `0 failed` three times, no sanitizer report.
- [x] **Step 3: Clean build and e2e.** `make clean && make && make test && make e2e`. Expected: no warnings,
  `e2e save/load: OK (…)`.
- [x] **Step 4: Commit** the README and this plan.

---

## Notes for Plan 3

- **Converting ops:** `session.c` maps `Op` → `NetOp` by value (same numbering) and replaces each `Texture *` by the
  hash of its WebP bytes (`netmsg_image_hash`), sending `NM_IMAGE` first when the peer has not seen the hash. The
  WebP bytes come from encoding the texture with the savefmt rules (lossless icons / walls, quality 90 backgrounds);
  cache them per texture so a texture is encoded once per session.
- **Reasons:** `OP_REJECTED.reason` is the `OpResult` value. Plan 3 adds `OPR_ALREADY_OWNED` ("That character already
  has an owner"); the `_Static_assert` keeps every `OpResult` ≤ `NETMSG_MAX_REASON`.
- **`NOP_ROLL` / `NOP_CLAIM`** are wire-only today. Either append `OP_ROLL`, `OP_CLAIM` to `OpType` (then extend the
  static assert) or handle them in `session.c` before `ops_check`.
- **Borrowed data:** a decoded `SNAPSHOT` / `IMAGE` points into the frame buffer. Keep the frame alive until the main
  thread has consumed the message (queue frame + message together).
- **Join code size:** one IPv6 + one IPv4 address gives 89 characters; the clipboard copy matters more than length.
