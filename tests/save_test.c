/* Unit tests for src/savefmt.c. Run with `make test`.
 * `build/save_test <file>` additionally writes a sample save (used as fuzzing seed). */
#include "savefmt.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <webp/encode.h>
#include <zstd.h>

static int failures, checks;

#define CHECK(cond) do {                                                       \
        checks++;                                                              \
        if (!(cond)) {                                                         \
            failures++;                                                        \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                      \
    } while (0)

#define CHECK_ERR(expr, want) do {                                             \
        SaveError got_ = (expr);                                               \
        checks++;                                                              \
        if (got_ != (want)) {                                                  \
            failures++;                                                        \
            fprintf(stderr, "%s:%d: %s = \"%s\", expected \"%s\"\n", __FILE__, __LINE__, #expr, \
                    savefmt_strerror(got_), savefmt_strerror(want));           \
        }                                                                      \
    } while (0)

/* ---- sample data ---------------------------------------------------------------------- */

/* Deterministic RGBA pattern with varied alpha, including transparent pixels with colour. */
static uint8_t *pattern(int w, int h, int seed) {
    uint8_t *p = malloc((size_t)w * h * 4);
    for (int i = 0; i < w * h; i++) {
        p[i * 4 + 0] = (uint8_t)(i * 7 + seed);
        p[i * 4 + 1] = (uint8_t)(i * 13 + seed * 3);
        p[i * 4 + 2] = (uint8_t)(i * 29);
        p[i * 4 + 3] = (uint8_t)((i % 5) * 60);
    }
    return p;
}

typedef struct {
    SaveData d;
    SaveImage images[2];
    SaveEntity entities[3];
    SaveInit init[2];
    SaveWaypoint waypoints[2];
    uint8_t *icon, *bg;
} Sample;

/* A table with a background, two characters sharing an icon, a wall, initiative and waypoints. */
static void sample_init(Sample *s) {
    memset(s, 0, sizeof(*s));
    s->icon = pattern(8, 8, 1);
    s->bg = pattern(64, 48, 2);
    s->images[0] = (SaveImage){ .kind = SAVE_IMG_LOSSY, .w = 64, .h = 48, .rgba = s->bg, .id = 101 };
    s->images[1] = (SaveImage){ .kind = SAVE_IMG_LOSSLESS, .w = 8, .h = 8, .rgba = s->icon, .id = 102 };
    s->entities[0] = (SaveEntity){ SAVE_ENT_CHARACTER, 1, 3, -4, 1, 30, 25, -2, 6, 12, "" };
    s->entities[1] = (SaveEntity){ SAVE_ENT_CHARACTER, 1, -100, 250, -2, 7, 7, 0, 1000, -3, "" };
    s->entities[2] = (SaveEntity){ .type = SAVE_ENT_WALL, .image = 1, .row = 5, .col = 5 };
    snprintf(s->entities[0].owner, SAVEFMT_OWNER_MAX, "Alice");
    s->init[0] = (SaveInit){ 1, 0.875 };
    s->init[1] = (SaveInit){ 0, 0.125 };
    snprintf(s->waypoints[0].name, SAVEFMT_NAME_MAX, "camp");
    s->waypoints[0].row = 2;
    s->waypoints[0].col = 3;
    snprintf(s->waypoints[1].name, SAVEFMT_NAME_MAX, "Höhle");
    s->waypoints[1].row = -7;
    s->waypoints[1].col = 0;

    SaveData *d = &s->d;
    d->is_flat = true;
    d->has_background = true;
    d->background = 0;
    d->init_markers_visible = true;
    d->turn_started = true;
    d->bg_rows = 20;
    d->bg_cols = 20;
    d->offset_x = -120;
    d->offset_y = 45;
    d->hex_size = 40;
    snprintf(d->theme, sizeof(d->theme), "dark");
    d->images = s->images;
    d->nimages = 2;
    d->entities = s->entities;
    d->nentities = 3;
    d->init = s->init;
    d->ninit = 2;
    d->current_turn = 1;
    d->waypoints = s->waypoints;
    d->nwaypoints = 2;
}

static void sample_free(Sample *s) {
    free(s->icon);
    free(s->bg);
}

static uint8_t *encode_sample(Sample *s, size_t *len) {
    uint8_t *file = NULL;
    CHECK_ERR(savefmt_encode(&s->d, NULL, &file, len), SAVE_OK);
    return file;
}

/* ---- tests ----------------------------------------------------------------------------- */

