/* Relay messages: see relaymsg.h. */
#include "relaymsg.h"

#include <stdio.h>
#include <string.h>

static const uint8_t MAGIC[4] = { 'T', 'O', 'H', 'R' };

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool addr_ok(const char *s, size_t len) {
    if (len < 1 || len >= RELAY_ADDR_MAX) return false;
    for (size_t i = 0; i < len; i++)
        if (s[i] < 0x21 || s[i] > 0x7e) return false;
    return true;
}

size_t relaymsg_encode(const RelayMsg *m, uint8_t out[RELAY_MSG_MAX]) {
    memcpy(out, MAGIC, 4);
    size_t n = 4;
    out[n++] = m->type;
    switch (m->type) {
    case RM_HOST:
    case RM_JOIN:
    case RM_ATTACH:
        out[n++] = m->version;
        memcpy(out + n, m->room, RELAY_ROOM_SIZE);
        n += RELAY_ROOM_SIZE;
        if (m->type == RM_ATTACH) {
            put_u32(out + n, m->ticket);
            n += 4;
        }
        return n;
    case RM_STATUS:
        if (m->status > RS_BAD_REQUEST) return 0;
        out[n++] = m->status;
        return n;
    case RM_WAITING: {
        size_t len = strnlen(m->addr, RELAY_ADDR_MAX);
        if (!addr_ok(m->addr, len)) return 0;
        put_u32(out + n, m->ticket);
        n += 4;
        out[n++] = (uint8_t)len;
        memcpy(out + n, m->addr, len);
        return n + len;
    }
    case RM_PING:
        return n;
    default:
        return 0;
    }
}

int relaymsg_decode(const uint8_t *buf, size_t len, RelayMsg *out) {
    memset(out, 0, sizeof(*out));
    if (memcmp(buf, MAGIC, len < 4 ? len : 4) != 0) return -1;
    if (len < 5) return 0;
    uint8_t type = buf[4];
    size_t need;
    switch (type) {
    case RM_HOST:
    case RM_JOIN:   need = 5 + 1 + RELAY_ROOM_SIZE; break;
    case RM_ATTACH: need = 5 + 1 + RELAY_ROOM_SIZE + 4; break;
    case RM_STATUS: need = 6; break;
    case RM_PING:   need = 5; break;
    case RM_WAITING:
        if (len < 10) return 0;
        if (buf[9] == 0 || buf[9] >= RELAY_ADDR_MAX) return -1;
        need = 10 + buf[9];
        break;
    default:
        return -1;
    }
    if (len < need) return 0;
    const uint8_t *p = buf + 5;
    out->type = type;
    switch (type) {
    case RM_HOST:
    case RM_JOIN:
    case RM_ATTACH:
        out->version = p[0];
        memcpy(out->room, p + 1, RELAY_ROOM_SIZE);
        if (type == RM_ATTACH) out->ticket = get_u32(p + 1 + RELAY_ROOM_SIZE);
        break;
    case RM_STATUS:
        out->status = p[0];
        if (out->status > RS_BAD_REQUEST) return -1;
        break;
    case RM_WAITING:
        out->ticket = get_u32(p);
        memcpy(out->addr, p + 5, p[4]);
        out->addr[p[4]] = '\0';
        if (!addr_ok(out->addr, p[4])) return -1;
        break;
    }
    return (int)need;
}

static bool parse_port(const char *s, uint16_t *port) {
    if (!*s) return false;
    unsigned long v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return false;
        v = v * 10 + (unsigned long)(*s - '0');
        if (v > 65535) return false;
    }
    if (v == 0) return false;
    *port = (uint16_t)v;
    return true;
}

static bool copy_host(const char *s, size_t len, char *host, size_t cap) {
    if (len == 0 || len >= cap) return false;
    memcpy(host, s, len);
    host[len] = '\0';
    return true;
}

bool relay_parse_addr(const char *s, char *host, size_t cap, uint16_t *port) {
    *port = RELAY_DEFAULT_PORT;
    if (!s || !*s) return false;
    if (s[0] == '[') {
        const char *close = strchr(s, ']');
        if (!close || !copy_host(s + 1, (size_t)(close - s - 1), host, cap)) return false;
        if (close[1] == '\0') return true;
        return close[1] == ':' && parse_port(close + 2, port);
    }
    const char *colon = strchr(s, ':');
    if (colon && strchr(colon + 1, ':')) return copy_host(s, strlen(s), host, cap); /* bare IPv6 */
    if (!copy_host(s, colon ? (size_t)(colon - s) : strlen(s), host, cap)) return false;
    return !colon || parse_port(colon + 1, port);
}
