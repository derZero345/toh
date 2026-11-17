#include "font.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include <fontconfig/fontconfig.h>
#include <stdio.h>

#define ATLAS_SIZE  FONT_ATLAS_SIZE
#define CACHE_SIZE  8192 /* power of two */

typedef struct {
    uint32_t key; /* 0 = empty */
    Glyph glyph;
} CacheEntry;

static struct {
    FT_Library lib;
    FT_Face faces[2];
    int face_px[2];
    uint8_t *pixels;
    Texture *atlas;
    int pen_x, pen_y, row_h;
    int dirty_y0, dirty_y1;
    bool reset_pending;
    CacheEntry cache[CACHE_SIZE];
    int cache_count;
} F;

static char *find_font_file(const char *pattern, int *index) {
    FcPattern *pat = FcNameParse((const FcChar8 *)pattern);
    if (!pat) return NULL;
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult res;
    FcPattern *match = FcFontMatch(NULL, pat, &res);
    char *path = NULL;
    if (match) {
        FcChar8 *file = NULL;
        if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch) path = str_dup((const char *)file);
        if (FcPatternGetInteger(match, FC_INDEX, 0, index) != FcResultMatch) *index = 0;
        FcPatternDestroy(match);
    }
    FcPatternDestroy(pat);
    return path;
}

static bool load_face(int slot, const char *pattern) {
    int index = 0;
    char *path = find_font_file(pattern, &index);
    if (!path) {
        fprintf(stderr, "fontconfig: no font for '%s'\n", pattern);
        return false;
    }
    FT_Error err = FT_New_Face(F.lib, path, index, &F.faces[slot]);
    if (err) fprintf(stderr, "FreeType: cannot load '%s' (%d)\n", path, err);
    free(path);
    return err == 0;
}

static unsigned atlas_generation;

unsigned font_atlas_generation(void) { return atlas_generation; }

static void clear_cache(void) {
    atlas_generation++;
    memset(F.cache, 0, sizeof(F.cache));
    F.cache_count = 0;
    memset(F.pixels, 0, (size_t)ATLAS_SIZE * ATLAS_SIZE);
    /* keep a fully opaque 2x2 block at the origin for solid fills */
    F.pixels[0] = F.pixels[1] = F.pixels[ATLAS_SIZE] = F.pixels[ATLAS_SIZE + 1] = 255;
    F.pen_x = 4;
    F.pen_y = 0;
    F.row_h = 4;
    F.dirty_y0 = 0;
    F.dirty_y1 = ATLAS_SIZE;
}

bool font_init(void) {
    if (!FcInit()) { fprintf(stderr, "FcInit failed\n"); return false; }
    if (FT_Init_FreeType(&F.lib)) { fprintf(stderr, "FT_Init_FreeType failed\n"); return false; }
    /* The Java version uses "Arial"; fontconfig falls back to a sans font. */
    if (!load_face(FONT_PLAIN, "Arial:style=Regular")) return false;
    if (!load_face(FONT_BOLD, "Arial:bold")) return false;
    F.pixels = malloc((size_t)ATLAS_SIZE * ATLAS_SIZE);
    if (!F.pixels) abort();
    clear_cache();
    F.atlas = vkr_texture_create(ATLAS_SIZE, ATLAS_SIZE, F.pixels, true);
    F.dirty_y0 = ATLAS_SIZE;
    F.dirty_y1 = 0;
    return true;
}

void font_shutdown(void) {
    if (F.atlas) vkr_texture_release(F.atlas);
    for (int i = 0; i < 2; i++) if (F.faces[i]) FT_Done_Face(F.faces[i]);
    if (F.lib) FT_Done_FreeType(F.lib);
    free(F.pixels);
    memset(&F, 0, sizeof(F));
}

Texture *font_atlas(void) { return F.atlas; }

static FT_Face face_at(FontStyle style, int px) {
    FT_Face face = F.faces[style];
    if (F.face_px[style] != px) {
        FT_Set_Pixel_Sizes(face, 0, (FT_UInt)px);
        F.face_px[style] = px;
    }
    return face;
}

void font_metrics(FontStyle style, int px, float *ascent, float *descent) {
    if (px < 1) px = 1;
    FT_Face face = face_at(style, px);
    *ascent = (float)face->size->metrics.ascender / 64.0f;
    *descent = (float)-face->size->metrics.descender / 64.0f;
}