static void test_tag(void) {
    uint8_t a[SAVEFMT_TAG_SIZE], b[SAVEFMT_TAG_SIZE];
    uint8_t data[16] = "talesofhonor";
    savefmt_compute_tag(data, sizeof(data), a);
    savefmt_compute_tag(data, sizeof(data), b);
    CHECK(memcmp(a, b, sizeof(a)) == 0);
    data[3] ^= 1;
    savefmt_compute_tag(data, sizeof(data), b);
    CHECK(memcmp(a, b, sizeof(a)) != 0);
}

static void test_roundtrip(void) {
    Sample s;
    sample_init(&s);
    size_t len;
    uint8_t *file = encode_sample(&s, &len);
    CHECK(file && len > 4 && memcmp(file, "TUHH", 4) == 0);

    SaveData d;
    CHECK_ERR(savefmt_decode(file, len, &d), SAVE_OK);
    const SaveData *o = &s.d;
    CHECK(d.is_flat == o->is_flat && d.has_background && d.background == 0);
    CHECK(d.init_markers_visible && d.turn_started);
    CHECK(d.bg_rows == 20 && d.bg_cols == 20 && d.offset_x == -120 && d.offset_y == 45 && d.hex_size == 40);
    CHECK(strcmp(d.theme, "dark") == 0);
    CHECK(d.nentities == 3 && memcmp(d.entities, o->entities, sizeof(s.entities)) == 0);
    CHECK(strcmp(d.entities[0].owner, "Alice") == 0 && d.entities[1].owner[0] == '\0');
    CHECK(d.ninit == 2 && d.init[0].entity == 1 && d.init[0].value == 0.875 && d.init[1].value == 0.125);
    CHECK(d.current_turn == 1);
    CHECK(d.nwaypoints == 2 && strcmp(d.waypoints[1].name, "Höhle") == 0 && d.waypoints[1].row == -7);
    CHECK(d.nimages == 2);
    /* lossless icon: pixel exact, lossy background: right size */
    CHECK(d.images[1].kind == SAVE_IMG_LOSSLESS && d.images[1].w == 8 && d.images[1].h == 8);
    CHECK(memcmp(d.images[1].rgba, s.icon, 8 * 8 * 4) == 0);
    CHECK(d.images[0].kind == SAVE_IMG_LOSSY && d.images[0].w == 64 && d.images[0].h == 48);
    CHECK(d.images[0].rgba && d.images[0].webp && d.images[0].webp_len > 0);
    savefmt_free(&d);
    free(file);
    sample_free(&s);
}

static void test_empty_table(void) {
    SaveData d = { .bg_rows = 20, .bg_cols = 20, .hex_size = 40, .init_markers_visible = true };
    snprintf(d.theme, sizeof(d.theme), "light");
    uint8_t *file;
    size_t len;
    CHECK_ERR(savefmt_encode(&d, NULL, &file, &len), SAVE_OK);
    SaveData o;
    CHECK_ERR(savefmt_decode(file, len, &o), SAVE_OK);
    CHECK(o.nentities == 0 && o.nimages == 0 && !o.has_background);
    savefmt_free(&o);
    free(file);
}

/* Images encoded once are handed back; re-saving with them reproduces the file exactly. */
static void test_blobs(void) {
    Sample s;
    sample_init(&s);
    SaveBlob blobs[2];
    uint8_t *a, *b;
    size_t alen, blen;
    CHECK_ERR(savefmt_encode(&s.d, blobs, &a, &alen), SAVE_OK);
    CHECK(blobs[0].webp && blobs[0].w == 64 && blobs[0].h == 48);
    CHECK(blobs[1].webp && blobs[1].w == 8 && blobs[1].h == 8);
    for (int i = 0; i < 2; i++) {
        s.images[i].rgba = NULL;
        s.images[i].webp = blobs[i].webp;
        s.images[i].webp_len = blobs[i].len;
    }
    SaveBlob again[2];
    CHECK_ERR(savefmt_encode(&s.d, again, &b, &blen), SAVE_OK);
    CHECK(again[0].webp == NULL && again[1].webp == NULL);
    CHECK(alen == blen && memcmp(a, b, alen) == 0);
    savefmt_blobs_free(blobs, 2);
    free(a);
    free(b);
    sample_free(&s);
}

