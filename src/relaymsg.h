#ifndef TOH_RELAYMSG_H
#define TOH_RELAYMSG_H

/*
 * Messages between the game and the relay (toh-relay), sent in plain bytes before the encrypted
 * TOH1 protocol starts. Every message is "TOHR" | u8 type | body; integers are little endian.
 *
 *   HOST    GM -> relay (control connection)   u8 version | 16 room
 *   JOIN    player -> relay                    u8 version | 16 room
 *   ATTACH  GM -> relay (one per player)       u8 version | 16 room | u32 ticket
 *   STATUS  relay -> any                       u8 status (RelayStatus)
 *   WAITING relay -> GM (control)              u32 ticket | u8 len | len bytes player address
 *   PING    both ways on the control connection, no body
 *
 * No dependencies (not even libsodium): the relay links this file too.
 * Design: docs/superpowers/specs/2026-11-24-relay-design.md, section 2.3.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RELAY_VERSION      1
#define RELAY_DEFAULT_PORT 41234
#define RELAY_ROOM_SIZE    16
#define RELAY_ADDR_MAX     64                       /* WAITING address, incl. the terminating NUL */
#define RELAY_MSG_MAX      (10 + RELAY_ADDR_MAX - 1) /* the longest message: WAITING */

typedef enum { RM_HOST = 1, RM_JOIN, RM_ATTACH, RM_STATUS, RM_WAITING, RM_PING } RelayMsgType;

typedef enum {
    RS_OK,
    RS_ROOM_TAKEN,
    RS_NO_ROOM,
    RS_FULL,
    RS_BAD_VERSION,
    RS_TIMEOUT,
    RS_BAD_REQUEST, /* the highest value */
} RelayStatus;

typedef struct {
    uint8_t type;                 /* RelayMsgType */
    uint8_t version;              /* HOST, JOIN, ATTACH */
    uint8_t room[RELAY_ROOM_SIZE]; /* HOST, JOIN, ATTACH */
    uint32_t ticket;              /* ATTACH, WAITING */
    uint8_t status;               /* STATUS */
    char addr[RELAY_ADDR_MAX];    /* WAITING: 1..63 printable ASCII characters, no spaces */
} RelayMsg;

/* Writes m to out. Returns its length, or 0 when m is invalid. */
size_t relaymsg_encode(const RelayMsg *m, uint8_t out[RELAY_MSG_MAX]);
/* Decodes the message at the start of buf. Returns the bytes it used (> 0), 0 when buf holds only the
 * beginning of a valid message, -1 when buf cannot start a valid message. Any version is accepted. */
int relaymsg_decode(const uint8_t *buf, size_t len, RelayMsg *out);

/* Splits "host", "host:port", "[v6]", "[v6]:port" or a bare IPv6 address. The port defaults to
 * RELAY_DEFAULT_PORT. False when s is malformed or host does not fit in cap. */
bool relay_parse_addr(const char *s, char *host, size_t cap, uint16_t *port);

#endif
