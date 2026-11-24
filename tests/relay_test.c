/* Tests for src/relaymsg.c and relay/relay.c (raw sockets on localhost). Run with `make test`. */
#include "relay.h"
#include "relaymsg.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int failures, checks;

#define CHECK(cond) do {                                                       \
        checks++;                                                              \
        if (!(cond)) {                                                         \
            failures++;                                                        \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                      \
    } while (0)

/* ---- messages ------------------------------------------------------------------------------ */

static bool msg_equal(const RelayMsg *a, const RelayMsg *b) {
    return a->type == b->type && a->version == b->version && memcmp(a->room, b->room, RELAY_ROOM_SIZE) == 0 &&
           a->ticket == b->ticket && a->status == b->status && strcmp(a->addr, b->addr) == 0;
}

static void test_round_trip(void) {
    RelayMsg msgs[] = {
        { .type = RM_HOST, .version = RELAY_VERSION, .room = { 1, 2, 3, [15] = 16 } },
        { .type = RM_JOIN, .version = 7, .room = { 9 } },
        { .type = RM_ATTACH, .version = RELAY_VERSION, .room = { 5 }, .ticket = 0xA1B2C3D4u },
        { .type = RM_STATUS, .status = RS_TIMEOUT },
        { .type = RM_WAITING, .ticket = 42, .addr = "2001:db8::1" },
        { .type = RM_PING },
    };
    for (size_t i = 0; i < sizeof(msgs) / sizeof(msgs[0]); i++) {
        uint8_t buf[RELAY_MSG_MAX];
        size_t n = relaymsg_encode(&msgs[i], buf);
        CHECK(n >= 5 && n <= RELAY_MSG_MAX);
        RelayMsg d;
        CHECK(relaymsg_decode(buf, n, &d) == (int)n && msg_equal(&d, &msgs[i]));
        /* every shorter prefix asks for more bytes */
        for (size_t k = 0; k < n; k++) CHECK(relaymsg_decode(buf, k, &d) == 0);
        /* a second message behind it is left alone */
        uint8_t two[2 * RELAY_MSG_MAX];
        memcpy(two, buf, n);
        memcpy(two + n, buf, n);
        CHECK(relaymsg_decode(two, 2 * n, &d) == (int)n);
    }
    /* the longest address fits */
    RelayMsg w = { .type = RM_WAITING, .ticket = 1 };
    memset(w.addr, 'a', RELAY_ADDR_MAX - 1);
    uint8_t buf[RELAY_MSG_MAX];
    CHECK(relaymsg_encode(&w, buf) == RELAY_MSG_MAX);
}

static void test_wire_bytes(void) {
    uint8_t buf[RELAY_MSG_MAX];
    RelayMsg a = { .type = RM_ATTACH, .version = RELAY_VERSION, .ticket = 0x01020304u };
    memset(a.room, 0xAA, RELAY_ROOM_SIZE);
    uint8_t want_a[26] = { 'T', 'O', 'H', 'R', RM_ATTACH, RELAY_VERSION };
    memset(want_a + 6, 0xAA, RELAY_ROOM_SIZE);
    want_a[22] = 4; want_a[23] = 3; want_a[24] = 2; want_a[25] = 1;
    CHECK(relaymsg_encode(&a, buf) == 26 && memcmp(buf, want_a, 26) == 0);

    RelayMsg w = { .type = RM_WAITING, .ticket = 0x01020304u, .addr = "1.2.3.4" };
    const uint8_t want_w[] = { 'T', 'O', 'H', 'R', RM_WAITING, 4, 3, 2, 1, 7, '1', '.', '2', '.', '3', '.', '4' };
    CHECK(relaymsg_encode(&w, buf) == sizeof(want_w) && memcmp(buf, want_w, sizeof(want_w)) == 0);
}

