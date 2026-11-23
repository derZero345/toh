#ifndef TOH_WIRE_H
#define TOH_WIRE_H

/*
 * Byte buffer writer and bounds-checked reader shared by the save format (savefmt.c) and the
 * network messages (netmsg.c): little-endian integers, LEB128 / zig-zag varints, f64, UTF-8 strings.
 * Header-only (static inline), no dependencies besides libc.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---- growable output buffer --------------------------------------------------------- */

typedef struct {
    uint8_t *p;
    size_t len, cap;
    bool oom;
} Buf;

static inline void buf_reserve(Buf *b, size_t extra) {
    if (b->oom) return;
    if (extra > SIZE_MAX - b->len) { b->oom = true; return; }
    size_t need = b->len + extra;
    if (need <= b->cap) return;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < need) cap = cap > SIZE_MAX / 2 ? need : cap * 2;
    uint8_t *np = realloc(b->p, cap);
    if (!np) { b->oom = true; return; }
    b->p = np;
    b->cap = cap;
}

static inline void put_bytes(Buf *b, const void *src, size_t n) {
    buf_reserve(b, n);
    if (b->oom) return;
    if (n) memcpy(b->p + b->len, src, n);
    b->len += n;
}

static inline void put_u8(Buf *b, uint8_t v) { put_bytes(b, &v, 1); }

static inline void put_u16(Buf *b, uint16_t v) {
    uint8_t t[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    put_bytes(b, t, 2);
}

static inline void put_u32(Buf *b, uint32_t v) {
    uint8_t t[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    put_bytes(b, t, 4);
}

/* unsigned LEB128 */
static inline void put_uv(Buf *b, uint32_t v) {
    uint8_t t[5];
    int n = 0;
    do {
        uint8_t c = v & 0x7f;
        v >>= 7;
        if (v) c |= 0x80;
        t[n++] = c;
    } while (v);
    put_bytes(b, t, (size_t)n);
}

/* zig-zag: small negative numbers stay small */
static inline void put_sv(Buf *b, int32_t v) { put_uv(b, ((uint32_t)v << 1) ^ (v < 0 ? 0xFFFFFFFFu : 0)); }

static inline void put_f64(Buf *b, double v) {
    uint64_t u;
    memcpy(&u, &v, 8);
    put_u32(b, (uint32_t)u);
    put_u32(b, (uint32_t)(u >> 32));
}

static inline void put_str(Buf *b, const char *s) {
    size_t n = strlen(s);
    put_uv(b, (uint32_t)n);
    put_bytes(b, s, n);
}

/* ---- bounds-checked reader ------------------------------------------------------------ */

typedef struct {
    const uint8_t *p;
    size_t len, pos;
    bool bad; /* sticky: any failed read marks the whole input as bad */
} Rd;

static inline bool rd_need(Rd *r, size_t n) {
    if (r->bad || n > r->len - r->pos) {
        r->bad = true;
        return false;
    }
    return true;
}

static inline uint8_t get_u8(Rd *r) { return rd_need(r, 1) ? r->p[r->pos++] : 0; }

static inline uint16_t get_u16(Rd *r) {
    if (!rd_need(r, 2)) return 0;
    uint16_t v = (uint16_t)(r->p[r->pos] | r->p[r->pos + 1] << 8);
    r->pos += 2;
    return v;
}

static inline uint32_t get_u32(Rd *r) {
    if (!rd_need(r, 4)) return 0;
    const uint8_t *q = r->p + r->pos;
    r->pos += 4;
    return (uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24;
}

static inline uint32_t get_uv(Rd *r) {
    uint32_t v = 0;
    for (int i = 0; i < 5; i++) {
        uint8_t c = get_u8(r);
        if (r->bad) return 0;
        if (i == 4 && (c & 0xF0)) { /* more than 32 bits */
            r->bad = true;
            return 0;
        }
        v |= (uint32_t)(c & 0x7f) << (7 * i);
        if (!(c & 0x80)) return v;
    }
    return v; /* not reached */
}

static inline int32_t get_sv(Rd *r) {
    uint32_t u = get_uv(r);
    uint32_t x = (u >> 1) ^ (0u - (u & 1));
    int32_t v;
    memcpy(&v, &x, 4);
    return v;
}

static inline double get_f64(Rd *r) {
    uint64_t lo = get_u32(r), hi = get_u32(r);
    uint64_t u = lo | hi << 32;
    double v;
    memcpy(&v, &u, 8);
    return v;
}

static inline bool utf8_valid(const char *s, size_t n) {
    const uint8_t *p = (const uint8_t *)s;
    size_t i = 0;
    while (i < n) {
        uint8_t c = p[i];
        if (c == 0) return false;
        if (c < 0x80) { i++; continue; }
        size_t len;
        uint32_t cp, min;
        if ((c & 0xE0) == 0xC0)      { len = 2; cp = c & 0x1F; min = 0x80; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; min = 0x800; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; min = 0x10000; }
        else return false;
        if (len > n - i) return false;
        for (size_t k = 1; k < len; k++) {
            if ((p[i + k] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += len;
    }
    return true;
}

/* Reads a string into out[cap]; it must be shorter than cap and valid UTF-8. */
static inline void get_str(Rd *r, char *out, size_t cap) {
    uint32_t n = get_uv(r);
    if (r->bad || n >= cap || !rd_need(r, n) || !utf8_valid((const char *)r->p + r->pos, n)) {
        r->bad = true;
        out[0] = '\0';
        return;
    }
    memcpy(out, r->p + r->pos, n);
    out[n] = '\0';
    r->pos += n;
}

#endif
