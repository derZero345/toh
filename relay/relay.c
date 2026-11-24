/* The relay: see relay.h. */
#include "relay.h"
#include "relaymsg.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define LINK_BUF_MAX   (256u << 10) /* bytes buffered towards one side of a link */
#define CLOSE_FLUSH_MS 2000         /* a closing socket gets this long to send what it has */
#define RATE_SLOTS     256
#define ROOMS_PER_ADDR 2            /* rooms one source address may control */
#define ACCEPT_PAUSE_MS 100         /* out of file descriptors: stop accepting this long */

enum { SK_NEW, SK_CONTROL, SK_WAITING, SK_LINKED, SK_CLOSING };

typedef struct Sock {
    int fd, kind;
    int room;          /* SK_CONTROL, SK_WAITING, linked player: room index; -1 = none */
    bool player;       /* SK_LINKED: the player's side (counts towards its room) */
    bool dead;         /* closed; freed at the start of the next loop iteration */
    bool rd_eof;       /* SK_CLOSING: the client sent everything; only flushing is left */
    uint32_t ticket;   /* SK_WAITING */
    uint64_t deadline; /* SK_NEW: first message; SK_WAITING: attach; SK_CLOSING: flush */
    uint64_t last_rx, next_ping; /* SK_CONTROL */
    struct Sock *peer; /* SK_LINKED */
    uint8_t in[RELAY_MSG_MAX];
    size_t in_len;
    uint8_t *out;
    size_t out_len, out_pos, out_cap;
    char addr[RELAY_ADDR_MAX]; /* for logs and WAITING */
    char key[RELAY_ADDR_MAX];  /* for limits: the IPv4 address, or the IPv6 /64 prefix */
} Sock;

typedef struct {
    bool used;
    uint8_t id[RELAY_ROOM_SIZE];
    Sock *control;
    int players;
} Room;

typedef struct {
    char addr[RELAY_ADDR_MAX];
    uint64_t start;
    int count;
} Rate;

struct Relay {
    RelayLimits lim;
    FILE *log;
    int lfd, wake_rd, wake_wr;
    uint16_t port;
    pthread_t thread;
    Sock **socks;
    int nsocks, cap;
    Room *rooms;
    int nrooms;
    Rate rate[RATE_SLOTS];
    uint32_t next_ticket;
    uint64_t accept_pause_until;
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

static void rlog(Relay *r, const char *fmt, ...) {
    if (!r->log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(r->log, fmt, ap);
    va_end(ap);
    fputc('\n', r->log);
    fflush(r->log);
}

/* ---- sockets ------------------------------------------------------------------------------- */

static size_t out_pending(const Sock *s) { return s->out_len - s->out_pos; }

static bool out_append(Sock *s, const void *p, size_t len) {
    if (s->out_pos > 0 && s->out_pos == s->out_len) s->out_pos = s->out_len = 0;
    if (len > s->out_cap - s->out_len) {
        if (s->out_pos > 0) { /* compact first */
            memmove(s->out, s->out + s->out_pos, s->out_len - s->out_pos);
            s->out_len -= s->out_pos;
            s->out_pos = 0;
        }
        if (len > s->out_cap - s->out_len) {
            size_t cap = s->out_cap ? s->out_cap : 4096;
            while (cap - s->out_len < len) cap *= 2;
            uint8_t *np = realloc(s->out, cap);
            if (!np) return false;
            s->out = np;
            s->out_cap = cap;
        }
    }
    memcpy(s->out + s->out_len, p, len);
    s->out_len += len;
    return true;
}

static bool out_msg(Sock *s, const RelayMsg *m) {
    uint8_t buf[RELAY_MSG_MAX];
    size_t n = relaymsg_encode(m, buf);
    return n && out_append(s, buf, n);
}

static Sock *sock_new(Relay *r, int fd, const char *addr, const char *key) {
    if (r->nsocks == r->cap) {
        int cap = r->cap ? r->cap * 2 : 64;
        Sock **ns = realloc(r->socks, (size_t)cap * sizeof(*ns));
        if (!ns) return NULL;
        r->socks = ns;
        r->cap = cap;
    }
    Sock *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->fd = fd;
    s->kind = SK_NEW;
    s->room = -1;
    s->deadline = now_ms() + (uint64_t)r->lim.first_msg_ms;
    snprintf(s->addr, sizeof(s->addr), "%s", addr);
    snprintf(s->key, sizeof(s->key), "%s", key);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one)); /* links whose ends vanished get closed */
    set_nonblocking(fd);
    r->socks[r->nsocks++] = s;
    return s;
}

