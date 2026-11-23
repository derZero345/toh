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