static void test_rejects(void) {
    RelayMsg d;
    const uint8_t max_status[] = { 'T', 'O', 'H', 'R', RM_STATUS, RS_BAD_REQUEST };
    CHECK(relaymsg_decode(max_status, sizeof(max_status), &d) == 6 && d.status == RS_BAD_REQUEST);
    const uint8_t bad_magic[] = { 'T', 'O', 'H', 'X', RM_PING };
    CHECK(relaymsg_decode(bad_magic, sizeof(bad_magic), &d) == -1);
    CHECK(relaymsg_decode(bad_magic, 3, &d) == 0); /* "TOH" could still become "TOHR" */
    CHECK(relaymsg_decode((const uint8_t *)"GET / HTTP/1.1", 14, &d) == -1);
    const uint8_t bad_type[] = { 'T', 'O', 'H', 'R', 99 };
    CHECK(relaymsg_decode(bad_type, sizeof(bad_type), &d) == -1);
    const uint8_t bad_status[] = { 'T', 'O', 'H', 'R', RM_STATUS, RS_BAD_REQUEST + 1 };
    CHECK(relaymsg_decode(bad_status, sizeof(bad_status), &d) == -1);
    const uint8_t bad_addr[] = { 'T', 'O', 'H', 'R', RM_WAITING, 1, 0, 0, 0, 2, 'a', ' ' };
    CHECK(relaymsg_decode(bad_addr, sizeof(bad_addr), &d) == -1);
    const uint8_t empty_addr[] = { 'T', 'O', 'H', 'R', RM_WAITING, 1, 0, 0, 0, 0 };
    CHECK(relaymsg_decode(empty_addr, sizeof(empty_addr), &d) == -1);
    const uint8_t long_addr[] = { 'T', 'O', 'H', 'R', RM_WAITING, 1, 0, 0, 0, RELAY_ADDR_MAX };
    CHECK(relaymsg_decode(long_addr, sizeof(long_addr), &d) == -1);

    uint8_t buf[RELAY_MSG_MAX];
    RelayMsg m = { .type = RM_STATUS, .status = 200 };
    CHECK(relaymsg_encode(&m, buf) == 0);
    m = (RelayMsg){ .type = 0 };
    CHECK(relaymsg_encode(&m, buf) == 0);
    m = (RelayMsg){ .type = RM_WAITING, .addr = "two words" };
    CHECK(relaymsg_encode(&m, buf) == 0);
    m = (RelayMsg){ .type = RM_WAITING, .addr = "" };
    CHECK(relaymsg_encode(&m, buf) == 0);
}

static void test_parse_addr(void) {
    char host[64];
    uint16_t port;
    CHECK(relay_parse_addr("nas.example.net", host, sizeof(host), &port) && strcmp(host, "nas.example.net") == 0 &&
          port == RELAY_DEFAULT_PORT);
    CHECK(relay_parse_addr("127.0.0.1:5000", host, sizeof(host), &port) && strcmp(host, "127.0.0.1") == 0 &&
          port == 5000);
    CHECK(relay_parse_addr("[2001:db8::1]:41000", host, sizeof(host), &port) && strcmp(host, "2001:db8::1") == 0 &&
          port == 41000);
    CHECK(relay_parse_addr("[::1]", host, sizeof(host), &port) && strcmp(host, "::1") == 0 &&
          port == RELAY_DEFAULT_PORT);
    CHECK(relay_parse_addr("2001:db8::1", host, sizeof(host), &port) && strcmp(host, "2001:db8::1") == 0 &&
          port == RELAY_DEFAULT_PORT);
    CHECK(!relay_parse_addr("", host, sizeof(host), &port));
    CHECK(!relay_parse_addr("host:", host, sizeof(host), &port));
    CHECK(!relay_parse_addr("host:0", host, sizeof(host), &port));
    CHECK(!relay_parse_addr("host:65536", host, sizeof(host), &port));
    CHECK(!relay_parse_addr("host:12a", host, sizeof(host), &port));
    CHECK(!relay_parse_addr(":41234", host, sizeof(host), &port));
    CHECK(!relay_parse_addr("[]:1", host, sizeof(host), &port));
    CHECK(!relay_parse_addr("[::1]x", host, sizeof(host), &port));
    CHECK(!relay_parse_addr("averyverylonghostname", host, 8, &port));
}

/* ---- the relay, with raw sockets ------------------------------------------------------------ */

static int dial(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port) };
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void put(int fd, const RelayMsg *m) {
    uint8_t buf[RELAY_MSG_MAX];
    size_t n = relaymsg_encode(m, buf);
    CHECK(n > 0 && send(fd, buf, n, MSG_NOSIGNAL) == (ssize_t)n);
}

/* Reads one message, byte by byte so nothing behind it is consumed. */
static bool get(int fd, RelayMsg *m, int ms) {
    uint8_t buf[RELAY_MSG_MAX];
    size_t n = 0;
    while (n < sizeof(buf)) {
        struct pollfd pf = { .fd = fd, .events = POLLIN };
        if (poll(&pf, 1, ms) <= 0 || recv(fd, buf + n, 1, 0) != 1) return false;
        int r = relaymsg_decode(buf, ++n, m);
        if (r > 0) return true;
        if (r < 0) return false;
    }
    return false;
}

