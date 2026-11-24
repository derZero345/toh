/* libFuzzer target for the network message decoder, the join code parser and the relay messages.
 * The first input byte picks the parser (% 3): 0 = netmsg_decode, 1 = joincode_decode, 2 = relaymsg_decode.
 * Seeds come from `build/netmsg_test <dir>`. */
#include "netmsg.h"
#include "relaymsg.h"

#include <stdlib.h>
#include <string.h>

static void fuzz_message(const uint8_t *data, size_t size) {
    NetMsg m;
    if (netmsg_decode(data, size, &m)) {
        /* everything the decoder accepts must encode again, and that encoding must be stable */
        uint8_t *a, *b;
        size_t alen, blen;
        if (!netmsg_encode(&m, &a, &alen)) abort();
        NetMsg m2;
        if (!netmsg_decode(a, alen, &m2)) abort();
        if (!netmsg_encode(&m2, &b, &blen)) abort();
        if (alen != blen || memcmp(a, b, alen) != 0) abort();
        free(a);
        free(b);
        netmsg_free(&m2);
    }
    netmsg_free(&m);
}

static void fuzz_joincode(const uint8_t *data, size_t size) {
    char *s = malloc(size + 1);
    if (!s) return;
    memcpy(s, data, size);
    s[size] = '\0';
    JoinCode c, d;
    if (joincode_decode(s, &c)) {
        char out[JOINCODE_MAX_LEN];
        if (!joincode_encode(&c, out) || !joincode_decode(out, &d)) abort();
        char again[JOINCODE_MAX_LEN];
        if (!joincode_encode(&d, again) || strcmp(out, again) != 0) abort();
    }
    free(s);
}

static void fuzz_relaymsg(const uint8_t *data, size_t size) {
    RelayMsg m;
    int used = relaymsg_decode(data, size, &m);
    if (used > 0) {
        /* what the decoder accepts encodes back to the same bytes */
        uint8_t out[RELAY_MSG_MAX];
        size_t n = relaymsg_encode(&m, out);
        if (n != (size_t)used || memcmp(out, data, n) != 0) abort();
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0) return 0;
    switch (data[0] % 3) {
    case 0: fuzz_message(data + 1, size - 1); break;
    case 1: fuzz_joincode(data + 1, size - 1); break;
    default: fuzz_relaymsg(data + 1, size - 1); break;
    }
    return 0;
}