/* Images over WebP's 16383 px limit are scaled down. */
static void test_scale_down(void) {
    SaveData d = { .bg_rows = 20, .bg_cols = 20, .hex_size = 40, .has_background = true };
    snprintf(d.theme, sizeof(d.theme), "light");
    uint8_t *px = pattern(16400, 2, 3);
    SaveImage im = { .kind = SAVE_IMG_LOSSY, .w = 16400, .h = 2, .rgba = px };
    d.images = &im;
    d.nimages = 1;
    SaveBlob blob;
    uint8_t *file;
    size_t len;
    CHECK_ERR(savefmt_encode(&d, &blob, &file, &len), SAVE_OK);
    CHECK(blob.w == 16383 && blob.h == 1);
    SaveData o;
    CHECK_ERR(savefmt_decode(file, len, &o), SAVE_OK);
    CHECK(o.nimages == 1 && o.images[0].w == 16383 && o.images[0].h == 1);
    savefmt_free(&o);
    savefmt_blobs_free(&blob, 1);
    free(file);
    free(px);
}

static void test_every_byte_flip_rejected(void) {
    Sample s;
    sample_init(&s);
    size_t len;
    uint8_t *file = encode_sample(&s, &len);
    int accepted = 0, wrong_magic_error = 0;
    for (size_t i = 0; i < len; i++) {
        for (int bit = 0; bit < 8; bit += 3) {
            file[i] ^= (uint8_t)(1 << bit);
            SaveData d;
            SaveError e = savefmt_decode(file, len, &d);
            if (e == SAVE_OK) accepted++;
            if (i < 4 && e != SAVE_ERR_NOT_SAVE) wrong_magic_error++;
            savefmt_free(&d);
            file[i] ^= (uint8_t)(1 << bit);
        }
    }
    CHECK(accepted == 0);
    CHECK(wrong_magic_error == 0);
    free(file);
    sample_free(&s);
}

static void test_every_truncation_rejected(void) {
    Sample s;
    sample_init(&s);
    size_t len;
    uint8_t *file = encode_sample(&s, &len);
    int accepted = 0;
    for (size_t n = 0; n < len; n++) {
        SaveData d;
        if (savefmt_decode(file, n, &d) == SAVE_OK) accepted++;
        savefmt_free(&d);
    }
    CHECK(accepted == 0);
    /* appended bytes too */
    uint8_t *longer = malloc(len + 1);
    memcpy(longer, file, len);
    longer[len] = 0;
    SaveData d;
    CHECK_ERR(savefmt_decode(longer, len + 1, &d), SAVE_ERR_CORRUPT);
    free(longer);
    free(file);
    sample_free(&s);
}

static void retag(uint8_t *file, size_t len) {
    savefmt_compute_tag(file, len - SAVEFMT_TAG_SIZE, file + len - SAVEFMT_TAG_SIZE);
}

static void test_header_errors(void) {
    Sample s;
    sample_init(&s);
    size_t len;
    uint8_t *file = encode_sample(&s, &len);
    SaveData d;

    CHECK_ERR(savefmt_decode((const uint8_t *)"PNG\x89 not a save", 16, &d), SAVE_ERR_NOT_SAVE);
    CHECK_ERR(savefmt_decode(file, 2, &d), SAVE_ERR_NOT_SAVE);

    file[4] = SAVEFMT_VERSION + 1; /* from the future */
    retag(file, len);
    CHECK_ERR(savefmt_decode(file, len, &d), SAVE_ERR_NEWER);
    file[4] = 0; /* version 0 */
    retag(file, len);
    CHECK_ERR(savefmt_decode(file, len, &d), SAVE_ERR_CORRUPT);
    file[4] = SAVEFMT_VERSION;
    file[6] = 1; /* signature flag: reserved */
    retag(file, len);
    CHECK_ERR(savefmt_decode(file, len, &d), SAVE_ERR_CORRUPT);
    file[6] = 0;
    retag(file, len);
    CHECK_ERR(savefmt_decode(file, len, &d), SAVE_OK);
    savefmt_free(&d);
    free(file);
    sample_free(&s);
}

/* ---- hand-built files with a valid tag (the parser must not trust the tag) ----------------- */

typedef struct { uint8_t b[8192]; size_t n; } W;

static void w8(W *w, uint8_t v) { w->b[w->n++] = v; }
static void w32(W *w, uint32_t v) { for (int i = 0; i < 4; i++) w8(w, (uint8_t)(v >> (8 * i))); }
static void wuv(W *w, uint32_t v) {
    do {
        uint8_t c = v & 0x7f;
        v >>= 7;
        w8(w, (uint8_t)(c | (v ? 0x80 : 0)));
    } while (v);
}
static void wsv(W *w, int32_t v) { wuv(w, ((uint32_t)v << 1) ^ (v < 0 ? 0xFFFFFFFFu : 0)); }
static void wf64(W *w, double v) { uint64_t u; memcpy(&u, &v, 8); w32(w, (uint32_t)u); w32(w, (uint32_t)(u >> 32)); }
static void wbytes(W *w, const void *p, size_t n) { memcpy(w->b + w->n, p, n); w->n += n; }
static void wstr(W *w, const char *s) { wuv(w, (uint32_t)strlen(s)); wbytes(w, s, strlen(s)); }