static bool get_status(int fd, int status, int ms) {
    RelayMsg m;
    return get(fd, &m, ms) && m.type == RM_STATUS && m.status == status;
}

/* The relay closed fd within ms (whatever it still sent is skipped). */
static bool closed(int fd, int ms) {
    uint8_t b[256];
    for (;;) {
        struct pollfd pf = { .fd = fd, .events = POLLIN };
        if (poll(&pf, 1, ms) <= 0) return false;
        if (recv(fd, b, sizeof(b), 0) <= 0) return true;
    }
}

static bool recv_exact(int fd, void *p, size_t n, int ms) {
    size_t got = 0;
    while (got < n) {
        struct pollfd pf = { .fd = fd, .events = POLLIN };
        if (poll(&pf, 1, ms) <= 0) return false;
        ssize_t r = recv(fd, (char *)p + got, n - got, 0);
        if (r <= 0) return false;
        got += (size_t)r;
    }
    return true;
}

typedef struct {
    int fd;
    const uint8_t *p;
    size_t n;
} SendJob;

static void *send_all(void *arg) {
    SendJob *j = arg;
    size_t off = 0;
    while (off < j->n) {
        ssize_t w = send(j->fd, j->p + off, j->n - off, MSG_NOSIGNAL);
        if (w <= 0) break;
        off += (size_t)w;
    }
    return NULL;
}

static RelayMsg room_msg(int type, uint8_t id) {
    RelayMsg m = { .type = (uint8_t)type, .version = RELAY_VERSION };
    memset(m.room, id, RELAY_ROOM_SIZE);
    return m;
}

static Relay *start(RelayLimits *l) {
    char err[128];
    l->port = 0;
    Relay *r = relay_start(l, NULL, err, sizeof(err));
    if (!r) fprintf(stderr, "relay_start: %s\n", err);
    CHECK(r != NULL);
    return r;
}

static void test_link(void) {
    RelayLimits l;
    relay_default_limits(&l);
    Relay *r = start(&l);
    if (!r) return;
    uint16_t port = relay_port(r);

    int gm = dial(port);
    RelayMsg host = room_msg(RM_HOST, 1);
    put(gm, &host);
    CHECK(get_status(gm, RS_OK, 2000));

    int pl = dial(port);
    RelayMsg join = room_msg(RM_JOIN, 1), w;
    put(pl, &join);
    CHECK(get(gm, &w, 2000) && w.type == RM_WAITING && w.ticket != 0 && strcmp(w.addr, "127.0.0.1") == 0);

    int at = dial(port);
    RelayMsg attach = room_msg(RM_ATTACH, 1);
    attach.ticket = w.ticket;
    put(at, &attach);
    CHECK(get_status(at, RS_OK, 2000));
    CHECK(get_status(pl, RS_OK, 2000));

    /* bytes pass both ways, unchanged */
    char buf[8] = { 0 };
    CHECK(send(pl, "hello", 5, 0) == 5 && recv_exact(at, buf, 5, 2000) && memcmp(buf, "hello", 5) == 0);
    CHECK(send(at, "world", 5, 0) == 5 && recv_exact(pl, buf, 5, 2000) && memcmp(buf, "world", 5) == 0);

    /* more than the relay buffers (256 KiB) arrives complete */
    size_t big = 3u << 20;
    uint8_t *data = malloc(big), *got = malloc(big);
    CHECK(data != NULL && got != NULL);
    if (data && got) {
        for (size_t i = 0; i < big; i++) data[i] = (uint8_t)(i * 13 + 5);
        SendJob job = { at, data, big };
        pthread_t t;
        bool started = pthread_create(&t, NULL, send_all, &job) == 0;
        CHECK(started);
        if (started) {
            CHECK(recv_exact(pl, got, big, 5000) && memcmp(got, data, big) == 0);
            pthread_join(t, NULL);
        }
    }
    free(data);
    free(got);

    /* when one side closes, the other is closed too */
    close(at);
    CHECK(closed(pl, 2000));
    close(pl);
    close(gm);
    relay_stop(r);
}

