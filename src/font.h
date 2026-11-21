#ifndef TOH_FONT_H
#define TOH_FONT_H

#include "util.h"
#include "vk_renderer.h"

/* The atlas keeps an opaque 2x2 texel block at (0,0) so solid shapes can be
 * batched together with text: sample it at (FONT_WHITE_UV, FONT_WHITE_UV). */
#define FONT_ATLAS_SIZE 2048
#define FONT_WHITE_UV   (1.0f / FONT_ATLAS_SIZE)

typedef enum { FONT_PLAIN = 0, FONT_BOLD = 1 } FontStyle;

typedef struct {
    bool valid;
    float u0, v0, u1, v1;
    int w, h;          /* bitmap size in physical pixels */
    int left, top;     /* bearing in physical pixels */
    float advance;     /* physical pixels */
} Glyph;

bool font_init(void);
void font_shutdown(void);

/* px = font size in physical pixels. */
const Glyph *font_glyph(FontStyle style, int px, uint32_t codepoint);
/* Ascender / descender (positive) in physical pixels. */
void font_metrics(FontStyle style, int px, float *ascent, float *descent);

Texture *font_atlas(void);
/* Changes whenever the atlas is cleared, invalidating previously returned glyph UVs. */
unsigned font_atlas_generation(void);
/* Uploads newly rasterised glyphs; call once per frame before rendering. */
void font_flush(void);

/* Decodes one UTF-8 code point and advances *s. */
uint32_t utf8_next(const char **s);

#endif