static void room_end(Relay *r, int idx);

/* Gives back what s holds in its room: a player slot, or the whole room for a control connection. */
static void release(Relay *r, Sock *s) {
    int idx = s->room;
    s->room = -1;
    if (idx < 0 || !r->rooms[idx].used) return;
    if (s->kind == SK_CONTROL) room_end(r, idx);
    else if (s->kind == SK_WAITING || (s->kind == SK_LINKED && s->player)) r->rooms[idx].players--;
}

/* Closes s now. Its link partner sends what it still has buffered, then closes too. */
static void sock_close(Relay *r, Sock *s) {
    if (s->dead) return;
    release(r, s);
    s->dead = true;
    close(s->fd);
    Sock *p = s->peer;
    s->peer = NULL;
    if (p) {
        p->peer = NULL;
        if (!p->dead) {
            release(r, p);
            p->kind = SK_CLOSING;
            p->deadline = now_ms() + CLOSE_FLUSH_MS;
        }
    }
}

/* Answers s with an error status, then closes it once the answer is out. */
static void fail(Relay *r, Sock *s, int status) {
    release(r, s);
    RelayMsg m = { .type = RM_STATUS, .status = (uint8_t)status };
    out_msg(s, &m);
    s->kind = SK_CLOSING;
    s->in_len = 0;
    s->deadline = now_ms() + CLOSE_FLUSH_MS;
}

static void room_end(Relay *r, int idx) {
    r->rooms[idx].used = false;
    r->rooms[idx].control = NULL;
    r->nrooms--;
    for (int i = 0; i < r->nsocks; i++) {
        Sock *s = r->socks[i];
        if (s->dead || s->room != idx) continue;
        if (s->kind == SK_WAITING) fail(r, s, RS_NO_ROOM);
        else s->room = -1; /* linked players stay linked */
    }
    rlog(r, "room closed (%d open)", r->nrooms);
}

static int room_find(Relay *r, const uint8_t id[RELAY_ROOM_SIZE]) {
    for (int i = 0; i < r->lim.max_rooms; i++)
        if (r->rooms[i].used && memcmp(r->rooms[i].id, id, RELAY_ROOM_SIZE) == 0) return i;
    return -1;
}

/* At most joins_per_min JOINs per address (key) and minute. */
static bool rate_ok(Relay *r, const char *addr) {
    uint64_t now = now_ms();
    Rate *free_slot = NULL, *oldest = &r->rate[0];
    for (int i = 0; i < RATE_SLOTS; i++) {
        Rate *s = &r->rate[i];
        if (s->addr[0] && strcmp(s->addr, addr) == 0) {
            if (now - s->start >= 60000) {
                s->start = now;
                s->count = 0;
            }
            return ++s->count <= r->lim.joins_per_min;
        }
        if (!s->addr[0] || now - s->start >= 60000) free_slot = s;
        if (s->start < oldest->start) oldest = s;
    }
    Rate *s = free_slot ? free_slot : oldest;
    snprintf(s->addr, sizeof(s->addr), "%s", addr);
    s->start = now;
    s->count = 1;
    return s->count <= r->lim.joins_per_min;
}

/* ---- messages ------------------------------------------------------------------------------ */

static void on_host(Relay *r, Sock *s, const RelayMsg *m) {
    if (room_find(r, m->room) >= 0) {
        fail(r, s, RS_ROOM_TAKEN);
        return;
    }
    int mine = 0;
    for (int i = 0; i < r->lim.max_rooms; i++)
        if (r->rooms[i].used && strcmp(r->rooms[i].control->key, s->key) == 0) mine++;
    if (mine >= ROOMS_PER_ADDR) {
        rlog(r, "%s: too many rooms", s->addr);
        fail(r, s, RS_FULL);
        return;
    }
    int idx = -1;
    for (int i = 0; i < r->lim.max_rooms && idx < 0; i++)
        if (!r->rooms[i].used) idx = i;
    if (idx < 0) {
        rlog(r, "%s: no free room", s->addr);
        fail(r, s, RS_FULL);
        return;
    }
    r->rooms[idx] = (Room){ .used = true, .control = s };
    memcpy(r->rooms[idx].id, m->room, RELAY_ROOM_SIZE);
    r->nrooms++;
    uint64_t now = now_ms();
    s->kind = SK_CONTROL;
    s->room = idx;
    s->deadline = 0;
    s->last_rx = now;
    s->next_ping = now + (uint64_t)r->lim.ping_ms;
    RelayMsg ok = { .type = RM_STATUS, .status = RS_OK };
    out_msg(s, &ok);
    rlog(r, "%s opened a room (%d open)", s->addr, r->nrooms);
}

