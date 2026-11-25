#ifndef TOH_RELAY_H
#define TOH_RELAY_H

/*
 * The relay (toh-relay): GMs and players both connect out to it, so the GM needs no open port.
 * A GM keeps a control connection (HOST) open for its room. A player sends JOIN; the relay tells
 * the GM (WAITING), the GM opens a new connection for that player (ATTACH), and the relay links
 * the two sockets and copies bytes both ways. Rooms are named by a hash of the session secret, so
 * the relay cannot read the (encrypted) game.
 * One thread with a poll loop; POSIX only (no SDL, no libsodium), so tests run it in-process.
 * Design: docs/superpowers/specs/2026-11-24-relay-design.md, section 2.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    uint16_t port;     /* 0 = any free port */
    int max_rooms;     /* 64 */
    int max_players;   /* waiting or linked players per room: 32 */
    int max_sockets;   /* 1024 */
    int conns_per_addr; /* open connections per source address (IPv4, or IPv6 /64): 96 */
    int joins_per_min; /* JOINs per source address and minute: 20 */
    int first_msg_ms;  /* a new connection's first message: 5000 */
    int attach_ms;     /* a JOIN waits this long for the GM: 10000 */
    int ping_ms;       /* PING to each GM: 20000; a GM silent for 3 * ping_ms loses its room */
} RelayLimits;

typedef struct Relay Relay;

void relay_default_limits(RelayLimits *l);
/* Listens on IPv6 + IPv4 and starts the thread. log gets one line per event (NULL: quiet).
 * Returns NULL and fills err on failure. */
Relay *relay_start(const RelayLimits *l, FILE *log, char *err, size_t errcap);
uint16_t relay_port(const Relay *r);
/* Closes every connection and stops the thread. NULL is fine. */
void relay_stop(Relay *r);

#endif
