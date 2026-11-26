/* .tuhh encoding, decoding and validation. See savefmt.h. */
#include "savefmt.h"
#include "wire.h"

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

/* Names shown to people: valid UTF-8 shorter than cap, without control, invisible or formatting
 * characters. spaces: plain spaces are allowed, but not a name of spaces only. */
static bool name_valid(const char *name, size_t cap, bool spaces) {
    if (!name) return false;
    size_t n = strnlen(name, cap);
    if (n == 0 || n >= cap || !utf8_valid(name, n)) return false;
    bool visible = false;
    for (size_t i = 0; i < n;) {
        uint8_t c = (uint8_t)name[i];
        uint32_t cp;
        size_t len;
        if (c < 0x80) { cp = c; len = 1; }
        else if (c < 0xE0) { cp = c & 0x1F; len = 2; }
        else if (c < 0xF0) { cp = c & 0x0F; len = 3; }
        else { cp = c & 0x07; len = 4; }
        for (size_t k = 1; k < len; k++) cp = (cp << 6) | ((uint8_t)name[i + k] & 0x3F);
        i += len;
        if (cp == 0x20 && spaces) continue;
        if (cp <= 0x20 || (cp >= 0x7F && cp <= 0xA0) || cp == 0xAD) return false;
        if ((cp >= 0x2000 && cp <= 0x200F) || (cp >= 0x2028 && cp <= 0x202F)) return false;
        if ((cp >= 0x205F && cp <= 0x206F) || cp == 0x3000 || cp == 0xFEFF) return false;
        visible = true;
    }
    return visible;
}

bool savefmt_char_name_valid(const char *name) { return name_valid(name, SAVEFMT_CHAR_NAME_MAX, true); }

bool savefmt_owner_valid(const char *name) { return name_valid(name, SAVEFMT_OWNER_MAX, false); }

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
        if (e->turn_used > SAVEFMT_MAX_TURN_USED || (e->turn_used && e->type != SAVE_ENT_CHARACTER)) return false;
        if (e->owner[0] != '\0' && (e->type != SAVE_ENT_CHARACTER || !savefmt_owner_valid(e->owner))) return false;
        if (e->name[0] != '\0' && (e->type != SAVE_ENT_CHARACTER || !savefmt_char_name_valid(e->name))) return false;
        if (e->party[0] != '\0' && (e->type != SAVE_ENT_CHARACTER || !savefmt_char_name_valid(e->party))) return false;
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
            put_sv(b, e->speed);
            put_sv(b, e->initiative);
            put_str(b, e->owner);
            put_str(b, e->name);
            put_sv(b, e->max_stamina);
            put_sv(b, e->stamina);
            put_str(b, e->party);
            put_u8(b, e->turn_used);
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

int32_t savefmt_size_from_v3(int32_t s) {
    if (s <= 0) return s + 1 < SAVEFMT_MIN_SIZE ? SAVEFMT_MIN_SIZE : s + 1; /* tiny, small, normal: one tile */
    return s == 1 ? 3 : 4; /* large (3 tiles) -> huge; huge (7) and gargantuan (12) -> gigantic */
}

static SaveError read_state(const uint8_t *p, size_t n, uint16_t version, SaveData *d) {
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
            if (version < 4) e->size = savefmt_size_from_v3(e->size);
            e->max_health = get_sv(&r);
            e->health = get_sv(&r);
            e->attack = get_sv(&r);
            if (version < 3) (void)get_sv(&r); /* armor class, no longer used */
            e->speed = get_sv(&r);
            e->initiative = get_sv(&r);
            if (version >= 2) get_str(&r, e->owner, sizeof(e->owner));
            if (version >= 5) get_str(&r, e->name, sizeof(e->name));
            if (version >= 6) {
                e->max_stamina = get_sv(&r);
                e->stamina = get_sv(&r);
            }
            if (version >= 7) get_str(&r, e->party, sizeof(e->party));
            if (version >= 8) e->turn_used = get_u8(&r);
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

bool savefmt_encode_image(const uint8_t *rgba, int w, int h, uint8_t kind, SaveBlob *out) {
    memset(out, 0, sizeof(*out));
    if (!rgba || w < 1 || h < 1 || w > 65535 || h > 65535 || kind > SAVE_IMG_LOSSY) return false;
    SaveImage im = { .kind = kind, .w = w, .h = h, .rgba = rgba };
    if (encode_webp(&im, out)) return true;
    free(out->webp);
    memset(out, 0, sizeof(*out));
    return false;
}

bool savefmt_decode_image(const uint8_t *webp, size_t len, int *w, int *h, uint8_t **rgba) {
    *rgba = NULL;
    if (!WebPGetInfo(webp, len, w, h) || *w < 1 || *h < 1 || *w > SAVEFMT_MAX_DIM || *h > SAVEFMT_MAX_DIM)
        return false;
    size_t px = (size_t)*w * (size_t)*h * 4;
    uint8_t *p = malloc(px);
    if (!p) return false;
    if (!WebPDecodeRGBAInto(webp, len, p, px, *w * 4)) {
        free(p);
        return false;
    }
    *rgba = p;
    return true;
}

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
    SaveError err = (ZSTD_isError(got) || got != size) ? SAVE_ERR_CORRUPT : read_state(state, (size_t)size, version, out);
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