static const Glyph INVALID_GLYPH = { 0 };

const Glyph *font_glyph(FontStyle style, int px, uint32_t cp) {
    if (px < 1) px = 1;
    if (px > 1023) px = 1023;
    if (cp > 0x1FFFFF) cp = '?';
    uint32_t key = ((uint32_t)style << 31) | ((uint32_t)px << 21) | cp;
    if (key == 0) key = 1u << 21; /* never collides with a real key at px >= 1 */

    uint32_t h = (key * 2654435761u) & (CACHE_SIZE - 1);
    while (F.cache[h].key) {
        if (F.cache[h].key == key) return &F.cache[h].glyph;
        h = (h + 1) & (CACHE_SIZE - 1);
    }
    if (F.reset_pending || F.cache_count > CACHE_SIZE * 3 / 4) {
        F.reset_pending = true;
        return &INVALID_GLYPH;
    }

    FT_Face face = face_at(style, px);
    Glyph g = { 0 };
    if (FT_Load_Char(face, cp, FT_LOAD_RENDER) == 0) {
        FT_GlyphSlot slot = face->glyph;
        FT_Bitmap *bm = &slot->bitmap;
        g.w = (int)bm->width;
        g.h = (int)bm->rows;
        g.left = slot->bitmap_left;
        g.top = slot->bitmap_top;
        g.advance = (float)slot->advance.x / 64.0f;
        g.valid = true;
        if (g.w > 0 && g.h > 0) {
            if (F.pen_x + g.w + 1 > ATLAS_SIZE) {
                F.pen_x = 0;
                F.pen_y += F.row_h + 1;
                F.row_h = 0;
            }
            if (F.pen_y + g.h + 1 > ATLAS_SIZE || g.w + 1 > ATLAS_SIZE) {
                F.reset_pending = true; /* atlas full: rebuild next frame */
                return &INVALID_GLYPH;
            }
            for (int y = 0; y < g.h; y++) {
                const uint8_t *src = bm->buffer + y * bm->pitch;
                memcpy(F.pixels + (size_t)(F.pen_y + y) * ATLAS_SIZE + F.pen_x, src, (size_t)g.w);
            }
            g.u0 = (float)F.pen_x / ATLAS_SIZE;
            g.v0 = (float)F.pen_y / ATLAS_SIZE;
            g.u1 = (float)(F.pen_x + g.w) / ATLAS_SIZE;
            g.v1 = (float)(F.pen_y + g.h) / ATLAS_SIZE;
            if (F.pen_y < F.dirty_y0) F.dirty_y0 = F.pen_y;
            if (F.pen_y + g.h > F.dirty_y1) F.dirty_y1 = F.pen_y + g.h;
            F.pen_x += g.w + 1;
            if (g.h > F.row_h) F.row_h = g.h;
        }
    }
    F.cache[h].key = key;
    F.cache[h].glyph = g;
    F.cache_count++;
    return &F.cache[h].glyph;
}

void font_flush(void) {
    if (F.dirty_y1 > F.dirty_y0) {
        vkr_texture_update(F.atlas, F.pixels, F.dirty_y0, F.dirty_y1);
        F.dirty_y0 = ATLAS_SIZE;
        F.dirty_y1 = 0;
    }
    if (F.reset_pending) {
        /* Glyphs drawn this frame are still valid on the GPU; the cleared atlas
         * is uploaded together with the next frame's glyphs. */
        F.reset_pending = false;
        clear_cache();
    }
}

uint32_t utf8_next(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t cp;
    int extra;
    if (p[0] < 0x80)      { cp = p[0];        extra = 0; }
    else if (p[0] < 0xE0) { cp = p[0] & 0x1F; extra = 1; }
    else if (p[0] < 0xF0) { cp = p[0] & 0x0F; extra = 2; }
    else                  { cp = p[0] & 0x07; extra = 3; }
    p++;
    for (int i = 0; i < extra; i++) {
        if ((p[0] & 0xC0) != 0x80) { *s = (const char *)p; return 0xFFFD; }
        cp = (cp << 6) | (p[0] & 0x3F);
        p++;
    }
    *s = (const char *)p;
    return cp;
}