static void on_join(Relay *r, Sock *s, const RelayMsg *m) {
    if (!rate_ok(r, s->key)) {
        rlog(r, "%s: too many joins", s->addr);
        fail(r, s, RS_FULL);
        return;
    }
    int idx = room_find(r, m->room);
    if (idx < 0) {
        fail(r, s, RS_NO_ROOM);
        return;
    }
    Room *room = &r->rooms[idx];
    if (room->players >= r->lim.max_players) {
        fail(r, s, RS_FULL);
        return;
    }
    if (++r->next_ticket == 0) r->next_ticket = 1;
    room->players++;
    s->kind = SK_WAITING;
    s->room = idx;
    s->ticket = r->next_ticket;
    s->deadline = now_ms() + (uint64_t)r->lim.attach_ms;
    RelayMsg w = { .type = RM_WAITING, .ticket = s->ticket };
    snprintf(w.addr, sizeof(w.addr), "%s", s->addr);
    out_msg(room->control, &w);
    rlog(r, "%s is waiting for its GM", s->addr);
}

static void on_attach(Relay *r, Sock *s, const RelayMsg *m) {
    int idx = room_find(r, m->room);
    Sock *w = NULL;
    for (int i = 0; idx >= 0 && i < r->nsocks && !w; i++) {
        Sock *t = r->socks[i];
        if (!t->dead && t->kind == SK_WAITING && t->room == idx && t->ticket == m->ticket) w = t;
    }
    if (!w) {
        fail(r, s, RS_NO_ROOM);
        return;
    }
    RelayMsg ok = { .type = RM_STATUS, .status = RS_OK };
    if (!out_msg(w, &ok) || !out_msg(s, &ok)) { /* out of memory */
        sock_close(r, w);
        sock_close(r, s);
        return;
    }
    w->kind = SK_LINKED;
    w->player = true;
    w->deadline = 0;
    s->kind = SK_LINKED;
    s->deadline = 0;
    w->peer = s;
    s->peer = w;
    rlog(r, "%s linked", w->addr);
}

static void on_message(Relay *r, Sock *s, const RelayMsg *m) {
    if (s->kind == SK_CONTROL) {
        if (m->type != RM_PING) sock_close(r, s); /* a PING only refreshes last_rx */
        return;
    }
    if (s->kind != SK_NEW || (m->type != RM_HOST && m->type != RM_JOIN && m->type != RM_ATTACH)) {
        fail(r, s, RS_BAD_REQUEST);
        return;
    }
    if (m->version != RELAY_VERSION) {
        fail(r, s, RS_BAD_VERSION);
        return;
    }
    if (m->type == RM_HOST) on_host(r, s, m);
    else if (m->type == RM_JOIN) on_join(r, s, m);
    else on_attach(r, s, m);
}

/* ---- I/O ----------------------------------------------------------------------------------- */

