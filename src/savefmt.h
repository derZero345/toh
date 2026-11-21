#ifndef TOH_SAVEFMT_H
#define TOH_SAVEFMT_H

/*
 * .tuhh file format, version 2: encoding, decoding and validation.
 * Version 2 adds an owner name after each character's stats; version 1 files still load (no owners).
 *
 * Plain C11 on top of libzstd, libwebp and libsodium. It knows nothing about SDL,
 * Vulkan or the App struct, so tools or a server can reuse it to validate files.
 * Design: docs/superpowers/specs/2026-11-18-save-files-design.md
 *
 * Layout (little endian):
 *   "TUHH" | u16 version | u16 flags | u32 state_len | zstd(state) |
 *   u32 image_count | { u8 kind, u16 w, u16 h, u32 len, WebP }... | 32-byte keyed BLAKE2b tag
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SAVEFMT_VERSION     2
#define SAVEFMT_TAG_SIZE    32
#define SAVEFMT_HEADER_SIZE 12 /* magic, version, flags, state_len */

/* Limits for untrusted input. The encoder enforces the same rules. */
#define SAVEFMT_MAX_FILE        (256u << 20)
#define SAVEFMT_MAX_STATE       (16u << 20)
#define SAVEFMT_MAX_IMAGES      4096u
#define SAVEFMT_MAX_DIM         16383 /* WebP's limit */
#define SAVEFMT_MAX_PIXEL_BYTES (1ull << 30) /* decoded RGBA of all images together */
#define SAVEFMT_MAX_ENTITIES    100000u
#define SAVEFMT_MAX_WAYPOINTS   10000u
#define SAVEFMT_MAX_STR         255u
#define SAVEFMT_NAME_MAX        32 /* waypoint names are shorter than this (= WAYPOINT_NAME_MAX) */
#define SAVEFMT_OWNER_MAX       25 /* owner names are shorter than this (= PLAYER_NAME_MAX) */
#define SAVEFMT_MAX_SPEED       1000 /* = MAX_SPEED */
#define SAVEFMT_MAX_COORD       1000000
#define SAVEFMT_MAX_OFFSET      1000000000
#define SAVEFMT_MIN_HEX         5
#define SAVEFMT_MAX_HEX         400
#define SAVEFMT_MAX_GRID        10000

enum { SAVE_IMG_LOSSLESS = 0, SAVE_IMG_LOSSY = 1 };
enum { SAVE_ENT_CHARACTER = 0, SAVE_ENT_WALL = 1 };

typedef enum {
    SAVE_OK = 0,
    SAVE_ERR_NOT_SAVE,  /* wrong magic */
    SAVE_ERR_NEWER,     /* version from the future */
    SAVE_ERR_CORRUPT,   /* bad tag, structure or values */
    SAVE_ERR_TOO_LARGE, /* over one of the size limits */
    SAVE_ERR_INVALID,   /* encoder: the data breaks a rule and would not load again */
    SAVE_ERR_ENCODE,    /* encoder: WebP or zstd failed */
    SAVE_ERR_INTERNAL,  /* out of memory, libsodium init failed */
} SaveError;

typedef struct {
    uint8_t kind;        /* SAVE_IMG_* */
    int w, h;            /* size of rgba; when webp is set: size of the WebP image */
    const uint8_t *rgba; /* w*h*4 bytes; may be NULL when webp is set */
    const uint8_t *webp; /* already encoded data, used as is when set */
    size_t webp_len;
    uint64_t id;         /* identity for savefmt_fingerprint only; never written to files */
} SaveImage;

typedef struct {
    uint8_t type;   /* SAVE_ENT_* */
    uint32_t image; /* index into SaveData.images */
    int32_t row, col;
    /* characters only (not stored for walls) */
    int32_t size, max_health, health, attack, ac, speed, initiative;
    char owner[SAVEFMT_OWNER_MAX]; /* characters only: player name, "" = no owner (version 2) */
} SaveEntity;

typedef struct {
    uint32_t entity; /* index into SaveData.entities */
    double value;
} SaveInit;

typedef struct {
    char name[SAVEFMT_NAME_MAX];
    int32_t row, col;
} SaveWaypoint;

typedef struct {
    bool is_flat, has_background, init_markers_visible, turn_started;
    uint32_t background; /* image index, when has_background */
    uint32_t bg_rows, bg_cols;
    int32_t offset_x, offset_y;
    uint32_t hex_size;
    char theme[SAVEFMT_MAX_STR + 1];

    SaveImage *images;
    uint32_t nimages;
    SaveEntity *entities;
    uint32_t nentities;
    SaveInit *init;
    uint32_t ninit;
    uint32_t current_turn;
    SaveWaypoint *waypoints;
    uint32_t nwaypoints;

    /* Set by savefmt_decode: savefmt_free releases the arrays and image buffers.
     * Data built by the caller leaves it false and manages its own memory. */
    bool owns_memory;
} SaveData;

/* A WebP image produced while encoding. Free webp with free(). */
typedef struct {
    uint8_t *webp;
    size_t len;
    int w, h; /* can be smaller than the source: images over SAVEFMT_MAX_DIM are scaled down */
} SaveBlob;

/* Encodes d into a new buffer (*out, free with free()).
 * blobs_out (optional, d->nimages entries): receives the WebP data of every image that
 * had to be encoded (webp == NULL on input); other entries are zeroed. Ownership passes
 * to the caller, also on failure (use savefmt_blobs_free). */
SaveError savefmt_encode(const SaveData *d, SaveBlob *blobs_out, uint8_t **out, size_t *out_len);

/* Verifies and decodes a whole file. Images come back with both rgba and webp set.
 * On failure *out is left empty (savefmt_free is still safe to call). */
SaveError savefmt_decode(const uint8_t *buf, size_t len, SaveData *out);

void savefmt_free(SaveData *d);
void savefmt_blobs_free(SaveBlob *blobs, uint32_t n);

/* Hash of the table content, ignoring camera and theme; images by their id.
 * Returns false when out of memory. */
bool savefmt_fingerprint(const SaveData *d, uint8_t out[SAVEFMT_TAG_SIZE]);

/* Integrity tag of buf[0..len). Exposed for tests. */
void savefmt_compute_tag(const uint8_t *buf, size_t len, uint8_t out[SAVEFMT_TAG_SIZE]);

/* Player names, used as character owners: 1..24 bytes of UTF-8 without spaces or control characters. */
bool savefmt_owner_valid(const char *name);

const char *savefmt_strerror(SaveError e);

#endif
