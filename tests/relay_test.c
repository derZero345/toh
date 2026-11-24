/* Tests for src/relaymsg.c (and, from task 3 on, relay/relay.c with raw sockets on localhost).
 * Run with `make test`. */
#include "relaymsg.h"

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

static void test_rejects(void) {
    RelayMsg d;
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

int main(void) {
    test_round_trip();
    test_rejects();
    test_parse_addr();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