static void test_refusals(void) {
    RelayLimits l;
    relay_default_limits(&l);
    l.first_msg_ms = 200;
    Relay *r = start(&l);
    if (!r) return;
    uint16_t port = relay_port(r);

    int gm = dial(port);
    RelayMsg host = room_msg(RM_HOST, 1);
    put(gm, &host);
    CHECK(get_status(gm, RS_OK, 2000));

    int fd = dial(port); /* the same room twice */
    put(fd, &host);
    CHECK(get_status(fd, RS_ROOM_TAKEN, 2000) && closed(fd, 2000));
    close(fd);

    fd = dial(port); /* a room nobody hosts */
    RelayMsg join = room_msg(RM_JOIN, 2);
    put(fd, &join);
    CHECK(get_status(fd, RS_NO_ROOM, 2000) && closed(fd, 2000));
    close(fd);

    fd = dial(port); /* an attach without a waiting player */
    RelayMsg attach = room_msg(RM_ATTACH, 1);
    attach.ticket = 12345;
    put(fd, &attach);
    CHECK(get_status(fd, RS_NO_ROOM, 2000) && closed(fd, 2000));
    close(fd);

    fd = dial(port); /* another protocol version */
    RelayMsg newer = room_msg(RM_JOIN, 1);
    newer.version = RELAY_VERSION + 1;
    put(fd, &newer);
    CHECK(get_status(fd, RS_BAD_VERSION, 2000) && closed(fd, 2000));
    close(fd);

    fd = dial(port); /* not our protocol at all */
    CHECK(send(fd, "GET / HTTP/1.1\r\n\r\n", 18, 0) == 18);
    CHECK(get_status(fd, RS_BAD_REQUEST, 2000) && closed(fd, 2000));
    close(fd);

    fd = dial(port); /* a STATUS is not a request */
    RelayMsg st = { .type = RM_STATUS, .status = RS_OK };
    put(fd, &st);
    CHECK(get_status(fd, RS_BAD_REQUEST, 2000) && closed(fd, 2000));
    close(fd);

    fd = dial(port); /* silence */
    CHECK(get_status(fd, RS_BAD_REQUEST, 1000) && closed(fd, 1000));
    close(fd);

    close(gm);
    relay_stop(r);
}

static void test_limits(void) {
    RelayLimits l;
    relay_default_limits(&l);
    l.max_rooms = 1;
    l.max_players = 1;
    l.attach_ms = 1000;
    Relay *r = start(&l);
    if (!r) return;
    uint16_t port = relay_port(r);

    int gm = dial(port);
    RelayMsg host = room_msg(RM_HOST, 1);
    put(gm, &host);
    CHECK(get_status(gm, RS_OK, 2000));

    int fd = dial(port); /* a second room */
    RelayMsg host2 = room_msg(RM_HOST, 2);
    put(fd, &host2);
    CHECK(get_status(fd, RS_FULL, 2000) && closed(fd, 2000));
    close(fd);

    /* player 1 waits; player 2 finds the room full; player 1 times out (the GM never attaches) */
    RelayMsg join = room_msg(RM_JOIN, 1), w;
    int p1 = dial(port);
    put(p1, &join);
    CHECK(get(gm, &w, 2000) && w.type == RM_WAITING);
    int p2 = dial(port);
    put(p2, &join);
    CHECK(get_status(p2, RS_FULL, 2000) && closed(p2, 2000));
    close(p2);
    CHECK(get_status(p1, RS_TIMEOUT, 2000) && closed(p1, 2000));
    close(p1);

    /* the slot is free again; when the GM goes, the waiting player hears that the room is gone */
    int p3 = dial(port);
    put(p3, &join);
    CHECK(get(gm, &w, 2000) && w.type == RM_WAITING);
    close(gm);
    CHECK(get_status(p3, RS_NO_ROOM, 2000));
    close(p3);
    relay_stop(r);
}

static void test_rate(void) {
    RelayLimits l;
    relay_default_limits(&l);
    l.joins_per_min = 2;
    Relay *r = start(&l);
    if (!r) return;
    uint16_t port = relay_port(r);

    int gm = dial(port);
    RelayMsg host = room_msg(RM_HOST, 1), join = room_msg(RM_JOIN, 1), w;
    put(gm, &host);
    CHECK(get_status(gm, RS_OK, 2000));
    int p[3];
    for (int i = 0; i < 2; i++) {
        p[i] = dial(port);
        put(p[i], &join);
        CHECK(get(gm, &w, 2000) && w.type == RM_WAITING);
    }
    p[2] = dial(port); /* the room has space, but this address joined twice this minute */
    put(p[2], &join);
    CHECK(get_status(p[2], RS_FULL, 2000));
    for (int i = 0; i < 3; i++) close(p[i]);
    close(gm);
    relay_stop(r);
}

