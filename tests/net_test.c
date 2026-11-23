/* Tests for src/net.c: a host and clients on localhost in one process. Run with `make test`. */
#include "net.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int failures, checks;

#define CHECK(cond) do {                                                       \
        checks++;                                                              \
        if (!(cond)) {                                                         \
            failures++;                                                        \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                      \
    } while (0)

static void sleep_ms(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* Waits up to ms for the next event of n. */
static bool wait_event(Net *n, NetEvent *ev, int ms) {
    uint64_t end = now_ms() + (uint64_t)ms;
    while (now_ms() < end) {
        if (net_next(n, ev)) return true;
        sleep_ms(1);
    }
    return false;
}

static JoinCode local_code(uint16_t port, const uint8_t secret[NET_SECRET_SIZE]) {
    JoinCode c = { .port = port, .naddrs = 1 };
    memcpy(c.secret, secret, NET_SECRET_SIZE);
    c.addrs[0] = (NetAddr){ .family = 4, .addr = { 127, 0, 0, 1 } };
    return c;
}

static int wakes;
static void count_wake(void *ctx) { (void)ctx; __atomic_add_fetch(&wakes, 1, __ATOMIC_SEQ_CST); }

static void test_exchange(void) {
    uint8_t secret[NET_SECRET_SIZE];
    randombytes_buf(secret, sizeof(secret));
    char err[128];
    Net *host = net_host(0, secret, count_wake, NULL, err, sizeof(err));
    CHECK(host != NULL);
    if (!host) { fprintf(stderr, "net_host: %s\n", err); return; }
    CHECK(net_port(host) != 0);

    JoinCode code = local_code(net_port(host), secret);
    Net *client = net_join(&code, NULL, NULL);
    NetEvent ev;
    CHECK(wait_event(client, &ev, 3000) && ev.type == NET_EV_CONNECTED && ev.conn == 1);
    net_event_free(&ev);
    CHECK(wait_event(host, &ev, 3000) && ev.type == NET_EV_CONNECTED && ev.conn > 0);
    int conn = ev.conn;
    CHECK(strstr(ev.addr, "127.0.0.1") != NULL);
    net_event_free(&ev);
    CHECK(wakes > 0);

    NetMsg hello = { .type = NM_HELLO, .protocol = NET_PROTOCOL_VERSION, .name = "Bob" };
    CHECK(net_send(client, 1, &hello));
    CHECK(wait_event(host, &ev, 3000) && ev.type == NET_EV_MESSAGE && ev.conn == conn &&
          ev.msg.type == NM_HELLO && strcmp(ev.msg.name, "Bob") == 0);
    net_event_free(&ev);

    /* a large message and several small ones arrive complete and in order */
    size_t big = 5u << 20;
    uint8_t *data = malloc(big);
    for (size_t i = 0; i < big; i++) data[i] = (uint8_t)(i * 31 + 7);
    NetMsg snap = { .type = NM_SNAPSHOT, .data = data, .data_len = big };
    CHECK(net_send(host, conn, &snap));
    for (int i = 0; i < 10; i++) {
        NetMsg log = { .type = NM_LOG };
        snprintf(log.text, sizeof(log.text), "line %d", i);
        CHECK(net_send(host, conn, &log));
    }
    CHECK(wait_event(client, &ev, 5000) && ev.type == NET_EV_MESSAGE && ev.msg.type == NM_SNAPSHOT &&
          ev.msg.data_len == big && memcmp(ev.msg.data, data, big) == 0);
    net_event_free(&ev);
    free(data);
    for (int i = 0; i < 10; i++) {
        char want[24];
        snprintf(want, sizeof(want), "line %d", i);
        CHECK(wait_event(client, &ev, 3000) && ev.type == NET_EV_MESSAGE && strcmp(ev.msg.text, want) == 0);
        net_event_free(&ev);
    }

    /* invalid messages are refused before they are queued */
    NetMsg bad = { .type = NM_HELLO, .name = "two words" };
    CHECK(!net_send(client, 1, &bad));
    CHECK(!net_send(host, 999, &hello));

    /* net_close sends what is queued first */
    NetMsg bye = { .type = NM_CLOSE, .reason = NET_CLOSE_KICKED };
    CHECK(net_send(host, conn, &bye));
    net_close(host, conn);
    CHECK(wait_event(client, &ev, 3000) && ev.type == NET_EV_MESSAGE && ev.msg.type == NM_CLOSE);
    net_event_free(&ev);
    CHECK(wait_event(client, &ev, 3000) && ev.type == NET_EV_CLOSED && ev.end == NET_END_LOST);
    net_event_free(&ev);
    CHECK(wait_event(host, &ev, 3000) && ev.type == NET_EV_CLOSED && ev.conn == conn && ev.end == NET_END_NORMAL);
    net_event_free(&ev);

    net_free(client);
    net_free(host);
}

static void test_wrong_secret(void) {
    uint8_t secret[NET_SECRET_SIZE] = { 1 }, other[NET_SECRET_SIZE] = { 2 };
    char err[128];
    Net *host = net_host(0, secret, NULL, NULL, err, sizeof(err));
    JoinCode code = local_code(net_port(host), other);
    Net *client = net_join(&code, NULL, NULL);
    NetEvent ev;
    CHECK(wait_event(client, &ev, 3000) && ev.type == NET_EV_CONNECTED);
    net_event_free(&ev);
    NetMsg hello = { .type = NM_HELLO, .protocol = NET_PROTOCOL_VERSION, .name = "Eve" };
    CHECK(net_send(client, 1, &hello));
    /* the host drops the connection without passing on a message */
    bool closed = false, message = false;
    while (wait_event(host, &ev, 2000)) {
        if (ev.type == NET_EV_MESSAGE) message = true;
        if (ev.type == NET_EV_CLOSED && ev.end == NET_END_BAD_DATA) closed = true;
        bool done = ev.type == NET_EV_CLOSED;
        net_event_free(&ev);
        if (done) break;
    }
    CHECK(closed && !message);
    CHECK(wait_event(client, &ev, 3000) && ev.type == NET_EV_CLOSED && ev.end == NET_END_LOST);
    net_event_free(&ev);
    net_free(client);
    net_free(host);
}

static void test_garbage_and_timeout(void) {
    uint8_t secret[NET_SECRET_SIZE] = { 3 };
    char err[128];
    Net *host = net_host(0, secret, NULL, NULL, err, sizeof(err));

    /* a peer that sends junk instead of the greeting is dropped */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(net_port(host)) };
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0);
    CHECK(write(fd, "GET / HTTP/1.1\r\n\r\nxxxxxxxxxxxxxxxx", 34) == 34);
    char buf[64];
    CHECK(read(fd, buf, sizeof(buf)) <= 0); /* closed by the host */
    close(fd);

    /* a peer that never finishes the handshake is dropped after NET_HELLO_TIMEOUT_MS */
    fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0);
    uint64_t t0 = now_ms();
    CHECK(read(fd, buf, sizeof(buf)) <= 0);
    uint64_t waited = now_ms() - t0;
    CHECK(waited >= NET_HELLO_TIMEOUT_MS - 200 && waited < NET_HELLO_TIMEOUT_MS + 2000);
    close(fd);

    /* neither reached the main thread as a connection */
    NetEvent ev;
    CHECK(!net_next(host, &ev));
    net_free(host);
}