/* world section of a valid table: pointy grid, no background */
static void wworld(W *w, uint8_t flags) {
    w8(w, flags);
    wuv(w, 20);
    wuv(w, 20);
    wsv(w, 0);
    wsv(w, 0);
    wuv(w, 40);
    wstr(w, "light");
}

static void wwall(W *w, uint32_t image) { w8(w, SAVE_ENT_WALL); wuv(w, image); wsv(w, 1); wsv(w, 2); }

/* Wraps a raw state section and raw image section (count + records) into a tagged file. */
static uint8_t *wrap_v(const W *state, const W *images, size_t *len, int content_size_flag, uint8_t version) {
    ZSTD_CCtx *cc = ZSTD_createCCtx();
    ZSTD_CCtx_setParameter(cc, ZSTD_c_contentSizeFlag, content_size_flag);
    size_t bound = ZSTD_compressBound(state->n);
    uint8_t *z = malloc(bound);
    size_t zn = ZSTD_compress2(cc, z, bound, state->b, state->n);
    ZSTD_freeCCtx(cc);
    size_t total = SAVEFMT_HEADER_SIZE + zn + images->n + SAVEFMT_TAG_SIZE;
    uint8_t *f = malloc(total);
    memcpy(f, "TUHH\x01\x00\x00\x00", 8);
    f[4] = version;
    for (int i = 0; i < 4; i++) f[8 + i] = (uint8_t)(zn >> (8 * i));
    memcpy(f + 12, z, zn);
    memcpy(f + 12 + zn, images->b, images->n);
    retag(f, total);
    free(z);
    *len = total;
    return f;
}

static uint8_t *wrap(const W *state, const W *images, size_t *len, int content_size_flag) {
    return wrap_v(state, images, len, content_size_flag, 1);
}

static SaveError decode_raw(const W *state, const W *images, int content_size_flag) {
    size_t len;
    uint8_t *f = wrap(state, images, &len, content_size_flag);
    SaveData d;
    SaveError e = savefmt_decode(f, len, &d);
    savefmt_free(&d);
    free(f);
    return e;
}

/* image section with one lossless 8x8 image, declared as w x h */
static void wimage(W *w, int decl_w, int decl_h) {
    uint8_t *px = pattern(8, 8, 9);
    uint8_t *webp = NULL;
    size_t n = WebPEncodeLosslessRGBA(px, 8, 8, 32, &webp);
    w32(w, 1);
    w8(w, SAVE_IMG_LOSSLESS);
    w8(w, (uint8_t)decl_w); w8(w, 0);
    w8(w, (uint8_t)decl_h); w8(w, 0);
    w32(w, (uint32_t)n);
    wbytes(w, webp, n);
    WebPFree(webp);
    free(px);
}