static void test_ping(void) {
    RelayLimits l;
    relay_default_limits(&l);
    l.ping_ms = 200;
    Relay *r = start(&l);
    if (!r) return;
    uint16_t port = relay_port(r);

    int gm = dial(port);
    RelayMsg host = room_msg(RM_HOST, 1), m;
    put(gm, &host);
    CHECK(get_status(gm, RS_OK, 2000));
    /* answered pings keep the room */
    for (int i = 0; i < 5; i++) {
        CHECK(get(gm, &m, 1000) && m.type == RM_PING);
        put(gm, &m);
    }
    int fd = dial(port);
    put(fd, &host);
    CHECK(get_status(fd, RS_ROOM_TAKEN, 2000));
    close(fd);
    /* a GM that stops answering loses it */
    CHECK(closed(gm, 2000));
    close(gm);
    fd = dial(port);
    put(fd, &host);
    CHECK(get_status(fd, RS_OK, 2000));
    close(fd);
    relay_stop(r);
}

/* A linked player holds its room slot until it closes, and outlives its GM's control connection. */
static void test_linked_slot(void) {
    RelayLimits l;
    relay_default_limits(&l);
    l.max_players = 1;
    Relay *r = start(&l);
    if (!r) return;
    uint16_t port = relay_port(r);

    int gm = dial(port);
    RelayMsg host = room_msg(RM_HOST, 1), join = room_msg(RM_JOIN, 1), attach = room_msg(RM_ATTACH, 1), w;
    put(gm, &host);
    CHECK(get_status(gm, RS_OK, 2000));

    int pl = dial(port);
    put(pl, &join);
    CHECK(get(gm, &w, 2000) && w.type == RM_WAITING);
    int at = dial(port);
    attach.ticket = w.ticket;
    put(at, &attach);
    CHECK(get_status(at, RS_OK, 2000) && get_status(pl, RS_OK, 2000));
    close(pl);
    CHECK(closed(at, 2000));
    close(at);

    /* the slot is free again */
    pl = dial(port);
    put(pl, &join);
    CHECK(get(gm, &w, 2000) && w.type == RM_WAITING);
    at = dial(port);
    attach.ticket = w.ticket;
    put(at, &attach);
    CHECK(get_status(at, RS_OK, 2000) && get_status(pl, RS_OK, 2000));

    /* the GM's control connection goes; the link stays */
    close(gm);
    char buf[8] = { 0 };
    CHECK(send(pl, "hello", 5, 0) == 5 && recv_exact(at, buf, 5, 2000) && memcmp(buf, "hello", 5) == 0);
    CHECK(send(at, "world", 5, 0) == 5 && recv_exact(pl, buf, 5, 2000) && memcmp(buf, "world", 5) == 0);
    close(pl);
    close(at);
    relay_stop(r);
}

/* At most 2 rooms per source address. */
static void test_rooms_per_addr(void) {
    RelayLimits l;
    relay_default_limits(&l);
    Relay *r = start(&l);
    if (!r) return;
    uint16_t port = relay_port(r);

    int gm[3];
    for (int i = 0; i < 3; i++) {
        gm[i] = dial(port);
        RelayMsg host = room_msg(RM_HOST, (uint8_t)(i + 1));
        put(gm[i], &host);
        CHECK(get_status(gm[i], i < 2 ? RS_OK : RS_FULL, 2000));
    }
    CHECK(closed(gm[2], 2000));
    for (int i = 0; i < 3; i++) close(gm[i]);
    relay_stop(r);
}

/* A client that stops sending after its request still gets the answer. */
static void test_half_close(void) {
    RelayLimits l;
    relay_default_limits(&l);
    Relay *r = start(&l);
    if (!r) return;

    int fd = dial(relay_port(r));
    RelayMsg join = room_msg(RM_JOIN, 3);
    put(fd, &join);
    CHECK(shutdown(fd, SHUT_WR) == 0);
    CHECK(get_status(fd, RS_NO_ROOM, 2000) && closed(fd, 2000));
    close(fd);
    relay_stop(r);
}

int main(void) {
    test_round_trip();
    test_wire_bytes();
    test_rejects();
    test_parse_addr();
    test_link();
    test_refusals();
    test_limits();
    test_rate();
    test_ping();
    test_linked_slot();
    test_rooms_per_addr();
    test_half_close();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
