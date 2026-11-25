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
 * With a relay, the same handshake runs after the relay linked the two sockets (relaymsg.h).
 * Design: docs/superpowers/specs/2026-11-21-multiplayer-design.md, section 2.
 */

#include "netmsg.h"

#define NET_MAX_CONNS          32      /* host: connections, handshakes included */
#define NET_HELLO_TIMEOUT_MS   5000    /* host: time until the first message */
#define NET_CONNECT_TIMEOUT_MS 4000    /* client: per address */
#define NET_MAX_QUEUE          (64u << 20) /* bytes waiting to be sent to one connection */
#define NET_HANDSHAKES_PER_MIN 5       /* host: per source address (IPv4, or IPv6 /64) */
#define NET_FIRST_FRAME_MAX    256     /* host: a connection's first frame (HELLO), before it proved the secret */
#define NET_RELAY_ADDR_MAX     256     /* "host:port" of the relay, incl. the terminating NUL */
#define NET_RELAY_JOIN_MS      12000   /* client: wait for the GM to attach (the relay gives up after 10 s) */
#define NET_RELAY_RETRY_MS     5000    /* host: reconnect to a lost relay */
#define NET_RELAY_DEAD_MS      70000   /* host: nothing from the relay (it pings every 20 s) */

typedef struct Net Net;

typedef enum { NET_EV_CONNECTED, NET_EV_MESSAGE, NET_EV_CLOSED, NET_EV_RELAY } NetEventType;

typedef enum {
    NET_END_NORMAL,      /* net_close, or the peer closed cleanly after we asked */
    NET_END_LOST,        /* the peer closed or the connection broke */
    NET_END_UNREACHABLE, /* client: no address in the join code answered; relay: not reachable */
    NET_END_BAD_DATA,    /* a frame did not decrypt or a message did not decode */
    NET_END_TIMEOUT,     /* host: no message within NET_HELLO_TIMEOUT_MS; relay: the GM did not attach */
    NET_END_OVERFLOW,    /* the peer does not read: more than NET_MAX_QUEUE bytes pending */
    NET_END_NO_ROOM,     /* relay: nobody hosts this join code */
    NET_END_RELAY_FULL,  /* relay: too many rooms / players, or too many joins from this address */
    NET_END_VERSION,     /* relay: speaks another protocol version */
    NET_END_ROOM_TAKEN,  /* relay: the room is hosted already */
} NetEnd;

typedef struct {
    NetEventType type;
    int conn;       /* connection id, > 0; a client's connection to the host is always 1 */
    NetMsg msg;     /* NET_EV_MESSAGE; msg.data points into frame */
    uint8_t *frame; /* owned by the event */
    NetEnd end;     /* NET_EV_CLOSED */
    bool up;        /* NET_EV_RELAY: the relay accepted the room (false: end says why not) */
    char addr[64];  /* peer address, for log lines */
} NetEvent;

typedef void (*NetWakeFn)(void *ctx);

/* Listens on port (0 = any free port) on IPv6 and IPv4. Returns NULL and fills err on failure. */
Net *net_host(uint16_t port, const uint8_t secret[NET_SECRET_SIZE], NetWakeFn wake, void *ctx, char *err, size_t errcap);
/* Connects to the addresses of code in the background (IPv6 first). The result is a
 * NET_EV_CONNECTED or NET_EV_CLOSED (NET_END_UNREACHABLE) event for connection 1. */
Net *net_join(const JoinCode *code, NetWakeFn wake, void *ctx);
/* Hosts through the relay at relay ("host[:port]"). Returns at once; the network thread connects
 * and keeps reconnecting. It reports with NET_EV_RELAY (conn 0) when the state changes, and once
 * for a first attempt that fails. Players arrive as NET_EV_CONNECTED like with net_host. */
Net *net_host_relay(const char *relay, const uint8_t secret[NET_SECRET_SIZE], NetWakeFn wake, void *ctx);
/* Joins a game hosted through the relay (join code v2). Events as with net_join; a refusal by the
 * relay ends connection 1 with NET_END_NO_ROOM / _RELAY_FULL / _TIMEOUT / _VERSION / _UNREACHABLE. */
Net *net_join_relay(const char *relay, const uint8_t secret[NET_SECRET_SIZE], NetWakeFn wake, void *ctx);
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