static void test_semantic_validation(void) {
    W st, im;

    /* baseline: one wall using image 0, one initiative entry, one waypoint -> OK */
    st.n = 0; im.n = 0;
    wworld(&st, 4);
    wuv(&st, 1); wwall(&st, 0);
    wuv(&st, 1); wuv(&st, 0); wf64(&st, 0.5);
    wuv(&st, 0);
    wuv(&st, 1); wstr(&st, "camp"); wsv(&st, 0); wsv(&st, 0);
    wimage(&im, 8, 8);
    CHECK_ERR(decode_raw(&st, &im, 1), SAVE_OK);

    /* zstd frame without a content size */
    CHECK_ERR(decode_raw(&st, &im, 0), SAVE_ERR_CORRUPT);

    /* declared image size differs from the WebP data */
    W im2 = { .n = 0 };
    wimage(&im2, 9, 8);
    CHECK_ERR(decode_raw(&st, &im2, 1), SAVE_ERR_CORRUPT);

    /* trailing byte in the state section */
    W st2 = st;
    w8(&st2, 0);
    CHECK_ERR(decode_raw(&st2, &im, 1), SAVE_ERR_CORRUPT);

    W noimg = { .n = 0 };
    w32(&noimg, 0);

    /* entity refers to a missing image */
    st.n = 0;
    wworld(&st, 0);
    wuv(&st, 1); wwall(&st, 0);
    wuv(&st, 0); wuv(&st, 0); wuv(&st, 0);
    CHECK_ERR(decode_raw(&st, &noimg, 1), SAVE_ERR_CORRUPT);

    /* initiative refers to a missing entity */
    st.n = 0;
    wworld(&st, 0);
    wuv(&st, 1); wwall(&st, 0);
    wuv(&st, 1); wuv(&st, 5); wf64(&st, 0.5);
    wuv(&st, 0); wuv(&st, 0);
    CHECK_ERR(decode_raw(&st, &im, 1), SAVE_ERR_CORRUPT);

    /* same entity twice in the initiative */
    st.n = 0;
    wworld(&st, 0);
    wuv(&st, 2); wwall(&st, 0); wwall(&st, 0);
    wuv(&st, 2); wuv(&st, 1); wf64(&st, 0.5); wuv(&st, 1); wf64(&st, 0.25);
    wuv(&st, 0); wuv(&st, 0);
    CHECK_ERR(decode_raw(&st, &im, 1), SAVE_ERR_CORRUPT);

    /* initiative value NaN */
    st.n = 0;
    wworld(&st, 0);
    wuv(&st, 1); wwall(&st, 0);
    wuv(&st, 1); wuv(&st, 0); wf64(&st, NAN);
    wuv(&st, 0); wuv(&st, 0);
    CHECK_ERR(decode_raw(&st, &im, 1), SAVE_ERR_CORRUPT);

    /* current turn out of range */
    st.n = 0;
    wworld(&st, 8);
    wuv(&st, 1); wwall(&st, 0);
    wuv(&st, 1); wuv(&st, 0); wf64(&st, 0.5);
    wuv(&st, 1); wuv(&st, 0);
    CHECK_ERR(decode_raw(&st, &im, 1), SAVE_ERR_CORRUPT);

    /* absurd entity count without the data */
    st.n = 0;
    wworld(&st, 0);
    wuv(&st, 100001);
    CHECK_ERR(decode_raw(&st, &noimg, 1), SAVE_ERR_CORRUPT);

    /* waypoint name that is not UTF-8 */
    st.n = 0;
    wworld(&st, 0);
    wuv(&st, 0); wuv(&st, 0); wuv(&st, 0);
    wuv(&st, 1); wstr(&st, "\xC3\x28"); wsv(&st, 0); wsv(&st, 0);
    CHECK_ERR(decode_raw(&st, &noimg, 1), SAVE_ERR_CORRUPT);

    /* waypoint coordinates out of range */
    st.n = 0;
    wworld(&st, 0);
    wuv(&st, 0); wuv(&st, 0); wuv(&st, 0);
    wuv(&st, 1); wstr(&st, "far"); wsv(&st, SAVEFMT_MAX_COORD + 1); wsv(&st, 0);
    CHECK_ERR(decode_raw(&st, &noimg, 1), SAVE_ERR_CORRUPT);

    /* hexSize 0 */
    st.n = 0;
    w8(&st, 0); wuv(&st, 20); wuv(&st, 20); wsv(&st, 0); wsv(&st, 0); wuv(&st, 0); wstr(&st, "light");
    wuv(&st, 0); wuv(&st, 0); wuv(&st, 0); wuv(&st, 0);
    CHECK_ERR(decode_raw(&st, &noimg, 1), SAVE_ERR_CORRUPT);

    /* unknown world flag bit */
    st.n = 0;
    wworld(&st, 0x10);
    wuv(&st, 0); wuv(&st, 0); wuv(&st, 0); wuv(&st, 0);
    CHECK_ERR(decode_raw(&st, &noimg, 1), SAVE_ERR_CORRUPT);

    /* varint longer than 32 bits */
    st.n = 0;
    wworld(&st, 0);
    w8(&st, 0xFF); w8(&st, 0xFF); w8(&st, 0xFF); w8(&st, 0xFF); w8(&st, 0x7F);
    CHECK_ERR(decode_raw(&st, &noimg, 1), SAVE_ERR_CORRUPT);

    /* image section claims more data than there is */
    st.n = 0;
    wworld(&st, 0);
    wuv(&st, 0); wuv(&st, 0); wuv(&st, 0); wuv(&st, 0);
    W bigimg = { .n = 0 };
    w32(&bigimg, 1); w8(&bigimg, 0); w8(&bigimg, 8); w8(&bigimg, 0); w8(&bigimg, 8); w8(&bigimg, 0);
    w32(&bigimg, 0x7FFFFFFF);
    CHECK_ERR(decode_raw(&st, &bigimg, 1), SAVE_ERR_CORRUPT);
}

