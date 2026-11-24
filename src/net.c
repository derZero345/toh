/* Network thread: sockets, handshake, encryption, queues. See net.h. */
#include "net.h"
#include "relaymsg.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define GREETING_LEN   28 /* "TOH1" | 24-byte nonce */
#define HEADER_LEN     crypto_secretstream_xchacha20poly1305_HEADERBYTES
#define ABYTES         crypto_secretstream_xchacha20poly1305_ABYTES
#define MAX_CIPHERTEXT ((size_t)NET_MAX_FRAME + ABYTES)
#define CLOSE_FLUSH_MS 2000
#define STOP_FLUSH_MS  1000
#define RATE_SLOTS     64

_Static_assert(NET_ROOM_SIZE == RELAY_ROOM_SIZE, "room IDs must have the same size");

static const char GREETING_MAGIC[4] = { 'T', 'O', 'H', '1' };

typedef struct {
    uint8_t *p;
    size_t len, cap, pos; /* pos: bytes already consumed (in) or written (out) */
} Bytes;

enum { CS_GREET, CS_HEADER, CS_OPEN, CS_RELAY }; /* CS_RELAY: waiting for the relay's STATUS */
enum { CTL_DOWN, CTL_HELLO, CTL_UP };             /* host: the control connection to the relay */

typedef struct {
    int id, fd, state;
    bool announced;   /* NET_EV_CONNECTED was emitted */
    bool closing;     /* net_close / net_free: flush, then close */
    bool got_message; /* host: the HELLO deadline is over */
    uint64_t deadline;
    uint8_t nonce[24];
    uint8_t rx_key[32], tx_key[32];
    crypto_secretstream_xchacha20poly1305_state tx, rx;
    Bytes in, out;
    char addr[64];
} Conn;

/* main thread -> network thread */
typedef struct Pending {
    int conn;
    uint8_t *data; /* NULL: close request */
    size_t len;
    struct Pending *next;
} Pending;

typedef struct {
    char addr[64];
    uint64_t window_start;
    int count;
} RateSlot;

struct Net {
    bool host;
    pthread_t thread;
    pthread_mutex_t mu;
    int wake_rd, wake_wr;
    NetWakeFn wake;
    void *wake_ctx;

    /* shared, under mu */
    Pending *q_head, *q_tail;
    bool stop;
    NetEvent *ev;
    size_t ev_head, ev_tail, ev_cap;
    int open_ids[NET_MAX_CONNS];
    int nopen;

    /* network thread only */
    int lfd;
    uint16_t port;
    uint8_t secret[NET_SECRET_SIZE];
    JoinCode code;
    Conn *conns[NET_MAX_CONNS];
    int nconns, next_id;
    RateSlot rate[RATE_SLOTS];