static void test_unreachable(void) {
    /* find a port with nothing listening */
    uint8_t secret[NET_SECRET_SIZE] = { 4 };
    char err[128];
    Net *host = net_host(0, secret, NULL, NULL, err, sizeof(err));
    uint16_t port = net_port(host);
    net_free(host);

    JoinCode code = local_code(port, secret);
    Net *client = net_join(&code, NULL, NULL);
    NetEvent ev;
    CHECK(wait_event(client, &ev, 6000) && ev.type == NET_EV_CLOSED && ev.end == NET_END_UNREACHABLE);
    net_event_free(&ev);
    net_free(client);
}

static void test_free_while_connecting(void) {
    /* 192.0.2.1 (TEST-NET-1) does not answer: net_free must not wait for the connect timeout */
    uint8_t secret[NET_SECRET_SIZE] = { 5 };
    JoinCode code = { .port = 41234, .naddrs = 1 };
    memcpy(code.secret, secret, sizeof(secret));
    code.addrs[0] = (NetAddr){ .family = 4, .addr = { 192, 0, 2, 1 } };
    Net *client = net_join(&code, NULL, NULL);
    sleep_ms(100);
    uint64_t t0 = now_ms();
    net_free(client);
    CHECK(now_ms() - t0 < 1500);
}

int main(void) {
    if (sodium_init() < 0) return 1;
    test_exchange();
    test_wrong_secret();
    test_garbage_and_timeout();
    test_unreachable();
    test_free_while_connecting();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