static void test_encoder_validates(void) {
    Sample s;
    sample_init(&s);
    uint8_t *file;
    size_t len;
    s.waypoints[0].row = SAVEFMT_MAX_COORD + 1;
    CHECK_ERR(savefmt_encode(&s.d, NULL, &file, &len), SAVE_ERR_INVALID);
    CHECK(file == NULL);
    s.waypoints[0].row = 0;
    s.entities[0].image = 7;
    CHECK_ERR(savefmt_encode(&s.d, NULL, &file, &len), SAVE_ERR_INVALID);
    s.entities[0].image = 1;
    s.images[1].rgba = NULL; /* no pixels and no WebP data */
    CHECK_ERR(savefmt_encode(&s.d, NULL, &file, &len), SAVE_ERR_INVALID);
    sample_free(&s);
}

static void test_fingerprint(void) {
    Sample s;
    sample_init(&s);
    uint8_t a[SAVEFMT_TAG_SIZE], b[SAVEFMT_TAG_SIZE];
    CHECK(savefmt_fingerprint(&s.d, a));

    /* camera and theme don't count */
    s.d.offset_x += 500;
    s.d.hex_size = 80;
    snprintf(s.d.theme, sizeof(s.d.theme), "red");
    CHECK(savefmt_fingerprint(&s.d, b));
    CHECK(memcmp(a, b, sizeof(a)) == 0);

    /* moving an entity does */
    s.entities[2].row++;
    CHECK(savefmt_fingerprint(&s.d, b));
    CHECK(memcmp(a, b, sizeof(a)) != 0);
    s.entities[2].row--;

    /* and so does a new owner */
    snprintf(s.entities[1].owner, SAVEFMT_OWNER_MAX, "Bob");
    CHECK(savefmt_fingerprint(&s.d, b));
    CHECK(memcmp(a, b, sizeof(a)) != 0);
    s.entities[1].owner[0] = '\0';

    /* so does a different image (new texture id) */
    s.images[0].id = 999;
    CHECK(savefmt_fingerprint(&s.d, b));
    CHECK(memcmp(a, b, sizeof(a)) != 0);
    sample_free(&s);
}

static void test_owner_names(void) {
    CHECK(savefmt_owner_valid("Alice"));
    CHECK(savefmt_owner_valid("Jörg_42"));
    CHECK(savefmt_owner_valid("abcdefghijklmnopqrstuvwx"));   /* 24 bytes */
    CHECK(!savefmt_owner_valid("abcdefghijklmnopqrstuvwxy")); /* 25 bytes */
    CHECK(!savefmt_owner_valid(""));
    CHECK(!savefmt_owner_valid("two words"));
    CHECK(!savefmt_owner_valid("tab\there"));
    CHECK(!savefmt_owner_valid("\xc3")); /* broken UTF-8 */
    CHECK(!savefmt_owner_valid("a\xc2\xa0" "b"));     /* U+00A0 */
    CHECK(!savefmt_owner_valid("a\xe2\x80\x8b" "b")); /* U+200B */
    CHECK(!savefmt_owner_valid("a\xe2\x80\xae" "b")); /* U+202E */
    CHECK(!savefmt_owner_valid("\xef\xbb\xbf" "Alice")); /* U+FEFF */
    CHECK(!savefmt_owner_valid("\x7f"));
    CHECK(savefmt_owner_valid("\xe5\x90\x8d\xe5\x89\x8d")); /* CJK, accepted */
    CHECK(!savefmt_owner_valid(NULL));

    Sample s;
    sample_init(&s);
    uint8_t *file = NULL;
    size_t len;
    snprintf(s.entities[0].owner, SAVEFMT_OWNER_MAX, "bad name");
    CHECK_ERR(savefmt_encode(&s.d, NULL, &file, &len), SAVE_ERR_INVALID);
    s.entities[0].owner[0] = '\0';
    snprintf(s.entities[2].owner, SAVEFMT_OWNER_MAX, "Alice"); /* walls have no owner */
    CHECK_ERR(savefmt_encode(&s.d, NULL, &file, &len), SAVE_ERR_INVALID);
    sample_free(&s);
}

/* One character record in the layout of a version: size 1, max health 2, health 3, attack 4, speed 6,
 * initiative 7; versions 1 and 2 also store an armor class (5) after attack, version 2 on an owner string. */
static void wcharacter(W *w, int version, const char *owner) {
    w8(w, SAVE_ENT_CHARACTER);
    wuv(w, 0);
    wsv(w, 1);
    wsv(w, 2);
    for (int v = 1; v <= 7; v++)
        if (v != 5 || version < 3) wsv(w, v);
    if (version >= 2) wstr(w, owner);
}