static void do_read(Relay *r, Sock *s) {
    uint8_t buf[65536];
    for (;;) {
        if (s->dead) return;
        if (s->kind == SK_LINKED && s->peer && out_pending(s->peer) >= LINK_BUF_MAX) return; /* partner is slow */
        bool raw = s->kind == SK_LINKED || s->kind == SK_CLOSING;
        uint8_t *dst = raw ? buf : s->in + s->in_len;
        size_t cap = raw ? sizeof(buf) : sizeof(s->in) - s->in_len;
        ssize_t n = recv(s->fd, dst, cap, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (n == 0 && s->kind == SK_CLOSING) { /* half-closed: still send the answer */
            s->rd_eof = true;
            return;
        }
        if (n <= 0) {
            sock_close(r, s);
            return;
        }
        if (s->kind == SK_CLOSING) continue; /* nobody wants it */
        if (s->kind == SK_LINKED) {
            if (s->peer && !out_append(s->peer, buf, (size_t)n)) sock_close(r, s);
            continue;
        }
        s->in_len += (size_t)n;
        if (s->kind == SK_CONTROL) s->last_rx = now_ms();
        while (!s->dead && s->in_len > 0 && (s->kind == SK_NEW || s->kind == SK_CONTROL || s->kind == SK_WAITING)) {
            RelayMsg m;
            int used = relaymsg_decode(s->in, s->in_len, &m);
            if (used == 0 && s->in_len < sizeof(s->in)) break;
            if (used <= 0) {
                if (s->kind == SK_CONTROL) sock_close(r, s);
                else fail(r, s, RS_BAD_REQUEST);
                break;
            }
            memmove(s->in, s->in + used, s->in_len - (size_t)used);
            s->in_len -= (size_t)used;
            on_message(r, s, &m);
        }
        if (s->kind == SK_LINKED && s->in_len > 0) { /* bytes that came right behind ATTACH */
            bool ok = !s->peer || out_append(s->peer, s->in, s->in_len);
            s->in_len = 0;
            if (!ok) sock_close(r, s);
        }
    }
}

static void do_write(Relay *r, Sock *s) {
    while (out_pending(s) > 0) {
        ssize_t n = send(s->fd, s->out + s->out_pos, out_pending(s), MSG_NOSIGNAL);
        if (n > 0) {
            s->out_pos += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        sock_close(r, s);
        return;
    }
    s->out_pos = s->out_len = 0;
    if (s->kind == SK_CLOSING) sock_close(r, s);
}

static void addr_text(const struct sockaddr_storage *sa, char *out, size_t cap) {
    char ip[INET6_ADDRSTRLEN] = "?";
    if (sa->ss_family == AF_INET) {
        inet_ntop(AF_INET, &((const struct sockaddr_in *)sa)->sin_addr, ip, sizeof(ip));
    } else if (sa->ss_family == AF_INET6) {
        const struct in6_addr *a = &((const struct sockaddr_in6 *)sa)->sin6_addr;
        if (IN6_IS_ADDR_V4MAPPED(a)) inet_ntop(AF_INET, a->s6_addr + 12, ip, sizeof(ip));
        else inet_ntop(AF_INET6, a, ip, sizeof(ip));
    }
    snprintf(out, cap, "%s", ip);
}

/* The key per-address limits use: the IPv4 address (also when IPv4-mapped), or the IPv6 /64 prefix,
 * since one IPv6 host easily has a whole /64. */
static void addr_key(const struct sockaddr_storage *sa, char *out, size_t cap) {
    const struct in6_addr *a = &((const struct sockaddr_in6 *)sa)->sin6_addr;
    if (sa->ss_family != AF_INET6 || IN6_IS_ADDR_V4MAPPED(a)) {
        addr_text(sa, out, cap);
        return;
    }
    struct in6_addr prefix = *a;
    memset(prefix.s6_addr + 8, 0, 8);
    char ip[INET6_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET6, &prefix, ip, sizeof(ip));
    snprintf(out, cap, "%s/64", ip);
}

static void accept_all(Relay *r) {
    for (;;) {
        struct sockaddr_storage sa;
        socklen_t sl = sizeof(sa);
        int fd = accept(r->lfd, (struct sockaddr *)&sa, &sl);
        if (fd < 0) {
            /* out of descriptors or memory: the backlog keeps lfd readable, so pause instead of spinning */
            if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM) {
                uint64_t now = now_ms();
                if (now >= r->accept_pause_until + 1000) rlog(r, "accept: %s", strerror(errno)); /* once per run */
                r->accept_pause_until = now + ACCEPT_PAUSE_MS;
            }
            return;
        }
        if (r->nsocks >= r->lim.max_sockets) {
            close(fd);
            continue;
        }
        char addr[RELAY_ADDR_MAX], key[RELAY_ADDR_MAX];
        addr_text(&sa, addr, sizeof(addr));
        addr_key(&sa, key, sizeof(key));
        if (!sock_new(r, fd, addr, key)) close(fd);
    }
}

/* ---- the thread ---------------------------------------------------------------------------- */

static void timers(Relay *r, uint64_t now) {
    for (int i = 0; i < r->nsocks; i++) {
        Sock *s = r->socks[i];
        if (s->dead) continue;
        if (s->kind == SK_CLOSING) {
            if (out_pending(s) == 0 || now >= s->deadline) sock_close(r, s);
        } else if (s->kind == SK_NEW && now >= s->deadline) {
            fail(r, s, RS_BAD_REQUEST);
        } else if (s->kind == SK_WAITING && now >= s->deadline) {
            rlog(r, "%s: the GM did not attach", s->addr);
            fail(r, s, RS_TIMEOUT);
        } else if (s->kind == SK_CONTROL) {
            if (now - s->last_rx >= 3 * (uint64_t)r->lim.ping_ms) {
                rlog(r, "%s: the GM stopped answering", s->addr);
                sock_close(r, s);
            } else if (now >= s->next_ping) {
                RelayMsg ping = { .type = RM_PING };
                out_msg(s, &ping);
                s->next_ping = now + (uint64_t)r->lim.ping_ms;
            }
        }
    }
}

/* Milliseconds until timers has something to do. */
static int next_timeout(Relay *r, uint64_t now) {
    uint64_t best = now + 1000;
    if (r->accept_pause_until > now && r->accept_pause_until < best) best = r->accept_pause_until;
    for (int i = 0; i < r->nsocks; i++) {
        Sock *s = r->socks[i];
        uint64_t t = 0;
        if (s->kind == SK_CONTROL) {
            uint64_t dead = s->last_rx + 3 * (uint64_t)r->lim.ping_ms;
            t = s->next_ping < dead ? s->next_ping : dead;
        } else if (s->deadline) {
            t = s->deadline;
        }
        if (t && t < best) best = t;
    }
    return best > now ? (int)(best - now) : 0;
}

static void sweep(Relay *r) {
    int j = 0;
    for (int i = 0; i < r->nsocks; i++) {
        Sock *s = r->socks[i];
        if (s->dead) {
            free(s->out);
            free(s);
        } else {
            r->socks[j++] = s;
        }
    }
    r->nsocks = j;
}

static void *thread_main(void *arg) {
    Relay *r = arg;
    struct pollfd *pf = NULL;
    Sock **pc = NULL;
    int pcap = 0;
    for (;;) {
        uint64_t now = now_ms();
        timers(r, now);
        sweep(r);
        if (r->nsocks + 2 > pcap) {
            pcap = (r->nsocks + 2) * 2;
            pf = realloc(pf, (size_t)pcap * sizeof(*pf));
            pc = realloc(pc, (size_t)pcap * sizeof(*pc));
            if (!pf || !pc) abort();
        }
        int np = 0;
        pf[np] = (struct pollfd){ .fd = r->wake_rd, .events = POLLIN };
        pc[np++] = NULL;
        pf[np] = (struct pollfd){ .fd = now < r->accept_pause_until ? -1 : r->lfd, .events = POLLIN };
        pc[np++] = NULL;
        for (int i = 0; i < r->nsocks; i++) {
            Sock *s = r->socks[i];
            short ev = s->rd_eof ? 0 : POLLIN;
            if (s->kind == SK_LINKED && s->peer && out_pending(s->peer) >= LINK_BUF_MAX) ev = 0;
            if (out_pending(s)) ev |= POLLOUT;
            pf[np] = (struct pollfd){ .fd = s->fd, .events = ev };
            pc[np++] = s;
        }
        if (poll(pf, (nfds_t)np, next_timeout(r, now)) < 0 && errno != EINTR) {
            rlog(r, "poll: %s; the relay stops", strerror(errno));
            break;
        }
        if (pf[0].revents) break; /* relay_stop */
        if (pf[1].revents) accept_all(r);
        for (int i = 2; i < np; i++) {
            Sock *s = pc[i];
            short rev = pf[i].revents;
            if (s->dead || !rev) continue;
            if ((rev & (POLLHUP | POLLERR)) && !(pf[i].events & POLLIN)) { /* gone while back-pressured */
                sock_close(r, s);
                continue;
            }
            if (rev & (POLLIN | POLLHUP | POLLERR)) do_read(r, s);
            if (!s->dead && (rev & POLLOUT)) do_write(r, s);
        }
    }
    free(pf);
    free(pc);
    return NULL;
}

/* ---- public API ---------------------------------------------------------------------------- */

void relay_default_limits(RelayLimits *l) {
    *l = (RelayLimits){ .port = RELAY_DEFAULT_PORT, .max_rooms = 64, .max_players = 32, .max_sockets = 1024,
                        .joins_per_min = 5, .first_msg_ms = 5000, .attach_ms = 10000, .ping_ms = 20000 };
}

static int listen_socket(uint16_t port, char *err, size_t errcap) {
    /* dual stack first, plain IPv4 when the machine has no IPv6 */
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd >= 0) {
        int zero = 0, one = 1;
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in6 sa = { .sin6_family = AF_INET6, .sin6_port = htons(port), .sin6_addr = IN6ADDR_ANY_INIT };
        if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0 && listen(fd, 64) == 0) return fd;
        close(fd);
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port) };
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0 && listen(fd, 64) == 0) return fd;
        snprintf(err, errcap, "port %u: %s", port, strerror(errno));
        close(fd);
        return -1;
    }
    snprintf(err, errcap, "%s", strerror(errno));
    return -1;
}

