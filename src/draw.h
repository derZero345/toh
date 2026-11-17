#ifndef PF_DRAW_H
#define PF_DRAW_H

#include "font.h"
#include "util.h"
#include "vk_renderer.h"

/*
 * Immediate-mode 2D drawing API modelled on the subset of java.awt.Graphics2D
 * the original program used. All coordinates are logical window pixels.
 */

void dr_begin(float logical_w, float logical_h, float pixel_scale);
void dr_end_and_render(Color clear, const char *screenshot_path);

/* Multiplies the alpha of everything drawn (AlphaComposite.SrcOver.derive). */
void dr_set_alpha(float a);
void dr_set_clip(float x, float y, float w, float h);
void dr_clear_clip(void);

void dr_fill_rect(float x, float y, float w, float h, Color c);
void dr_stroke_rect(float x, float y, float w, float h, float thickness, Color c);
void dr_fill_round_rect(float x, float y, float w, float h, float arc_w, float arc_h, Color c);
void dr_fill_oval(float x, float y, float w, float h, Color c);
void dr_stroke_oval(float x, float y, float w, float h, float thickness, Color c);
/* Same convention as Graphics.drawArc: degrees, 0 = 3 o'clock, counter-clockwise. */
void dr_stroke_arc(float x, float y, float w, float h, float start_deg, float extent_deg,
                   float thickness, Color c);
void dr_line(float x1, float y1, float x2, float y2, float thickness, Color c);
void dr_polygon(const Vec2 *pts, int n, float thickness, Color c);

void dr_image(Texture *t, float x, float y, float w, float h);

/*
 * Vertex caches for geometry that is expensive to build but rarely changes (the
 * grid). Everything drawn between dr_cache_begin and dr_cache_end must use the
 * font atlas texture (solid shapes and text) with no clip rect; it is recorded
 * relative to (ox, oy) and can be replayed at another origin with dr_cache_draw.
 * Culling is disabled while recording, so a replay after panning is complete.
 */
typedef struct {
    DA(Vertex) verts;
    bool valid;
} DrCache;

void dr_cache_begin(void);
void dr_cache_end(DrCache *c, float ox, float oy);
void dr_cache_draw(const DrCache *c, float ox, float oy);
void dr_cache_free(DrCache *c);

/* Text is drawn with its baseline at y, like Graphics.drawString. */
void  dr_text(FontStyle style, float size, float x, float y, Color c, const char *text);
float dr_text_width(FontStyle style, float size, const char *text);
void  dr_font_metrics(FontStyle style, float size, float *ascent, float *descent);

#endif