    /* relay (network thread only, except relay / relay_addr / room set before the thread starts) */
    bool relay;
    char relay_addr[NET_RELAY_ADDR_MAX];
    uint8_t room[RELAY_ROOM_SIZE];
    struct sockaddr_storage relay_sa; /* host: the address the control connection reached */
    socklen_t relay_sl;
    int cfd, cstate;                  /* host: control connection (-1 = none), CTL_* */
    uint8_t cin[RELAY_MSG_MAX];
    size_t cin_len;
    Bytes cout;
    uint64_t cdeadline, cretry, clast_rx;
    bool relay_reported, relay_up;    /* the last NET_EV_RELAY */
};

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void set_nonblocking(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void poke(Net *n) {
    char c = 1;
    if (write(n->wake_wr, &c, 1) < 0) { /* pipe full: the thread wakes anyway */ }
}

/* ---- byte buffers ----------------------------------------------------------------------------- */

static bool bytes_append(Bytes *b, const void *src, size_t len) {
    if (b->pos > 0 && b->pos == b->len) b->pos = b->len = 0;
    if (len > b->cap - b->len) {
        if (b->pos > 0) { /* compact first */
            memmove(b->p, b->p + b->pos, b->len - b->pos);
            b->len -= b->pos;
            b->pos = 0;
        }
        if (len > b->cap - b->len) {
            size_t cap = b->cap ? b->cap : 4096;
            while (cap - b->len < len) cap *= 2;
            uint8_t *np = realloc(b->p, cap);
            if (!np) return false;
            b->p = np;
            b->cap = cap;
        }
    }
    memcpy(b->p + b->len, src, len);
    b->len += len;
    return true;
}

static size_t bytes_pending(const Bytes *b) { return b->len - b->pos; }

static bool queue_relay(Bytes *b, const RelayMsg *m) {
    uint8_t buf[RELAY_MSG_MAX];
    size_t n = relaymsg_encode(m, buf);
    return n && bytes_append(b, buf, n);
}

static NetEnd status_end(uint8_t status) {
    switch (status) {
    case RS_NO_ROOM:     return NET_END_NO_ROOM;
    case RS_FULL:        return NET_END_RELAY_FULL;
    case RS_TIMEOUT:     return NET_END_TIMEOUT;
    case RS_BAD_VERSION: return NET_END_VERSION;
    case RS_ROOM_TAKEN:  return NET_END_ROOM_TAKEN;
    default:             return NET_END_BAD_DATA;
    }
}

/* ---- events (network thread -> main thread) --------------------------------------------------- */

static void push_event(Net *n, NetEvent *ev) {
    pthread_mutex_lock(&n->mu);
    bool was_empty = n->ev_head == n->ev_tail;
    if (n->ev_tail == n->ev_cap) {
        if (n->ev_head > 0) {
            memmove(n->ev, n->ev + n->ev_head, (n->ev_tail - n->ev_head) * sizeof(*n->ev));
            n->ev_tail -= n->ev_head;
            n->ev_head = 0;
        } else {
            size_t cap = n->ev_cap ? n->ev_cap * 2 : 64;
            NetEvent *ne = realloc(n->ev, cap * sizeof(*ne));
            if (!ne) abort();
            n->ev = ne;
            n->ev_cap = cap;
        }
    }
    n->ev[n->ev_tail++] = *ev;
    pthread_mutex_unlock(&n->mu);
    if (was_empty && n->wake) n->wake(n->wake_ctx);
}

static void set_open(Net *n, int id, bool open) {
    pthread_mutex_lock(&n->mu);
    if (open && n->nopen < NET_MAX_CONNS) {
        n->open_ids[n->nopen++] = id;
    } else if (!open) {
        for (int i = 0; i < n->nopen; i++)
            if (n->open_ids[i] == id) n->open_ids[i] = n->open_ids[--n->nopen];
    }
    pthread_mutex_unlock(&n->mu);
}

bool net_next(Net *n, NetEvent *ev) {
    if (!n) return false;
    pthread_mutex_lock(&n->mu);
    bool have = n->ev_head < n->ev_tail;
    if (have) *ev = n->ev[n->ev_head++];
    if (n->ev_head == n->ev_tail) n->ev_head = n->ev_tail = 0;
    pthread_mutex_unlock(&n->mu);
    return have;
}

void net_event_free(NetEvent *ev) {
    netmsg_free(&ev->msg);
    free(ev->frame);
    memset(ev, 0, sizeof(*ev));
}

/* ---- connections ------------------------------------------------------------------------------ */

static Conn *conn_new(Net *n, int fd, const char *addr) {
    Conn *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->id = n->host ? ++n->next_id : 1;
    c->fd = fd;
    c->state = CS_GREET;
    snprintf(c->addr, sizeof(c->addr), "%s", addr);
    randombytes_buf(c->nonce, sizeof(c->nonce));
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    set_nonblocking(fd);
    n->conns[n->nconns++] = c;
    return c;
}

static Conn *conn_by_id(Net *n, int id) {
    for (int i = 0; i < n->nconns; i++)
        if (n->conns[i]->id == id) return n->conns[i];
    return NULL;
}

/* Is c still open? Compares pointers only: c may have been freed. */
static bool conn_alive(Net *n, const Conn *c) {
    for (int i = 0; i < n->nconns; i++)
        if (n->conns[i] == c) return true;
    return false;
}

/* Closes c. The main thread hears about it when it knew the connection (or, on a client,
 * always: it waits for the outcome of net_join). */
static void conn_end(Net *n, Conn *c, NetEnd end) {
    close(c->fd);
    if (c->announced || !n->host) {
        NetEvent ev = { .type = NET_EV_CLOSED, .conn = c->id, .end = end };
        snprintf(ev.addr, sizeof(ev.addr), "%s", c->addr);
        push_event(n, &ev);
    }
    if (c->announced) set_open(n, c->id, false);
    for (int i = 0; i < n->nconns; i++)
        if (n->conns[i] == c) n->conns[i] = n->conns[--n->nconns];
    sodium_memzero(c->rx_key, sizeof(c->rx_key));
    sodium_memzero(c->tx_key, sizeof(c->tx_key));
    free(c->in.p);
    free(c->out.p);
    free(c);
}

static bool queue_frame(Conn *c, const uint8_t *cipher, size_t len) {
    uint8_t hdr[4] = { (uint8_t)len, (uint8_t)(len >> 8), (uint8_t)(len >> 16), (uint8_t)(len >> 24) };
    return bytes_append(&c->out, hdr, 4) && bytes_append(&c->out, cipher, len);
}

/* Encrypts one message into c's output. False: out of memory or the peer does not read. */
static bool queue_message(Conn *c, const uint8_t *msg, size_t len) {
    if (bytes_pending(&c->out) + len + ABYTES + 4 > NET_MAX_QUEUE) return false;
    uint8_t *cipher = malloc(len + ABYTES);
    if (!cipher) return false;
    crypto_secretstream_xchacha20poly1305_push(&c->tx, cipher, NULL, msg, len, NULL, 0,
                                               crypto_secretstream_xchacha20poly1305_TAG_MESSAGE);
    bool ok = queue_frame(c, cipher, len + ABYTES);
    free(cipher);
    return ok;
}

static void queue_greeting(Conn *c) {
    uint8_t g[GREETING_LEN];
    memcpy(g, GREETING_MAGIC, 4);
    memcpy(g + 4, c->nonce, 24);
    bytes_append(&c->out, g, sizeof(g));
}

/* Derives both keys from the nonces and queues our stream header. */
static bool start_streams(Net *n, Conn *c, const uint8_t peer_nonce[24]) {
    uint8_t msg[11 + 48], k[64];
    memcpy(msg, "toh-session", 11);
    memcpy(msg + 11, n->host ? peer_nonce : c->nonce, 24);
    memcpy(msg + 35, n->host ? c->nonce : peer_nonce, 24);
    if (crypto_generichash(k, sizeof(k), msg, sizeof(msg), n->secret, NET_SECRET_SIZE) != 0) return false;
    memcpy(n->host ? c->rx_key : c->tx_key, k, 32);      /* client -> host */
    memcpy(n->host ? c->tx_key : c->rx_key, k + 32, 32); /* host -> client */
    sodium_memzero(k, sizeof(k));
    uint8_t header[HEADER_LEN];
    crypto_secretstream_xchacha20poly1305_init_push(&c->tx, header, c->tx_key);
    return queue_frame(c, header, sizeof(header));
}

/* Handles the bytes in c->in. Returns false when c was ended. */
static bool process_input(Net *n, Conn *c) {
    for (;;) {
        const uint8_t *p = c->in.p + c->in.pos;
        size_t avail = bytes_pending(&c->in);
        if (c->state == CS_RELAY) { /* the relay answers JOIN / ATTACH before the peer speaks */
            RelayMsg m;
            int used = relaymsg_decode(p, avail, &m);
            if (used == 0) return true;
            if (used < 0 || m.type != RM_STATUS) {
                conn_end(n, c, NET_END_BAD_DATA);
                return false;
            }
            if (m.status != RS_OK) {
                conn_end(n, c, status_end(m.status));
                return false;
            }
            c->in.pos += (size_t)used;
            c->state = CS_GREET;
            if (n->host) {
                c->deadline = now_ms() + NET_HELLO_TIMEOUT_MS;
            } else {
                c->deadline = 0;
                queue_greeting(c);
            }
            continue;
        }
        if (c->state == CS_GREET) {
            if (avail < GREETING_LEN) return true;
            if (memcmp(p, GREETING_MAGIC, 4) != 0) {
                conn_end(n, c, NET_END_BAD_DATA);
                return false;
            }
            if (n->host) queue_greeting(c);
            if (!start_streams(n, c, p + 4)) {
                conn_end(n, c, NET_END_BAD_DATA);
                return false;
            }
            c->in.pos += GREETING_LEN;
            c->state = CS_HEADER;
            continue;
        }
        if (avail < 4) return true;
        size_t len = (size_t)p[0] | (size_t)p[1] << 8 | (size_t)p[2] << 16 | (size_t)p[3] << 24;
        if (len > MAX_CIPHERTEXT || (c->state == CS_HEADER && len != HEADER_LEN) ||
            (c->state == CS_OPEN && len < ABYTES)) {
            conn_end(n, c, NET_END_BAD_DATA);
            return false;
        }
        if (avail < 4 + len) return true;
        const uint8_t *body = p + 4;
        c->in.pos += 4 + len;

        if (c->state == CS_HEADER) {
            if (crypto_secretstream_xchacha20poly1305_init_pull(&c->rx, body, c->rx_key) != 0) {
                conn_end(n, c, NET_END_BAD_DATA);
                return false;
            }
            c->state = CS_OPEN;
            c->announced = true;
            set_open(n, c->id, true);
            NetEvent ev = { .type = NET_EV_CONNECTED, .conn = c->id };
            snprintf(ev.addr, sizeof(ev.addr), "%s", c->addr);
            push_event(n, &ev);
            continue;
        }

        size_t mlen = len - ABYTES;
        uint8_t *plain = malloc(mlen ? mlen : 1);
        unsigned char tag = 0;
        if (!plain ||
            crypto_secretstream_xchacha20poly1305_pull(&c->rx, plain, NULL, &tag, body, len, NULL, 0) != 0 ||
            tag != crypto_secretstream_xchacha20poly1305_TAG_MESSAGE) {
            free(plain);
            conn_end(n, c, NET_END_BAD_DATA);
            return false;
        }
        NetEvent ev = { .type = NET_EV_MESSAGE, .conn = c->id, .frame = plain };
        if (!netmsg_decode(plain, mlen, &ev.msg)) {
            free(plain);
            conn_end(n, c, NET_END_BAD_DATA);
            return false;
        }
        if (!c->got_message) {
            c->got_message = true;
            if (!c->closing) c->deadline = 0;
        }
        if (c->closing) net_event_free(&ev); /* nobody wants it any more */
        else push_event(n, &ev);
    }
}

static bool do_read(Net *n, Conn *c) {
    uint8_t buf[65536];
    for (;;) {
        ssize_t r = recv(c->fd, buf, sizeof(buf), 0);
        if (r > 0) {
            if (!bytes_append(&c->in, buf, (size_t)r)) {
                conn_end(n, c, NET_END_LOST);
                return false;
            }
            if (!process_input(n, c)) return false;
            if (bytes_pending(&c->in) > MAX_CIPHERTEXT + 4) { /* cannot happen: frames are checked */
                conn_end(n, c, NET_END_BAD_DATA);
                return false;
            }
            continue;
        }
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
        if (r < 0 && errno == EINTR) continue;
        conn_end(n, c, c->closing ? NET_END_NORMAL : NET_END_LOST);
        return false;
    }
}

static bool do_write(Net *n, Conn *c) {
    while (bytes_pending(&c->out) > 0) {
        ssize_t w = send(c->fd, c->out.p + c->out.pos, bytes_pending(&c->out), MSG_NOSIGNAL);
        if (w > 0) {
            c->out.pos += (size_t)w;
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
        if (w < 0 && errno == EINTR) continue;
        conn_end(n, c, c->closing ? NET_END_NORMAL : NET_END_LOST);
        return false;
    }
    c->out.pos = c->out.len = 0;
    return true;
}

/* ---- host: accepting ----------------------------------------------------------------------------- */

static void addr_text(const struct sockaddr_storage *sa, char *out, size_t cap) {
    char ip[INET6_ADDRSTRLEN] = "?";
    if (sa->ss_family == AF_INET)
        inet_ntop(AF_INET, &((const struct sockaddr_in *)sa)->sin_addr, ip, sizeof(ip));
    else if (sa->ss_family == AF_INET6)
        inet_ntop(AF_INET6, &((const struct sockaddr_in6 *)sa)->sin6_addr, ip, sizeof(ip));
    snprintf(out, cap, "%s", ip);
}

/* At most NET_HANDSHAKES_PER_MIN new connections per address and minute. */
static bool rate_ok(Net *n, const char *addr) {
    uint64_t now = now_ms();
    RateSlot *free_slot = NULL, *oldest = &n->rate[0];
    for (int i = 0; i < RATE_SLOTS; i++) {
        RateSlot *s = &n->rate[i];
        if (s->addr[0] && strcmp(s->addr, addr) == 0) {
            if (now - s->window_start >= 60000) {
                s->window_start = now;
                s->count = 0;
            }
            return ++s->count <= NET_HANDSHAKES_PER_MIN;
        }
        if (!s->addr[0] || now - s->window_start >= 60000) free_slot = s;
        if (s->window_start < oldest->window_start) oldest = s;
    }
    RateSlot *s = free_slot ? free_slot : oldest;
    snprintf(s->addr, sizeof(s->addr), "%s", addr);
    s->window_start = now;
    s->count = 1;
    return true;
}

static void accept_all(Net *n) {
    for (;;) {
        struct sockaddr_storage sa;
        socklen_t sl = sizeof(sa);
        int fd = accept(n->lfd, (struct sockaddr *)&sa, &sl);
        if (fd < 0) return;
        char addr[64];
        addr_text(&sa, addr, sizeof(addr));
        if (n->nconns >= NET_MAX_CONNS || !rate_ok(n, addr)) {
            close(fd);
            continue;
        }
        Conn *c = conn_new(n, fd, addr);
        if (!c) {
            close(fd);
            continue;
        }
        c->deadline = now_ms() + NET_HELLO_TIMEOUT_MS;
    }
}

/* ---- client: connecting --------------------------------------------------------------------------- */

static bool stop_requested(Net *n) {
    pthread_mutex_lock(&n->mu);
    bool s = n->stop;
    pthread_mutex_unlock(&n->mu);
    return s;
}

/* Tries one address; returns the connected (non-blocking) socket or -1. */
static int try_connect_sa(Net *n, const struct sockaddr_storage *ss, socklen_t sl) {
    int fd = socket(ss->ss_family, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    set_nonblocking(fd);
    if (connect(fd, (const struct sockaddr *)ss, sl) == 0) return fd;
    if (errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    uint64_t end = now_ms() + NET_CONNECT_TIMEOUT_MS;
    for (;;) {
        uint64_t now = now_ms();
        if (now >= end || stop_requested(n)) break;
        struct pollfd pf[2] = { { .fd = fd, .events = POLLOUT }, { .fd = n->wake_rd, .events = POLLIN } };
        int r = poll(pf, 2, (int)(end - now));
        if (r < 0 && errno != EINTR) break;
        if (pf[1].revents) {
            char buf[64];
            while (read(n->wake_rd, buf, sizeof(buf)) > 0) {}
        }
        if (pf[0].revents) {
            int err = 0;
            socklen_t el = sizeof(err);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0) return fd;
            break;
        }
    }
    close(fd);
    return -1;
}

static int try_connect(Net *n, const NetAddr *a, uint16_t port) {
    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof(ss));
    socklen_t sl;
    if (a->family == 4) {
        struct sockaddr_in *s4 = (struct sockaddr_in *)&ss;
        s4->sin_family = AF_INET;
        s4->sin_port = htons(port);
        memcpy(&s4->sin_addr, a->addr, 4);
        sl = sizeof(*s4);
    } else {
        struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&ss;
        s6->sin6_family = AF_INET6;
        s6->sin6_port = htons(port);
        memcpy(&s6->sin6_addr, a->addr, 16);
        sl = sizeof(*s6);
    }
    return try_connect_sa(n, &ss, sl);
}

/* Connects to the first relay address that answers (IPv6 first). Returns the socket or -1;
 * sa / sl (may be NULL) get the address. */
static int relay_connect(Net *n, struct sockaddr_storage *sa, socklen_t *sl) {
    char host[NET_RELAY_ADDR_MAX], port[8];
    uint16_t p;
    if (!relay_parse_addr(n->relay_addr, host, sizeof(host), &p)) return -1;
    snprintf(port, sizeof(port), "%u", p);
    struct addrinfo hints = { .ai_socktype = SOCK_STREAM }, *res;
    if (getaddrinfo(host, port, &hints, &res) != 0) return -1;
    int fd = -1;
    for (int pass = 0; pass < 2 && fd < 0; pass++) {
        for (struct addrinfo *a = res; a && fd < 0 && !stop_requested(n); a = a->ai_next) {
            if (a->ai_family != (pass == 0 ? AF_INET6 : AF_INET)) continue;
            struct sockaddr_storage ss;
            memset(&ss, 0, sizeof(ss));
            memcpy(&ss, a->ai_addr, a->ai_addrlen);
            fd = try_connect_sa(n, &ss, a->ai_addrlen);
            if (fd >= 0 && sa) {
                *sa = ss;
                *sl = a->ai_addrlen;
            }
        }
    }
    freeaddrinfo(res);
    return fd;
}

static bool client_connect(Net *n) {
    if (n->relay) {
        int fd = relay_connect(n, NULL, NULL);
        if (fd < 0) return false;
        Conn *c = conn_new(n, fd, n->relay_addr);
        if (!c) {
            close(fd);
            return false;
        }
        c->state = CS_RELAY;
        c->deadline = now_ms() + NET_RELAY_JOIN_MS;
        RelayMsg m = { .type = RM_JOIN, .version = RELAY_VERSION };
        memcpy(m.room, n->room, RELAY_ROOM_SIZE);
        queue_relay(&c->out, &m);
        return true;
    }
    for (int pass = 0; pass < 2; pass++) { /* IPv6 first */
        for (int i = 0; i < n->code.naddrs; i++) {
            const NetAddr *a = &n->code.addrs[i];
            if ((pass == 0) != (a->family == 6)) continue;
            if (stop_requested(n)) return false;
            int fd = try_connect(n, a, n->code.port);
            if (fd < 0) continue;
            char addr[64];
            inet_ntop(a->family == 4 ? AF_INET : AF_INET6, a->addr, addr, sizeof(addr));
            Conn *c = conn_new(n, fd, addr);
            if (!c) {
                close(fd);
                return false;
            }
            queue_greeting(c);
            return true;
        }
    }
    return false;
}

/* ---- host: the control connection to the relay ------------------------------------------------- */

static void relay_report(Net *n, bool up, NetEnd end) {
    if (n->relay_reported && n->relay_up == up) return;
    n->relay_reported = true;
    n->relay_up = up;
    NetEvent ev = { .type = NET_EV_RELAY, .up = up, .end = end };
    snprintf(ev.addr, sizeof(ev.addr), "%.*s", (int)sizeof(ev.addr) - 1, n->relay_addr); /* may be cut */
    push_event(n, &ev);
}

static void ctl_down(Net *n, NetEnd end) {
    if (n->cfd >= 0) close(n->cfd);
    n->cfd = -1;
    n->cstate = CTL_DOWN;
    n->cin_len = 0;
    n->cout.len = n->cout.pos = 0;
    n->cretry = now_ms() + NET_RELAY_RETRY_MS;
    relay_report(n, false, end);
}

static void ctl_connect(Net *n) {
    int fd = relay_connect(n, &n->relay_sa, &n->relay_sl);
    if (fd < 0) {
        ctl_down(n, NET_END_UNREACHABLE);
        return;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    n->cfd = fd;
    n->cstate = CTL_HELLO;
    n->cdeadline = now_ms() + NET_CONNECT_TIMEOUT_MS;
    n->clast_rx = now_ms();
    RelayMsg m = { .type = RM_HOST, .version = RELAY_VERSION };
    memcpy(m.room, n->room, RELAY_ROOM_SIZE);
    queue_relay(&n->cout, &m);
}

/* A player waits at the relay: open a connection for them. A failure only loses that player. */
static void ctl_attach(Net *n, const RelayMsg *w) {
    if (n->nconns >= NET_MAX_CONNS) return; /* the relay times the player out */
    int fd = try_connect_sa(n, &n->relay_sa, n->relay_sl);
    if (fd < 0) return;
    Conn *c = conn_new(n, fd, w->addr);
    if (!c) {
        close(fd);
        return;
    }
    c->state = CS_RELAY;
    c->deadline = now_ms() + NET_CONNECT_TIMEOUT_MS;
    RelayMsg m = { .type = RM_ATTACH, .version = RELAY_VERSION, .ticket = w->ticket };
    memcpy(m.room, n->room, RELAY_ROOM_SIZE);
    queue_relay(&c->out, &m);
}

static void ctl_read(Net *n) {
    for (;;) {
        ssize_t r = recv(n->cfd, n->cin + n->cin_len, sizeof(n->cin) - n->cin_len, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (r <= 0) {
            ctl_down(n, NET_END_LOST);
            return;
        }
        n->cin_len += (size_t)r;
        n->clast_rx = now_ms();
        for (;;) {
            RelayMsg m;
            int used = relaymsg_decode(n->cin, n->cin_len, &m);
            if (used == 0 && n->cin_len < sizeof(n->cin)) break;
            if (used <= 0) {
                ctl_down(n, NET_END_BAD_DATA);
                return;
            }
            memmove(n->cin, n->cin + used, n->cin_len - (size_t)used);
            n->cin_len -= (size_t)used;
            if (n->cstate == CTL_HELLO) {
                if (m.type != RM_STATUS) {
                    ctl_down(n, NET_END_BAD_DATA);
                    return;
                }
                if (m.status != RS_OK) {
                    ctl_down(n, status_end(m.status));
                    return;
                }
                n->cstate = CTL_UP;
                n->cdeadline = 0;
                relay_report(n, true, NET_END_NORMAL);
            } else if (m.type == RM_WAITING) {
                ctl_attach(n, &m);
            } else if (m.type == RM_PING) {
                RelayMsg pong = { .type = RM_PING };
                queue_relay(&n->cout, &pong);
            } else {
                ctl_down(n, NET_END_BAD_DATA);
                return;
            }
        }
    }
}

static void ctl_write(Net *n) {
    while (bytes_pending(&n->cout) > 0) {
        ssize_t w = send(n->cfd, n->cout.p + n->cout.pos, bytes_pending(&n->cout), MSG_NOSIGNAL);
        if (w > 0) {
            n->cout.pos += (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        ctl_down(n, NET_END_LOST);
        return;
    }
    n->cout.pos = n->cout.len = 0;
}

static void ctl_timers(Net *n, uint64_t now) {
    if (n->cstate == CTL_DOWN && now >= n->cretry) ctl_connect(n);
    else if (n->cstate == CTL_HELLO && now >= n->cdeadline) ctl_down(n, NET_END_UNREACHABLE);
    else if (n->cstate == CTL_UP && now - n->clast_rx >= NET_RELAY_DEAD_MS) ctl_down(n, NET_END_LOST);
}

/* Milliseconds until ctl_timers has something to do. */
static int ctl_timeout(const Net *n, uint64_t now) {
    uint64_t at = n->cstate == CTL_DOWN    ? n->cretry
                : n->cstate == CTL_HELLO   ? n->cdeadline
                                           : n->clast_rx + NET_RELAY_DEAD_MS;
    return at > now ? (int)(at - now) : 0;
}

/* ---- the thread ------------------------------------------------------------------------------------ */

/* Moves queued messages and close requests to their connections. */
static void take_pending(Net *n, Pending *p) {
    while (p) {
        Pending *next = p->next;
        Conn *c = conn_by_id(n, p->conn);
        if (c && !c->closing) {
            if (!p->data) {
                c->closing = true;
                c->deadline = now_ms() + CLOSE_FLUSH_MS;
            } else if (c->state == CS_OPEN && !queue_message(c, p->data, p->len)) {
                conn_end(n, c, NET_END_OVERFLOW);
            }
        }
        free(p->data);
        free(p);
        p = next;
    }
}

static void *thread_main(void *arg) {
    Net *n = arg;
    if (!n->host && !client_connect(n)) {
        if (!stop_requested(n)) {
            NetEvent ev = { .type = NET_EV_CLOSED, .conn = 1, .end = NET_END_UNREACHABLE };
            push_event(n, &ev);
        }
        return NULL;
    }

    bool stopping = false;
    for (;;) {
        pthread_mutex_lock(&n->mu);
        Pending *p = n->q_head;
        n->q_head = n->q_tail = NULL;
        bool stop = n->stop;
        pthread_mutex_unlock(&n->mu);
        take_pending(n, p);

        uint64_t now = now_ms();
        if (stop && !stopping) {
            stopping = true;
            if (n->cfd >= 0) { /* the room ends with the control connection */
                close(n->cfd);
                n->cfd = -1;
            }
            for (int i = 0; i < n->nconns; i++) {
                Conn *c = n->conns[i];
                if (!c->closing || c->deadline > now + STOP_FLUSH_MS) c->deadline = now + STOP_FLUSH_MS;
                c->closing = true;
            }
        }

        /* flushed or overdue connections */
        for (int i = n->nconns - 1; i >= 0; i--) {
            Conn *c = n->conns[i];
            if (c->closing && bytes_pending(&c->out) == 0) conn_end(n, c, NET_END_NORMAL);
            else if (c->deadline && now >= c->deadline)
                conn_end(n, c, c->closing ? NET_END_NORMAL : NET_END_TIMEOUT);
        }
        if (n->relay && n->host && !stopping) ctl_timers(n, now);
        if (stopping && n->nconns == 0) break;
        if (!n->host && n->nconns == 0) { /* the client's only connection ended */
            if (stopping) break;
            /* wait for net_free */
            struct pollfd pf = { .fd = n->wake_rd, .events = POLLIN };
            poll(&pf, 1, 1000);
            char buf[64];
            while (read(n->wake_rd, buf, sizeof(buf)) > 0) {}
            if (stop_requested(n)) break;
            continue;
        }

        struct pollfd pf[3 + NET_MAX_CONNS];
        Conn *pc[3 + NET_MAX_CONNS];
        int np = 0;
        pf[np] = (struct pollfd){ .fd = n->wake_rd, .events = POLLIN };
        pc[np++] = NULL;
        int lslot = -1;
        if (n->host && !stopping && n->lfd >= 0) {
            lslot = np;
            pf[np] = (struct pollfd){ .fd = n->lfd, .events = POLLIN };
            pc[np++] = NULL;
        }
        int cslot = -1;
        if (n->cfd >= 0) {
            cslot = np;
            pf[np] = (struct pollfd){ .fd = n->cfd, .events = POLLIN | (bytes_pending(&n->cout) ? POLLOUT : 0) };
            pc[np++] = NULL;
        }
        int timeout = 1000;
        if (n->relay && n->host && !stopping) {
            int t = ctl_timeout(n, now);
            if (t < timeout) timeout = t;
        }
        for (int i = 0; i < n->nconns; i++) {
            Conn *c = n->conns[i];
            pf[np] = (struct pollfd){ .fd = c->fd, .events = POLLIN | (bytes_pending(&c->out) ? POLLOUT : 0) };
            pc[np++] = c;
            if (c->deadline) {
                int left = c->deadline > now ? (int)(c->deadline - now) : 0;
                if (left < timeout) timeout = left;
            }
        }
        if (poll(pf, (nfds_t)np, timeout) < 0 && errno != EINTR) break;

        if (pf[0].revents) {
            char buf[256];
            while (read(n->wake_rd, buf, sizeof(buf)) > 0) {}
        }
        if (lslot >= 0 && pf[lslot].revents) accept_all(n);
        if (cslot >= 0 && pf[cslot].revents) {
            if (pf[cslot].revents & (POLLIN | POLLHUP | POLLERR)) ctl_read(n);
            if (n->cfd >= 0 && (pf[cslot].revents & POLLOUT)) ctl_write(n);
        }
        for (int i = 0; i < np; i++) {
            Conn *c = pc[i];
            if (!c || !pf[i].revents || !conn_alive(n, c)) continue;
            if ((pf[i].revents & (POLLIN | POLLHUP | POLLERR)) && !do_read(n, c)) continue;
            if (pf[i].revents & POLLOUT) do_write(n, c);
        }
    }

    while (n->nconns > 0) {
        Conn *c = n->conns[n->nconns - 1];
        c->announced = false; /* nobody listens any more */
        conn_end(n, c, NET_END_NORMAL);
    }
    return NULL;
}

/* ---- public API -------------------------------------------------------------------------------------- */

static Net *net_alloc(bool host, NetWakeFn wake, void *ctx) {
    Net *n = calloc(1, sizeof(*n));
    if (!n) return NULL;
    int fds[2];
    if (pipe(fds) != 0) {
        free(n);
        return NULL;
    }
    set_nonblocking(fds[0]);
    set_nonblocking(fds[1]);
    n->wake_rd = fds[0];
    n->wake_wr = fds[1];
    n->host = host;
    n->wake = wake;
    n->wake_ctx = ctx;
    n->lfd = -1;
    n->cfd = -1;
    pthread_mutex_init(&n->mu, NULL);
    return n;
}

static void net_dealloc(Net *n) {
    if (n->lfd >= 0) close(n->lfd);
    if (n->cfd >= 0) close(n->cfd);
    free(n->cout.p);
    close(n->wake_rd);
    close(n->wake_wr);
    for (Pending *p = n->q_head; p;) {
        Pending *next = p->next;
        free(p->data);
        free(p);
        p = next;
    }
    for (size_t i = n->ev_head; i < n->ev_tail; i++) net_event_free(&n->ev[i]);
    free(n->ev);
    pthread_mutex_destroy(&n->mu);
    sodium_memzero(n->secret, sizeof(n->secret));
    sodium_memzero(n->code.secret, sizeof(n->code.secret));
    free(n);
}

static int listen_socket(uint16_t port, char *err, size_t errcap) {
    /* dual stack first, plain IPv4 when the machine has no IPv6 */
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd >= 0) {
        int zero = 0, one = 1;
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in6 sa = { .sin6_family = AF_INET6, .sin6_port = htons(port), .sin6_addr = IN6ADDR_ANY_INIT };
        if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0 && listen(fd, 16) == 0) return fd;
        close(fd);
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port) };
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0 && listen(fd, 16) == 0) return fd;
        snprintf(err, errcap, "port %u: %s", port, strerror(errno));
        close(fd);
        return -1;
    }
    snprintf(err, errcap, "%s", strerror(errno));
    return -1;
}

Net *net_host(uint16_t port, const uint8_t secret[NET_SECRET_SIZE], NetWakeFn wake, void *ctx, char *err, size_t errcap) {
    if (sodium_init() < 0) {
        snprintf(err, errcap, "libsodium failed to start");
        return NULL;
    }
    Net *n = net_alloc(true, wake, ctx);
    if (!n) {
        snprintf(err, errcap, "out of memory");
        return NULL;
    }
    memcpy(n->secret, secret, NET_SECRET_SIZE);
    n->lfd = listen_socket(port, err, errcap);
    if (n->lfd < 0) {
        net_dealloc(n);
        return NULL;
    }
    set_nonblocking(n->lfd);
    struct sockaddr_storage sa;
    socklen_t sl = sizeof(sa);
    getsockname(n->lfd, (struct sockaddr *)&sa, &sl);
    n->port = ntohs(sa.ss_family == AF_INET6 ? ((struct sockaddr_in6 *)&sa)->sin6_port
                                             : ((struct sockaddr_in *)&sa)->sin_port);
    if (pthread_create(&n->thread, NULL, thread_main, n) != 0) {
        snprintf(err, errcap, "could not start the network thread");
        net_dealloc(n);
        return NULL;
    }
    return n;
}

Net *net_join(const JoinCode *code, NetWakeFn wake, void *ctx) {
    if (sodium_init() < 0) return NULL;
    Net *n = net_alloc(false, wake, ctx);
    if (!n) return NULL;
    n->code = *code;
    memcpy(n->secret, code->secret, NET_SECRET_SIZE);
    if (pthread_create(&n->thread, NULL, thread_main, n) != 0) {
        net_dealloc(n);
        return NULL;
    }
    return n;
}

static Net *relay_net(bool host, const char *relay, const uint8_t secret[NET_SECRET_SIZE], NetWakeFn wake, void *ctx) {
    if (sodium_init() < 0 || !relay || strlen(relay) >= NET_RELAY_ADDR_MAX) return NULL;
    Net *n = net_alloc(host, wake, ctx);
    if (!n) return NULL;
    n->relay = true;
    snprintf(n->relay_addr, sizeof(n->relay_addr), "%s", relay);
    memcpy(n->secret, secret, NET_SECRET_SIZE);
    joincode_room_id(secret, n->room);
    if (pthread_create(&n->thread, NULL, thread_main, n) != 0) {
        net_dealloc(n);
        return NULL;
    }
    return n;
}

Net *net_host_relay(const char *relay, const uint8_t secret[NET_SECRET_SIZE], NetWakeFn wake, void *ctx) {
    return relay_net(true, relay, secret, wake, ctx);
}

Net *net_join_relay(const char *relay, const uint8_t secret[NET_SECRET_SIZE], NetWakeFn wake, void *ctx) {
    return relay_net(false, relay, secret, wake, ctx);
}

uint16_t net_port(const Net *n) { return n ? n->port : 0; }

static void enqueue(Net *n, int conn, uint8_t *data, size_t len) {
    Pending *p = malloc(sizeof(*p));
    if (!p) abort();
    *p = (Pending){ .conn = conn, .data = data, .len = len };
    pthread_mutex_lock(&n->mu);
    if (n->q_tail) n->q_tail->next = p;
    else n->q_head = p;
    n->q_tail = p;
    pthread_mutex_unlock(&n->mu);
    poke(n);
}

bool net_send(Net *n, int conn, const NetMsg *m) {
    if (!n) return false;
    pthread_mutex_lock(&n->mu);
    bool open = false;
    for (int i = 0; i < n->nopen; i++)
        if (n->open_ids[i] == conn) open = true;
    pthread_mutex_unlock(&n->mu);
    if (!open) return false;
    uint8_t *buf;
    size_t len;
    if (!netmsg_encode(m, &buf, &len)) return false;
    if (len > NET_MAX_FRAME) {
        free(buf);
        return false;
    }
    enqueue(n, conn, buf, len);
    return true;
}

void net_close(Net *n, int conn) {
    if (n) enqueue(n, conn, NULL, 0);
}

void net_free(Net *n) {
    if (!n) return;
    pthread_mutex_lock(&n->mu);
    n->stop = true;
    pthread_mutex_unlock(&n->mu);
    poke(n);
    pthread_join(n->thread, NULL);
    net_dealloc(n);
}