/* Raises the descriptor limit to fit max_sockets, or lowers max_sockets to fit the limit. */
static void fit_fd_limit(Relay *r) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0) return;
    rlim_t want = (rlim_t)r->lim.max_sockets + 16;
    if (rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur < want) {
        rl.rlim_cur = want < rl.rlim_max ? want : rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
        if (getrlimit(RLIMIT_NOFILE, &rl) != 0) return;
    }
    if (rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur < want) {
        r->lim.max_sockets = rl.rlim_cur > 17 ? (int)(rl.rlim_cur - 16) : 1;
        rlog(r, "only %llu file descriptors: at most %d sockets", (unsigned long long)rl.rlim_cur, r->lim.max_sockets);
    }
}

Relay *relay_start(const RelayLimits *l, FILE *log, char *err, size_t errcap) {
    Relay *r = calloc(1, sizeof(*r));
    int fds[2] = { -1, -1 };
    if (!r || !(r->rooms = calloc((size_t)l->max_rooms, sizeof(*r->rooms)))) {
        snprintf(err, errcap, "out of memory");
        if (r) free(r->rooms);
        free(r);
        return NULL;
    }
    if (pipe(fds) != 0) {
        snprintf(err, errcap, "%s", strerror(errno));
        free(r->rooms);
        free(r);
        return NULL;
    }
    r->lim = *l;
    r->log = log;
    fit_fd_limit(r);
    r->wake_rd = fds[0];
    r->wake_wr = fds[1];
    r->lfd = listen_socket(l->port, err, errcap);
    if (r->lfd < 0) {
        close(fds[0]);
        close(fds[1]);
        free(r->rooms);
        free(r);
        return NULL;
    }
    set_nonblocking(r->lfd);
    struct sockaddr_storage sa;
    socklen_t sl = sizeof(sa);
    getsockname(r->lfd, (struct sockaddr *)&sa, &sl);
    r->port = ntohs(sa.ss_family == AF_INET6 ? ((struct sockaddr_in6 *)&sa)->sin6_port
                                             : ((struct sockaddr_in *)&sa)->sin_port);
    rlog(r, "listening on port %u", r->port);
    if (pthread_create(&r->thread, NULL, thread_main, r) != 0) {
        snprintf(err, errcap, "could not start the relay thread");
        close(r->lfd);
        close(fds[0]);
        close(fds[1]);
        free(r->rooms);
        free(r);
        return NULL;
    }
    return r;
}

uint16_t relay_port(const Relay *r) { return r ? r->port : 0; }

void relay_stop(Relay *r) {
    if (!r) return;
    char c = 1;
    if (write(r->wake_wr, &c, 1) < 0) { /* the thread is gone already */ }
    pthread_join(r->thread, NULL);
    for (int i = 0; i < r->nsocks; i++) {
        if (!r->socks[i]->dead) close(r->socks[i]->fd);
        free(r->socks[i]->out);
        free(r->socks[i]);
    }
    free(r->socks);
    free(r->rooms);
    close(r->lfd);
    close(r->wake_rd);
    close(r->wake_wr);
    free(r);
}
