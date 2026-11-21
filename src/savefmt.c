/* .tuhh encoding, decoding and validation. See savefmt.h. */
#include "savefmt.h"

#include <math.h>
#include <sodium.h>
#include <stdlib.h>
#include <string.h>
#include <webp/decode.h>
#include <webp/encode.h>
#include <zstd.h>

static const uint8_t MAGIC[4] = { 'T', 'U', 'H', 'H' };

/* Key of the integrity tag. It ships with the game, so the tag detects corruption and
 * casual edits but does not prove who made a file (see the spec, "Integrity tag"). */
static const uint8_t TAG_KEY[32] = {
    0x3d, 0x56, 0xbc, 0xc9, 0xde, 0x4f, 0x53, 0x34, 0xb4, 0x6e, 0xd8, 0x90, 0x58, 0x20, 0x42, 0x86,
    0xc3, 0x9b, 0x4e, 0xa0, 0xb6, 0xd1, 0xcf, 0x91, 0xa5, 0x1b, 0x09, 0x85, 0x67, 0xb0, 0x27, 0x94,
};

#define ZSTD_LEVEL 19
#define WEBP_LOSSY_QUALITY 90

/* ---- growable output buffer --------------------------------------------------------- */

typedef struct {
    uint8_t *p;
    size_t len, cap;
    bool oom;
} Buf;