static void test_owner_versions(void) {
    W st[5] = { { .n = 0 } }, im = { .n = 0 };
    wimage(&im, 8, 8);
    /* body version, owner: v1, v2, v3, a bad owner, and a v1 body to be wrapped as version 2 */
    const int body[5] = { 1, 2, 3, 3, 1 };
    const char *owners[5] = { NULL, "Alice", "Bob", "A B", NULL };
    for (int i = 0; i < 5; i++) {
        wworld(&st[i], 0);
        wuv(&st[i], 1);
        wcharacter(&st[i], body[i], owners[i]);
        wuv(&st[i], 0); wuv(&st[i], 0); /* no initiative, current turn 0 */
        wuv(&st[i], 0);                  /* no waypoints */
    }
    SaveData d;
    size_t len;
    const char *want[4] = { "", "Alice", "Bob", "Bob" };
    for (int v = 1; v <= 4; v++) { /* every version loads; the old armor class is skipped */
        uint8_t *f = wrap_v(&st[v < 3 ? v - 1 : 2], &im, &len, 1, (uint8_t)v); /* v4 has v3's layout */
        CHECK_ERR(savefmt_decode(f, len, &d), SAVE_OK);
        const SaveEntity *e = d.nentities == 1 ? &d.entities[0] : NULL;
        int size = v < 4 ? 3 : 1; /* before v4, size 1 was large (3 tiles): now Huge */
        CHECK(e && strcmp(e->owner, want[v - 1]) == 0 && e->size == size && e->max_health == 2 && e->health == 3 &&
              e->attack == 4 && e->speed == 6 && e->initiative == 7);
        savefmt_free(&d);
        free(f);
    }

    /* a version 1 body in a version 2 file lacks the owner string */
    uint8_t *f = wrap_v(&st[4], &im, &len, 1, 2);
    CHECK_ERR(savefmt_decode(f, len, &d), SAVE_ERR_CORRUPT);
    savefmt_free(&d);
    free(f);

    /* owner names follow savefmt_owner_valid */
    f = wrap_v(&st[3], &im, &len, 1, 3);
    CHECK_ERR(savefmt_decode(f, len, &d), SAVE_ERR_CORRUPT);
    savefmt_free(&d);
    free(f);
}

/* version 5 stores character names after the owner */
static void test_char_names(void) {
    CHECK(savefmt_char_name_valid("Mao Clausewitz") && savefmt_char_name_valid("\xc3\x96laf"));
    CHECK(!savefmt_char_name_valid("") && !savefmt_char_name_valid("tab\there") &&
          !savefmt_char_name_valid("\xc2\x85") && !savefmt_char_name_valid("\xff"));
    CHECK(savefmt_char_name_valid("12345678901234567890123456789012"));   /* 32 bytes */
    CHECK(!savefmt_char_name_valid("123456789012345678901234567890123")); /* 33 */
    CHECK(!savefmt_char_name_valid("   ") && !savefmt_char_name_valid("a\xe2\x80\x8b") && /* spaces only, U+200B */
          !savefmt_char_name_valid("\xe2\x80\xae") && !savefmt_char_name_valid("\xef\xbb\xbf")); /* U+202E, U+FEFF */
    CHECK(savefmt_char_name_valid(" Mao ") && !savefmt_owner_valid("Mao C")); /* players' names have no spaces */

    Sample s;
    sample_init(&s);
    snprintf(s.entities[0].name, SAVEFMT_CHAR_NAME_MAX, "Mao Clausewitz");
    size_t len;
    uint8_t *file = encode_sample(&s, &len);
    SaveData d;
    CHECK_ERR(savefmt_decode(file, len, &d), SAVE_OK);
    CHECK(d.nentities == 3 && strcmp(d.entities[0].name, "Mao Clausewitz") == 0 && d.entities[1].name[0] == '\0');
    savefmt_free(&d);
    free(file);

    /* bad names don't encode */
    snprintf(s.entities[0].name, SAVEFMT_CHAR_NAME_MAX, "bad\x01");
    uint8_t *out;
    CHECK_ERR(savefmt_encode(&s.d, NULL, &out, &len), SAVE_ERR_INVALID);
    /* walls have no name */
    s.entities[0].name[0] = '\0';
    snprintf(s.entities[2].name, SAVEFMT_CHAR_NAME_MAX, "wall");
    CHECK_ERR(savefmt_encode(&s.d, NULL, &out, &len), SAVE_ERR_INVALID);
    sample_free(&s);

    /* a version 4 body loads without names */
    W st = { .n = 0 }, im = { .n = 0 };
    wimage(&im, 8, 8);
    wworld(&st, 0);
    wuv(&st, 1);
    wcharacter(&st, 3, "Bob");
    wuv(&st, 0); wuv(&st, 0);
    wuv(&st, 0);
    uint8_t *f = wrap_v(&st, &im, &len, 1, 4);
    CHECK_ERR(savefmt_decode(f, len, &d), SAVE_OK);
    CHECK(d.nentities == 1 && d.entities[0].name[0] == '\0' && strcmp(d.entities[0].owner, "Bob") == 0);
    savefmt_free(&d);
    free(f);
}

