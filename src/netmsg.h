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

#define NET_PROTOCOL_VERSION 4
#define NET_DEFAULT_PORT     41234
#define NET_MAX_FRAME        (64u << 20)            /* one encrypted frame */
#define NETMSG_MAX_BLOB      (NET_MAX_FRAME - 1024) /* SNAPSHOT / IMAGE payload */
#define NETMSG_MAX_TEXT      1024                   /* LOG text, bytes incl. the terminating NUL */
#define NETMSG_MAX_PLAYERS   64                     /* players in SESSION */
#define NETMSG_MAX_DICE      64                     /* ROLL spec, bytes incl. the terminating NUL */
#define NETMSG_MAX_REASON    31                     /* OP_REJECTED reason codes are 1..31 */
#define NETMSG_MAX_ROLLS     1001                   /* NOP_ROLL values: up to 1000 dice and the total */
#define NET_CREATE_MAX_BLOB  (1u << 20)          /* CREATE: the picture's WebP bytes */
#define NET_CREATE_MAX_DIM   512                    /* CREATE: the picture's width and height */
#define NET_HASH_SIZE        32                     /* BLAKE2b-256 of an image's WebP bytes */
#define NET_SECRET_SIZE      16
#define NET_MAX_ADDRS        8
#define NET_COLOR_MAX        0xFFFFFFu              /* colors are 0xRRGGBB */
#define NETMSG_MAX_SEL_TILES    4096 /* presence: selected tiles (longer selections are cut off) */
#define NETMSG_MAX_SEL_ENTITIES 256
#define NETMSG_MAX_PATH         4096
#define NETMSG_MAX_FOOTPRINT    64
#define NETMSG_MAX_LINES        256
#define NETMSG_MAX_WORLD        1000000000 /* world coordinates, in 1/256 of the hex size */
#define NETMSG_MAX_LABEL        10000000   /* tool length labels, in feet */
#define NET_WORLD_SCALE         256        /* world units per hex size */

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
    NM_PRESENCE,      /* C->H, H->C player (H->C: whose presence; C->H: 0, the host knows), presence */
    NM_IMAGE,         /* H->C hash, image_kind (SAVE_IMG_*), data: WebP bytes */
    NM_LOG,           /* H->C text */
    NM_CLOSE,         /* H->C reason (NET_CLOSE_*) */
    NM_CREATE,        /* C->H seq, spec (a character; no image hash), data: WebP bytes of its picture */
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
    NOP_ROLL,             /* dice: "NdM[+K|-K]" (characters 0-9 d D + -); values: none (client) or
                           * the host's result, every die then the total (at most NETMSG_MAX_ROLLS) */
    NOP_CLAIM,            /* id */
    NOP_COUNT
} NetOpType;

/* NetOp.stat_mask bits, the same values as STAT_* in ops.h. Values follow in this order. */
enum { NSTAT_SIZE = 1, NSTAT_MAXHEALTH = 2, NSTAT_SPEED = 4, NSTAT_INITIATIVE = 8, NSTAT_ALL = 15 };

/* An entity created by NOP_SPAWN. Walls carry only type, image and tile. */
typedef struct {
    uint8_t type; /* SAVE_ENT_* */
    uint8_t image[NET_HASH_SIZE];
    int32_t row, col;
    int32_t size, max_health, health, attack, speed, initiative;
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
    int32_t size, max_health, speed, initiative;
    char name[SAVEFMT_NAME_MAX]; /* waypoint or player name */
    char dice[NETMSG_MAX_DICE];
} NetOp;

typedef struct {
    uint8_t id;
    uint8_t role; /* NET_ROLE_* */
    uint32_t color;
    char name[SAVEFMT_OWNER_MAX];
} NetPlayer;

typedef struct {
    int32_t row, col;
} NetHex;

/* A point or line end in world coordinates: (pixel - grid offset) / hex size * NET_WORLD_SCALE.
 * Every screen converts them with its own zoom and pan. */
typedef struct {
    int32_t ax, ay, bx, by;
    int32_t label; /* length in feet, shown at the middle */
} NetLine;

typedef struct {
    int32_t x, y, radius; /* world units */
    int32_t label;        /* feet */
} NetCircle;

/* What a person is pointing at: shown to the others in their color (spec section 4). */
typedef struct {
    NetHex *tiles;      /* selected tiles */
    uint32_t ntiles;
    uint32_t *entities; /* selected entities (never 0) */
    uint32_t nentities;
    NetHex *path;       /* movement preview while Shift is held: path and footprint */
    uint32_t npath;
    NetHex *footprint;
    uint32_t nfootprint;
    NetLine *lines;     /* measurements and the line attack */
    uint32_t nlines;
    bool has_circle;    /* area attack */
    NetCircle circle;
    bool has_cone;      /* cone attack: circle.x/y/radius/label plus the angles in degrees */
    NetCircle cone;
    int32_t cone_start; /* 0..359, counter-clockwise from 3 o'clock */
    int32_t cone_extent; /* 0..360 */
} NetPresence;

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
    NetPresence presence;              /* PRESENCE (with player) */
    NetSpec spec;                      /* CREATE: type character, health = max_health */
} NetMsg;

/* Encodes m into a new buffer (*out, free with free()). Returns false when m breaks a rule
 * the decoder checks (it would be rejected) or when out of memory. */
bool netmsg_encode(const NetMsg *m, uint8_t **out, size_t *out_len);
/* Decodes one whole message. On failure *out is left empty (netmsg_free is still safe). */
bool netmsg_decode(const uint8_t *buf, size_t len, NetMsg *out);
void netmsg_free(NetMsg *m);

/* Deep copy of a presence; free it with netpresence_free (decoded messages: netmsg_free). */
void netpresence_copy(NetPresence *dst, const NetPresence *src);
void netpresence_free(NetPresence *p);

/* Hash that names an image on the wire: BLAKE2b-256 of its WebP bytes. */
void netmsg_image_hash(const uint8_t *webp, size_t len, uint8_t out[NET_HASH_SIZE]);

/* ---- join codes ----------------------------------------------------------------------------- */

#define JOINCODE_VERSION 1
#define JOINCODE_VERSION_RELAY 2 /* payload: version | secret; players reach the GM through the relay */
#define NET_ROOM_SIZE          16
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
    bool relay; /* version 2: secret only; port and addrs stay 0 */
} JoinCode;

/* "toh1-" + Crockford base32 in groups of 4 separated by '-'. out needs JOINCODE_MAX_LEN bytes.
 * Returns false when c is invalid. */
bool joincode_encode(const JoinCode *c, char out[JOINCODE_MAX_LEN]);
/* Accepts any case, ignores '-' after the prefix and reads O as 0 and I / L as 1. */
bool joincode_decode(const char *s, JoinCode *out);

/* The relay room of a session: BLAKE2b-128 keyed with the secret over "toh-room". Players and the GM
 * compute it; the relay only ever sees this, never the secret. */
void joincode_room_id(const uint8_t secret[NET_SECRET_SIZE], uint8_t room[NET_ROOM_SIZE]);

#endif