static void buf_reserve(Buf *b, size_t extra) {
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

static void put_bytes(Buf *b, const void *src, size_t n) {
    buf_reserve(b, n);
    if (b->oom) return;
    if (n) memcpy(b->p + b->len, src, n);
    b->len += n;
}

static void put_u8(Buf *b, uint8_t v) { put_bytes(b, &v, 1); }

static void put_u16(Buf *b, uint16_t v) {
    uint8_t t[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    put_bytes(b, t, 2);
}

static void put_u32(Buf *b, uint32_t v) {
    uint8_t t[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    put_bytes(b, t, 4);
}

/* unsigned LEB128 */
static void put_uv(Buf *b, uint32_t v) {
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
static void put_sv(Buf *b, int32_t v) { put_uv(b, ((uint32_t)v << 1) ^ (v < 0 ? 0xFFFFFFFFu : 0)); }

static void put_f64(Buf *b, double v) {
    uint64_t u;
    memcpy(&u, &v, 8);
    put_u32(b, (uint32_t)u);
    put_u32(b, (uint32_t)(u >> 32));
}

static void put_str(Buf *b, const char *s) {
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

static bool rd_need(Rd *r, size_t n) {
    if (r->bad || n > r->len - r->pos) {
        r->bad = true;
        return false;
    }
    return true;
}

static uint8_t get_u8(Rd *r) { return rd_need(r, 1) ? r->p[r->pos++] : 0; }

static uint16_t get_u16(Rd *r) {
    if (!rd_need(r, 2)) return 0;
    uint16_t v = (uint16_t)(r->p[r->pos] | r->p[r->pos + 1] << 8);
    r->pos += 2;
    return v;
}

static uint32_t get_u32(Rd *r) {
    if (!rd_need(r, 4)) return 0;
    const uint8_t *q = r->p + r->pos;
    r->pos += 4;
    return (uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24;
}

static uint32_t get_uv(Rd *r) {
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

static int32_t get_sv(Rd *r) {
    uint32_t u = get_uv(r);
    uint32_t x = (u >> 1) ^ (0u - (u & 1));
    int32_t v;
    memcpy(&v, &x, 4);
    return v;
}

static double get_f64(Rd *r) {
    uint64_t lo = get_u32(r), hi = get_u32(r);
    uint64_t u = lo | hi << 32;
    double v;
    memcpy(&v, &u, 8);
    return v;
}

static bool utf8_valid(const char *s, size_t n) {
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
static void get_str(Rd *r, char *out, size_t cap) {
    uint32_t n = get_uv(r);
    if (r->bad || n > SAVEFMT_MAX_STR || n >= cap || !rd_need(r, n) || !utf8_valid((const char *)r->p + r->pos, n)) {
        r->bad = true;
        out[0] = '\0';
        return;
    }
    memcpy(out, r->p + r->pos, n);
    out[n] = '\0';
    r->pos += n;
}

/* ---- validation (shared by encoder and decoder) ------------------------------------------ */

static bool str_ok(const char *s, size_t cap) {
    size_t n = strnlen(s, cap);
    return n < cap && n <= SAVEFMT_MAX_STR && utf8_valid(s, n);
}

static bool coord_ok(int32_t v) { return v >= -SAVEFMT_MAX_COORD && v <= SAVEFMT_MAX_COORD; }

/* encoding: images given as rgba may exceed SAVEFMT_MAX_DIM (they get scaled down) and
 * the total pixel limit is not checked (the pixels are already in memory). */
static bool validate(const SaveData *d, bool encoding) {
    if (d->nimages > SAVEFMT_MAX_IMAGES || d->nentities > SAVEFMT_MAX_ENTITIES ||
        d->ninit > d->nentities || d->nwaypoints > SAVEFMT_MAX_WAYPOINTS)
        return false;
    if (d->has_background && d->background >= d->nimages) return false;
    if (d->bg_rows < 1 || d->bg_rows > SAVEFMT_MAX_GRID || d->bg_cols < 1 || d->bg_cols > SAVEFMT_MAX_GRID) return false;
    if (d->offset_x < -SAVEFMT_MAX_OFFSET || d->offset_x > SAVEFMT_MAX_OFFSET ||
        d->offset_y < -SAVEFMT_MAX_OFFSET || d->offset_y > SAVEFMT_MAX_OFFSET)
        return false;
    if (d->hex_size < SAVEFMT_MIN_HEX || d->hex_size > SAVEFMT_MAX_HEX) return false;
    if (!str_ok(d->theme, sizeof(d->theme))) return false;

    uint64_t pixel_bytes = 0;
    for (uint32_t i = 0; i < d->nimages; i++) {
        const SaveImage *im = &d->images[i];
        int max = (encoding && !im->webp) ? 65535 : SAVEFMT_MAX_DIM;
        if (im->kind > SAVE_IMG_LOSSY || im->w < 1 || im->h < 1 || im->w > max || im->h > max) return false;
        if (encoding && !im->rgba && !im->webp) return false;
        pixel_bytes += (uint64_t)im->w * (uint64_t)im->h * 4;
    }
    if (!encoding && pixel_bytes > SAVEFMT_MAX_PIXEL_BYTES) return false;

    for (uint32_t i = 0; i < d->nentities; i++) {
        const SaveEntity *e = &d->entities[i];
        if (e->type > SAVE_ENT_WALL || e->image >= d->nimages || !coord_ok(e->row) || !coord_ok(e->col)) return false;
        if (e->type == SAVE_ENT_CHARACTER && e->speed > SAVEFMT_MAX_SPEED) return false;
    }

    if (d->ninit > 0) {
        uint8_t *seen = calloc(d->nentities, 1);
        if (!seen) return false;
        bool ok = true;
        for (uint32_t i = 0; i < d->ninit && ok; i++) {
            uint32_t e = d->init[i].entity;
            if (e >= d->nentities || seen[e] || !isfinite(d->init[i].value)) ok = false;
            else seen[e] = 1;
        }
        free(seen);
        if (!ok) return false;
    }
    if (d->ninit == 0 ? (d->current_turn != 0 || d->turn_started) : d->current_turn >= d->ninit) return false;

    for (uint32_t i = 0; i < d->nwaypoints; i++) {
        const SaveWaypoint *w = &d->waypoints[i];
        if (w->name[0] == '\0' || !str_ok(w->name, sizeof(w->name)) || !coord_ok(w->row) || !coord_ok(w->col))
            return false;
    }
    return true;
}

/* ---- state section ----------------------------------------------------------------------- */

/* fingerprint: camera and theme are left out (they don't make the table "modified") */
static void write_state(Buf *b, const SaveData *d, bool fingerprint) {
    put_u8(b, (uint8_t)((d->is_flat ? 1 : 0) | (d->has_background ? 2 : 0) |
                        (d->init_markers_visible ? 4 : 0) | (d->turn_started ? 8 : 0)));
    if (d->has_background) put_uv(b, d->background);
    put_uv(b, d->bg_rows);
    put_uv(b, d->bg_cols);
    put_sv(b, fingerprint ? 0 : d->offset_x);
    put_sv(b, fingerprint ? 0 : d->offset_y);
    put_uv(b, fingerprint ? 0 : d->hex_size);
    put_str(b, fingerprint ? "" : d->theme);

    put_uv(b, d->nentities);
    for (uint32_t i = 0; i < d->nentities; i++) {
        const SaveEntity *e = &d->entities[i];
        put_u8(b, e->type);
        put_uv(b, e->image);
        put_sv(b, e->row);
        put_sv(b, e->col);
        if (e->type == SAVE_ENT_CHARACTER) {
            put_sv(b, e->size);
            put_sv(b, e->max_health);
            put_sv(b, e->health);
            put_sv(b, e->attack);
            put_sv(b, e->ac);
            put_sv(b, e->speed);
            put_sv(b, e->initiative);
        }
    }

    put_uv(b, d->ninit);
    for (uint32_t i = 0; i < d->ninit; i++) {
        put_uv(b, d->init[i].entity);
        put_f64(b, d->init[i].value);
    }
    put_uv(b, d->current_turn);

    put_uv(b, d->nwaypoints);
    for (uint32_t i = 0; i < d->nwaypoints; i++) {
        put_str(b, d->waypoints[i].name);
        put_sv(b, d->waypoints[i].row);
        put_sv(b, d->waypoints[i].col);
    }
}

static SaveError read_state(const uint8_t *p, size_t n, SaveData *d) {
    Rd r = { p, n, 0, false };
    uint8_t flags = get_u8(&r);
    if (flags & ~15u) return SAVE_ERR_CORRUPT;
    d->is_flat = flags & 1;
    d->has_background = flags & 2;
    d->init_markers_visible = flags & 4;
    d->turn_started = flags & 8;
    if (d->has_background) d->background = get_uv(&r);
    d->bg_rows = get_uv(&r);
    d->bg_cols = get_uv(&r);
    d->offset_x = get_sv(&r);
    d->offset_y = get_sv(&r);
    d->hex_size = get_uv(&r);
    get_str(&r, d->theme, sizeof(d->theme));

    /* counts are checked before allocating: every record takes at least one byte */
    uint32_t ne = get_uv(&r);
    if (r.bad || ne > SAVEFMT_MAX_ENTITIES || ne > n - r.pos) return SAVE_ERR_CORRUPT;
    d->entities = calloc(ne ? ne : 1, sizeof(*d->entities));
    if (!d->entities) return SAVE_ERR_INTERNAL;
    d->nentities = ne;
    for (uint32_t i = 0; i < ne && !r.bad; i++) {
        SaveEntity *e = &d->entities[i];
        e->type = get_u8(&r);
        e->image = get_uv(&r);
        e->row = get_sv(&r);
        e->col = get_sv(&r);
        if (e->type == SAVE_ENT_CHARACTER) {
            e->size = get_sv(&r);
            e->max_health = get_sv(&r);
            e->health = get_sv(&r);
            e->attack = get_sv(&r);
            e->ac = get_sv(&r);
            e->speed = get_sv(&r);
            e->initiative = get_sv(&r);
        } else if (e->type != SAVE_ENT_WALL) {
            return SAVE_ERR_CORRUPT;
        }
    }

    uint32_t ni = get_uv(&r);
    if (r.bad || ni > ne) return SAVE_ERR_CORRUPT;
    d->init = calloc(ni ? ni : 1, sizeof(*d->init));
    if (!d->init) return SAVE_ERR_INTERNAL;
    d->ninit = ni;
    for (uint32_t i = 0; i < ni && !r.bad; i++) {
        d->init[i].entity = get_uv(&r);
        d->init[i].value = get_f64(&r);
    }
    d->current_turn = get_uv(&r);

    uint32_t nw = get_uv(&r);
    if (r.bad || nw > SAVEFMT_MAX_WAYPOINTS || nw > n - r.pos) return SAVE_ERR_CORRUPT;
    d->waypoints = calloc(nw ? nw : 1, sizeof(*d->waypoints));
    if (!d->waypoints) return SAVE_ERR_INTERNAL;
    d->nwaypoints = nw;
    for (uint32_t i = 0; i < nw && !r.bad; i++) {
        get_str(&r, d->waypoints[i].name, sizeof(d->waypoints[i].name));
        d->waypoints[i].row = get_sv(&r);
        d->waypoints[i].col = get_sv(&r);
    }

    if (r.bad || r.pos != n) return SAVE_ERR_CORRUPT; /* also rejects trailing bytes */
    return SAVE_OK;
}

/* ---- images ---------------------------------------------------------------------------------- */

static bool encode_webp(const SaveImage *im, SaveBlob *out) {
    WebPConfig cfg;
    if (im->kind == SAVE_IMG_LOSSY) {
        if (!WebPConfigPreset(&cfg, WEBP_PRESET_PICTURE, WEBP_LOSSY_QUALITY)) return false;
        cfg.method = 6;
    } else {
        if (!WebPConfigInit(&cfg) || !WebPConfigLosslessPreset(&cfg, 6)) return false;
        cfg.exact = 1; /* keep the colour of transparent pixels: pixel-exact round trip */
    }
    WebPPicture pic;
    if (!WebPPictureInit(&pic)) return false;
    pic.use_argb = 1;
    pic.width = im->w;
    pic.height = im->h;
    bool ok = WebPPictureImportRGBA(&pic, im->rgba, im->w * 4);
    if (ok && (pic.width > SAVEFMT_MAX_DIM || pic.height > SAVEFMT_MAX_DIM)) {
        double s = (double)SAVEFMT_MAX_DIM / (pic.width > pic.height ? pic.width : pic.height);
        int nw = (int)(pic.width * s), nh = (int)(pic.height * s);
        ok = WebPPictureRescale(&pic, nw < 1 ? 1 : nw, nh < 1 ? 1 : nh);
    }
    WebPMemoryWriter wr;
    WebPMemoryWriterInit(&wr);
    if (ok) {
        pic.writer = WebPMemoryWrite;
        pic.custom_ptr = &wr;
        ok = WebPEncode(&cfg, &pic);
    }
    out->w = pic.width;
    out->h = pic.height;
    WebPPictureFree(&pic);
    if (ok && wr.size > 0) {
        out->webp = malloc(wr.size);
        ok = out->webp != NULL;
        if (ok) {
            memcpy(out->webp, wr.mem, wr.size);
            out->len = wr.size;
        }
    } else {
        ok = false;
    }
    WebPMemoryWriterClear(&wr);
    return ok;
}

/* ---- public API ---------------------------------------------------------------------------- */

void savefmt_compute_tag(const uint8_t *buf, size_t len, uint8_t out[SAVEFMT_TAG_SIZE]) {
    crypto_generichash(out, SAVEFMT_TAG_SIZE, buf, len, TAG_KEY, sizeof(TAG_KEY));
}

void savefmt_blobs_free(SaveBlob *blobs, uint32_t n) {
    if (!blobs) return;
    for (uint32_t i = 0; i < n; i++) {
        free(blobs[i].webp);
        blobs[i] = (SaveBlob){ 0 };
    }
}

SaveError savefmt_encode(const SaveData *d, SaveBlob *blobs_out, uint8_t **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    if (blobs_out) memset(blobs_out, 0, sizeof(*blobs_out) * d->nimages);
    if (sodium_init() < 0) return SAVE_ERR_INTERNAL;
    if (!validate(d, true)) return SAVE_ERR_INVALID;

    Buf state = { 0 };
    write_state(&state, d, false);
    if (state.oom) { free(state.p); return SAVE_ERR_INTERNAL; }
    if (state.len > SAVEFMT_MAX_STATE) { free(state.p); return SAVE_ERR_TOO_LARGE; }

    SaveError err = SAVE_OK;
    Buf f = { 0 };
    put_bytes(&f, MAGIC, 4);
    put_u16(&f, SAVEFMT_VERSION);
    put_u16(&f, 0);
    put_u32(&f, 0); /* state_len, patched below */
    size_t bound = ZSTD_compressBound(state.len);
    buf_reserve(&f, bound);
    if (f.oom) {
        err = SAVE_ERR_INTERNAL;
        goto done;
    }
    size_t clen = ZSTD_compress(f.p + f.len, bound, state.p, state.len, ZSTD_LEVEL);
    if (ZSTD_isError(clen)) {
        err = SAVE_ERR_ENCODE;
        goto done;
    }
    for (int i = 0; i < 4; i++) f.p[8 + i] = (uint8_t)(clen >> (8 * i));
    f.len += clen;

    put_u32(&f, d->nimages);
    for (uint32_t i = 0; i < d->nimages && !f.oom; i++) {
        const SaveImage *im = &d->images[i];
        SaveBlob fresh = { 0 };
        const uint8_t *data = im->webp;
        size_t len = im->webp_len;
        int w = im->w, h = im->h, iw, ih;
        if (data) {
            if (!WebPGetInfo(data, len, &iw, &ih) || iw != w || ih != h) {
                err = SAVE_ERR_INVALID;
                goto done;
            }
        } else {
            if (!encode_webp(im, &fresh)) {
                free(fresh.webp);
                err = SAVE_ERR_ENCODE;
                goto done;
            }
            data = fresh.webp;
            len = fresh.len;
            w = fresh.w;
            h = fresh.h;
        }
        if (len > SAVEFMT_MAX_FILE || f.len + len > SAVEFMT_MAX_FILE) {
            free(fresh.webp);
            err = SAVE_ERR_TOO_LARGE;
            goto done;
        }
        put_u8(&f, im->kind);
        put_u16(&f, (uint16_t)w);
        put_u16(&f, (uint16_t)h);
        put_u32(&f, (uint32_t)len);
        put_bytes(&f, data, len);
        if (fresh.webp) {
            if (blobs_out) blobs_out[i] = fresh;
            else free(fresh.webp);
        }
    }

    buf_reserve(&f, SAVEFMT_TAG_SIZE);
    if (f.oom) {
        err = SAVE_ERR_INTERNAL;
        goto done;
    }
    savefmt_compute_tag(f.p, f.len, f.p + f.len);
    f.len += SAVEFMT_TAG_SIZE;
    if (f.len > SAVEFMT_MAX_FILE) err = SAVE_ERR_TOO_LARGE;

done:
    free(state.p);
    if (err != SAVE_OK) {
        free(f.p);
        return err;
    }
    *out = f.p;
    *out_len = f.len;
    return SAVE_OK;
}

static SaveError decode_images(Rd *r, SaveData *d) {
    uint32_t n = get_u32(r);
    if (r->bad || n > SAVEFMT_MAX_IMAGES) return SAVE_ERR_CORRUPT;
    d->images = calloc(n ? n : 1, sizeof(*d->images));
    if (!d->images) return SAVE_ERR_INTERNAL;
    d->nimages = n;
    uint64_t pixel_bytes = 0;
    for (uint32_t i = 0; i < n; i++) {
        SaveImage *im = &d->images[i];
        im->kind = get_u8(r);
        im->w = get_u16(r);
        im->h = get_u16(r);
        uint32_t len = get_u32(r);
        if (r->bad || im->kind > SAVE_IMG_LOSSY || im->w < 1 || im->h < 1 || im->w > SAVEFMT_MAX_DIM ||
            im->h > SAVEFMT_MAX_DIM || !rd_need(r, len))
            return SAVE_ERR_CORRUPT;
        size_t px = (size_t)im->w * (size_t)im->h * 4;
        pixel_bytes += px;
        if (pixel_bytes > SAVEFMT_MAX_PIXEL_BYTES) return SAVE_ERR_TOO_LARGE;
        const uint8_t *data = r->p + r->pos;
        r->pos += len;
        int w, h;
        if (!WebPGetInfo(data, len, &w, &h) || w != im->w || h != im->h) return SAVE_ERR_CORRUPT;
        uint8_t *rgba = malloc(px);
        uint8_t *webp = malloc(len ? len : 1);
        im->rgba = rgba;
        im->webp = webp;
        if (!rgba || !webp) return SAVE_ERR_INTERNAL;
        memcpy(webp, data, len);
        im->webp_len = len;
        if (!WebPDecodeRGBAInto(data, len, rgba, px, im->w * 4)) return SAVE_ERR_CORRUPT;
    }
    return SAVE_OK;
}

SaveError savefmt_decode(const uint8_t *buf, size_t len, SaveData *out) {
    memset(out, 0, sizeof(*out));
    out->owns_memory = true;
    if (len < 4 || memcmp(buf, MAGIC, 4) != 0) return SAVE_ERR_NOT_SAVE;
    if (len > SAVEFMT_MAX_FILE) return SAVE_ERR_TOO_LARGE;
    if (len < SAVEFMT_HEADER_SIZE + 4 + SAVEFMT_TAG_SIZE) return SAVE_ERR_CORRUPT;
    uint16_t version = (uint16_t)(buf[4] | buf[5] << 8);
    uint16_t flags = (uint16_t)(buf[6] | buf[7] << 8);
    if (version > SAVEFMT_VERSION) return SAVE_ERR_NEWER;
    if (version == 0 || flags != 0) return SAVE_ERR_CORRUPT;
    if (sodium_init() < 0) return SAVE_ERR_INTERNAL;

#ifndef SAVEFMT_FUZZING /* the fuzzer skips the tag so that it reaches the parser */
    uint8_t tag[SAVEFMT_TAG_SIZE];
    savefmt_compute_tag(buf, len - SAVEFMT_TAG_SIZE, tag);
    if (sodium_memcmp(tag, buf + len - SAVEFMT_TAG_SIZE, SAVEFMT_TAG_SIZE) != 0) return SAVE_ERR_CORRUPT;
#endif

    Rd r = { buf, len - SAVEFMT_TAG_SIZE, 8, false };
    uint32_t state_len = get_u32(&r);
    if (r.bad || !rd_need(&r, state_len)) return SAVE_ERR_CORRUPT;
    const uint8_t *z = buf + r.pos;
    r.pos += state_len;

    unsigned long long size = ZSTD_getFrameContentSize(z, state_len);
    if (size == ZSTD_CONTENTSIZE_UNKNOWN || size == ZSTD_CONTENTSIZE_ERROR) return SAVE_ERR_CORRUPT;
    if (size > SAVEFMT_MAX_STATE) return SAVE_ERR_TOO_LARGE;
    size_t frame = ZSTD_findFrameCompressedSize(z, state_len);
    if (ZSTD_isError(frame) || frame != state_len) return SAVE_ERR_CORRUPT; /* exactly one frame */

    uint8_t *state = malloc(size ? (size_t)size : 1);
    if (!state) return SAVE_ERR_INTERNAL;
    size_t got = ZSTD_decompress(state, (size_t)size, z, state_len);
    SaveError err = (ZSTD_isError(got) || got != size) ? SAVE_ERR_CORRUPT : read_state(state, (size_t)size, out);
    free(state);
    if (err == SAVE_OK) err = decode_images(&r, out);
    if (err == SAVE_OK && r.pos != r.len) err = SAVE_ERR_CORRUPT;
    if (err == SAVE_OK && !validate(out, false)) err = SAVE_ERR_CORRUPT;
    if (err != SAVE_OK) {
        savefmt_free(out);
        out->owns_memory = true;
    }
    return err;
}

void savefmt_free(SaveData *d) {
    if (!d) return;
    if (d->owns_memory) {
        for (uint32_t i = 0; d->images && i < d->nimages; i++) {
            free((void *)d->images[i].rgba);
            free((void *)d->images[i].webp);
        }
        free(d->images);
        free(d->entities);
        free(d->init);
        free(d->waypoints);
    }
    memset(d, 0, sizeof(*d));
}

bool savefmt_fingerprint(const SaveData *d, uint8_t out[SAVEFMT_TAG_SIZE]) {
    if (sodium_init() < 0) return false;
    Buf b = { 0 };
    write_state(&b, d, true);
    put_uv(&b, d->nimages);
    for (uint32_t i = 0; i < d->nimages; i++) {
        put_u8(&b, d->images[i].kind);
        put_u32(&b, (uint32_t)d->images[i].id);
        put_u32(&b, (uint32_t)(d->images[i].id >> 32));
    }
    bool ok = !b.oom;
    if (ok) crypto_generichash(out, SAVEFMT_TAG_SIZE, b.p, b.len, NULL, 0);
    free(b.p);
    return ok;
}

const char *savefmt_strerror(SaveError e) {
    switch (e) {
    case SAVE_OK:            return "OK";
    case SAVE_ERR_NOT_SAVE:  return "Not a Tales of Honor save file";
    case SAVE_ERR_NEWER:     return "Save file was made by a newer version of Tales of Honor";
    case SAVE_ERR_CORRUPT:   return "Save file is damaged or was modified";
    case SAVE_ERR_TOO_LARGE: return "Save file is too large";
    case SAVE_ERR_INVALID:   return "The table contains values that cannot be saved";
    case SAVE_ERR_ENCODE:    return "Could not encode the save file";
    case SAVE_ERR_INTERNAL:  return "Out of memory or internal error";
    }
    return "Unknown error";
}