/* version 6 stores max and current stamina after the name */
static void test_stamina_versions(void) {
    Sample s;
    sample_init(&s);
    s.entities[0].max_stamina = 9;
    s.entities[0].stamina = -2;
    size_t len;
    uint8_t *file = encode_sample(&s, &len);
    SaveData d;
    CHECK_ERR(savefmt_decode(file, len, &d), SAVE_OK);
    CHECK(d.nentities == 3 && d.entities[0].max_stamina == 9 && d.entities[0].stamina == -2 &&
          d.entities[1].max_stamina == 0);
    savefmt_free(&d);
    free(file);
    sample_free(&s);

    /* a version 5 body (v3 layout + name) loads with no stamina */
    W st = { .n = 0 }, im = { .n = 0 };
    wimage(&im, 8, 8);
    wworld(&st, 0);
    wuv(&st, 1);
    wcharacter(&st, 3, "Bob");
    wstr(&st, "Mao");
    wuv(&st, 0); wuv(&st, 0);
    wuv(&st, 0);
    uint8_t *f = wrap_v(&st, &im, &len, 1, 5);
    CHECK_ERR(savefmt_decode(f, len, &d), SAVE_OK);
    CHECK(d.nentities == 1 && strcmp(d.entities[0].name, "Mao") == 0 && d.entities[0].max_stamina == 0 &&
          d.entities[0].stamina == 0);
    savefmt_free(&d);
    free(f);
}

/* sizes of version 1-3 saves map to Tales of Honor's classes by footprint */
static void test_size_conversion(void) {
    const int32_t from[] = { -5, -2, -1, 0, 1, 2, 3, 9 }, to[] = { -2, -1, 0, 1, 3, 4, 4, 4 };
    for (int i = 0; i < 8; i++) CHECK(savefmt_size_from_v3(from[i]) == to[i]);
}

static int write_sample(const char *path) {
    Sample s;
    sample_init(&s);
    size_t len;
    uint8_t *file = encode_sample(&s, &len);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(file, 1, len, f) != len) {
        perror(path);
        return 1;
    }
    fclose(f);
    free(file);
    sample_free(&s);
    return 0;
}

static void test_single_image(void) {
    uint8_t *px = pattern(9, 7, 4);
    SaveBlob b;
    CHECK(savefmt_encode_image(px, 9, 7, SAVE_IMG_LOSSLESS, &b) && b.webp && b.w == 9 && b.h == 7);
    int w, h;
    uint8_t *back;
    CHECK(savefmt_decode_image(b.webp, b.len, &w, &h, &back) && w == 9 && h == 7);
    CHECK(back && memcmp(back, px, 9 * 7 * 4) == 0); /* lossless is pixel-exact */
    free(back);
    CHECK(!savefmt_decode_image(b.webp, b.len / 2, &w, &h, &back) && back == NULL);
    free(b.webp);
    CHECK(!savefmt_encode_image(px, 0, 7, SAVE_IMG_LOSSLESS, &b) && b.webp == NULL);
    CHECK(!savefmt_encode_image(px, 9, 7, 7, &b));
    CHECK(savefmt_encode_image(px, 9, 7, SAVE_IMG_LOSSY, &b) && b.webp);
    free(b.webp);
    free(px);
}

int main(int argc, char **argv) {
    if (argc == 2) return write_sample(argv[1]);
    test_tag();
    test_single_image();
    test_roundtrip();
    test_empty_table();
    test_blobs();
    test_scale_down();
    test_every_byte_flip_rejected();
    test_every_truncation_rejected();
    test_header_errors();
    test_semantic_validation();
    test_encoder_validates();
    test_fingerprint();
    test_owner_names();
    test_owner_versions();
    test_char_names();
    test_stamina_versions();
    test_size_conversion();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
