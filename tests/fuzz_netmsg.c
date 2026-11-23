/* libFuzzer target for the network message decoder and the join code parser.
 * The first input byte picks the parser: even = netmsg_decode, odd = joincode_decode.
 * Seeds come from `build/netmsg_test <dir>`. */
#include "netmsg.h"

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

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0) return 0;
    if (data[0] & 1) fuzz_joincode(data + 1, size - 1);
    else fuzz_message(data + 1, size - 1);
    return 0;
}
