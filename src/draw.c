#include "draw.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static struct {
    DA(Vertex) verts;
    DA(DrawCmd) cmds;
    float w, h, scale;
    float alpha;
    bool clip;
    float cx, cy, cw, ch;
    DA(Vec2) tmp;
    DA(Vec2) tmp2;
    bool recording; /* recording into a DrCache: no culling, see dr_cache_begin */
    int rec_start;
} D;

/* ---- culling --------------------------------------------------------------- */

/* Is the box [x0,x1]x[y0,y1] (logical coordinates) completely outside the window? */
static bool offscreen(double x0, double y0, double x1, double y1) {
    if (D.recording) return false;
    return x1 < 0 || y1 < 0 || x0 > D.w || y0 > D.h;
}

/* Bounding box of points, grown by `margin` on every side. */
static bool points_offscreen(const Vec2 *p, int n, double margin) {
    if (D.recording || n <= 0) return false;
    double x0 = p[0].x, x1 = p[0].x, y0 = p[0].y, y1 = p[0].y;
    for (int i = 1; i < n; i++) {
        if (p[i].x < x0) x0 = p[i].x;
        if (p[i].x > x1) x1 = p[i].x;
        if (p[i].y < y0) y0 = p[i].y;
        if (p[i].y > y1) y1 = p[i].y;
    }
    return offscreen(x0 - margin, y0 - margin, x1 + margin, y1 + margin);
}

void dr_begin(float logical_w, float logical_h, float pixel_scale) {
    da_clear(&D.verts);
    da_clear(&D.cmds);
    D.w = logical_w;
    D.h = logical_h;
    D.scale = pixel_scale > 0 ? pixel_scale : 1.0f;
    D.alpha = 1.0f;
    D.clip = false;
}

void dr_end_and_render(Color clear, const char *screenshot_path) {
    font_flush();
    float cc[4] = { clear.r / 255.0f, clear.g / 255.0f, clear.b / 255.0f, 1.0f };
    vkr_render(D.verts.items, D.verts.count, D.cmds.items, D.cmds.count, cc, D.w, D.h, screenshot_path);
}

void dr_set_alpha(float a) { D.alpha = a; }

void dr_set_clip(float x, float y, float w, float h) {
    D.clip = true;
    D.cx = x; D.cy = y; D.cw = w; D.ch = h;
}

void dr_clear_clip(void) { D.clip = false; }

/* ---- batching -------------------------------------------------------------- */

static void use_texture(Texture *t) {
    if (D.cmds.count > 0) {
        DrawCmd *last = &D.cmds.items[D.cmds.count - 1];
        bool same_clip = last->scissor == D.clip &&
            (!D.clip || (last->sx == D.cx && last->sy == D.cy && last->sw == D.cw && last->sh == D.ch));
        if (last->tex == t && same_clip) return;
    }
    DrawCmd c = { .tex = t, .first = D.verts.count, .count = 0, .scissor = D.clip,
                  .sx = D.cx, .sy = D.cy, .sw = D.cw, .sh = D.ch };
    da_push(&D.cmds, c);
}

static inline uint32_t pack(Color c) {
    uint32_t a = (uint32_t)lrintf(c.a * D.alpha);
    if (a > 255) a = 255;
    return (uint32_t)c.r | ((uint32_t)c.g << 8) | ((uint32_t)c.b << 16) | (a << 24);
}

static inline void vtx(float x, float y, float u, float v, uint32_t col) {
    Vertex vx = { x, y, u, v, col };
    da_push(&D.verts, vx);
    D.cmds.items[D.cmds.count - 1].count++;
}

static void solid_tri(Vec2 a, Vec2 b, Vec2 c, uint32_t col) {
    const float u = FONT_WHITE_UV;
    vtx((float)a.x, (float)a.y, u, u, col);
    vtx((float)b.x, (float)b.y, u, u, col);
    vtx((float)c.x, (float)c.y, u, u, col);
}

static void solid_quad(Vec2 a, Vec2 b, Vec2 c, Vec2 d, uint32_t col) {
    solid_tri(a, b, c, col);
    solid_tri(a, c, d, col);
}

static void fill_convex(const Vec2 *p, int n, Color c) {
    if (n < 3 || points_offscreen(p, n, 0)) return;
    use_texture(font_atlas());
    uint32_t col = pack(c);
    for (int i = 1; i + 1 < n; i++) solid_tri(p[0], p[i], p[i + 1], col);
}

/* Strokes a polyline with miter joins; open ends get square caps (the
 * java.awt.BasicStroke default: CAP_SQUARE, JOIN_MITER, miter limit 10). */
static void stroke_polyline(const Vec2 *p, int n, bool closed, float thickness, Color c) {
    if (n < 2 || thickness <= 0) return;
    /* miter joins reach at most 10 half-widths out (see the miter limit below) */
    if (points_offscreen(p, n, thickness * 5.0 + 1)) return;
    use_texture(font_atlas());
    uint32_t col = pack(c);
    double hw = thickness / 2.0;

    da_clear(&D.tmp);  /* outer */
    da_clear(&D.tmp2); /* inner */
    for (int i = 0; i < n; i++) {
        Vec2 cur = p[i];
        bool has_prev = closed || i > 0, has_next = closed || i < n - 1;
        Vec2 prev = p[(i - 1 + n) % n], next = p[(i + 1) % n];
        Vec2 d0 = { 0, 0 }, d1 = { 0, 0 };
        if (has_prev) {
            d0 = (Vec2){ cur.x - prev.x, cur.y - prev.y };
            double l = hypot(d0.x, d0.y);
            if (l > 1e-9) { d0.x /= l; d0.y /= l; } else has_prev = false;
        }
        if (has_next) {
            d1 = (Vec2){ next.x - cur.x, next.y - cur.y };
            double l = hypot(d1.x, d1.y);
            if (l > 1e-9) { d1.x /= l; d1.y /= l; } else has_next = false;
        }
        if (!has_prev && !has_next) { d1 = (Vec2){ 1, 0 }; has_next = true; }
        Vec2 o, in;
        if (has_prev && has_next) {
            Vec2 n0 = { -d0.y, d0.x }, n1 = { -d1.y, d1.x };
            Vec2 m = { n0.x + n1.x, n0.y + n1.y };
            double ml = hypot(m.x, m.y);
            double dot = 0;
            if (ml > 1e-9) { m.x /= ml; m.y /= ml; dot = m.x * n1.x + m.y * n1.y; }
            if (ml <= 1e-9 || dot < 0.1) { /* miter limit exceeded: fall back to bevel-ish */
                m = n1;
                dot = 1;
            }
            double len = hw / dot;
            o = (Vec2){ cur.x + m.x * len, cur.y + m.y * len };
            in = (Vec2){ cur.x - m.x * len, cur.y - m.y * len };
        } else {
            Vec2 d = has_next ? d1 : d0;
            Vec2 nn = { -d.y, d.x };
            /* square cap: push the end point out by half the line width */
            double s = has_next ? -hw : hw;
            Vec2 e = { cur.x + d.x * s, cur.y + d.y * s };
            o = (Vec2){ e.x + nn.x * hw, e.y + nn.y * hw };
            in = (Vec2){ e.x - nn.x * hw, e.y - nn.y * hw };
        }
        da_push(&D.tmp, o);
        da_push(&D.tmp2, in);
    }
    int segs = closed ? n : n - 1;
    for (int i = 0; i < segs; i++) {
        int j = (i + 1) % n;
        solid_quad(D.tmp.items[i], D.tmp.items[j], D.tmp2.items[j], D.tmp2.items[i], col);
    }
}

static int ellipse_segments(float w, float h) {
    float r = fmaxf(fabsf(w), fabsf(h)) * 0.5f * D.scale;
    int n = (int)(r * 0.75f);
    if (n < 16) n = 16;
    if (n > 256) n = 256;
    return n;
}

/* ---- primitives --------------------------------------------------------------- */

void dr_fill_rect(float x, float y, float w, float h, Color c) {
    if (w <= 0 || h <= 0) return;
    use_texture(font_atlas());
    solid_quad((Vec2){ x, y }, (Vec2){ x + w, y }, (Vec2){ x + w, y + h }, (Vec2){ x, y + h }, pack(c));
}

void dr_stroke_rect(float x, float y, float w, float h, float thickness, Color c) {
    Vec2 p[4] = { { x, y }, { x + w, y }, { x + w, y + h }, { x, y + h } };
    stroke_polyline(p, 4, true, thickness, c);
}

void dr_fill_round_rect(float x, float y, float w, float h, float arc_w, float arc_h, Color c) {
    float rx = fminf(arc_w / 2, w / 2), ry = fminf(arc_h / 2, h / 2);
    if (rx <= 0 || ry <= 0) { dr_fill_rect(x, y, w, h, c); return; }
    const int seg = 8;
    Vec2 p[4 * (seg + 1)];
    int n = 0;
    float cxs[4] = { x + w - rx, x + rx, x + rx, x + w - rx };
    float cys[4] = { y + ry, y + ry, y + h - ry, y + h - ry };
    for (int corner = 0; corner < 4; corner++) {
        for (int i = 0; i <= seg; i++) {
            double a = (corner * 90.0 + 90.0 * i / seg) * M_PI / 180.0;
            p[n++] = (Vec2){ cxs[corner] + rx * cos(a), cys[corner] - ry * sin(a) };
        }
    }
    fill_convex(p, n, c);
}

void dr_fill_oval(float x, float y, float w, float h, Color c) {
    int n = ellipse_segments(w, h);
    da_reserve(&D.tmp, n);
    double cx = x + w / 2.0, cy = y + h / 2.0;
    for (int i = 0; i < n; i++) {
        double a = 2 * M_PI * i / n;
        D.tmp.items[i] = (Vec2){ cx + w / 2.0 * cos(a), cy + h / 2.0 * sin(a) };
    }
    /* copy because fill_convex does not touch tmp, but keep it explicit */
    fill_convex(D.tmp.items, n, c);
}

void dr_stroke_oval(float x, float y, float w, float h, float thickness, Color c) {
    int n = ellipse_segments(w, h);
    Vec2 *p = malloc(sizeof(Vec2) * (size_t)n);
    double cx = x + w / 2.0, cy = y + h / 2.0;
    for (int i = 0; i < n; i++) {
        double a = 2 * M_PI * i / n;
        p[i] = (Vec2){ cx + w / 2.0 * cos(a), cy + h / 2.0 * sin(a) };
    }
    stroke_polyline(p, n, true, thickness, c);
    free(p);
}

void dr_stroke_arc(float x, float y, float w, float h, float start_deg, float extent_deg,
                   float thickness, Color c) {
    if (extent_deg == 0) return;
    int full = ellipse_segments(w, h);
    int n = (int)ceilf(full * fabsf(extent_deg) / 360.0f) + 1;
    if (n < 2) n = 2;
    Vec2 *p = malloc(sizeof(Vec2) * (size_t)n);
    double cx = x + w / 2.0, cy = y + h / 2.0;
    for (int i = 0; i < n; i++) {
        double a = (start_deg + extent_deg * i / (n - 1)) * M_PI / 180.0;
        p[i] = (Vec2){ cx + w / 2.0 * cos(a), cy - h / 2.0 * sin(a) };
    }
    stroke_polyline(p, n, fabsf(extent_deg) >= 360.0f, thickness, c);
    free(p);
}

void dr_line(float x1, float y1, float x2, float y2, float thickness, Color c) {
    Vec2 p[2] = { { x1, y1 }, { x2, y2 } };
    stroke_polyline(p, 2, false, thickness, c);
}

void dr_polygon(const Vec2 *pts, int n, float thickness, Color c) {
    stroke_polyline(pts, n, true, thickness, c);
}

void dr_fill_polygon(const Vec2 *pts, int n, Color c) { fill_convex(pts, n, c); }

void dr_image(Texture *t, float x, float y, float w, float h) {
    if (!t || w == 0 || h == 0) return;
    if (offscreen(fminf(x, x + w), fminf(y, y + h), fmaxf(x, x + w), fmaxf(y, y + h))) return;
    use_texture(t);
    uint32_t col = pack(COLOR_WHITE);
    vtx(x, y, 0, 0, col);
    vtx(x + w, y, 1, 0, col);
    vtx(x + w, y + h, 1, 1, col);
    vtx(x, y, 0, 0, col);
    vtx(x + w, y + h, 1, 1, col);
    vtx(x, y + h, 0, 1, col);
}

/* ---- text --------------------------------------------------------------------- */

static int font_px(float size) {
    int px = (int)lrintf(size * D.scale);
    return px < 1 ? 1 : px;
}

void dr_text(FontStyle style, float size, float x, float y, Color c, const char *text) {
    if (!text || size <= 0) return;
    use_texture(font_atlas());
    uint32_t col = pack(c);
    int px = font_px(size);
    float s = D.scale;
    float pen = roundf(x * s);
    float base = roundf(y * s);
    const char *p = text;
    while (*p) {
        uint32_t cp = utf8_next(&p);
        const Glyph *g = font_glyph(style, px, cp);
        if (!g->valid) continue;
        if (g->w > 0 && g->h > 0) {
            float gx0 = (pen + g->left) / s, gy0 = (base - g->top) / s;
            float gx1 = gx0 + g->w / s, gy1 = gy0 + g->h / s;
            if (offscreen(gx0, gy0, gx1, gy1)) {
                pen += roundf(g->advance);
                continue;
            }
            vtx(gx0, gy0, g->u0, g->v0, col);
            vtx(gx1, gy0, g->u1, g->v0, col);
            vtx(gx1, gy1, g->u1, g->v1, col);
            vtx(gx0, gy0, g->u0, g->v0, col);
            vtx(gx1, gy1, g->u1, g->v1, col);
            vtx(gx0, gy1, g->u0, g->v1, col);
        }
        pen += roundf(g->advance);
    }
}

float dr_text_width(FontStyle style, float size, const char *text) {
    if (!text || size <= 0) return 0;
    int px = font_px(size);
    float w = 0;
    const char *p = text;
    while (*p) {
        const Glyph *g = font_glyph(style, px, utf8_next(&p));
        if (g->valid) w += roundf(g->advance);
    }
    return w / D.scale;
}

void dr_font_metrics(FontStyle style, float size, float *ascent, float *descent) {
    font_metrics(style, font_px(size), ascent, descent);
    *ascent /= D.scale;
    *descent /= D.scale;
}

/* ---- vertex caches ---------------------------------------------------------- */

void dr_cache_begin(void) {
    D.recording = true;
    D.rec_start = D.verts.count;
}

void dr_cache_end(DrCache *c, float ox, float oy) {
    D.recording = false;
    da_clear(&c->verts);
    da_reserve(&c->verts, D.verts.count - D.rec_start);
    for (int i = D.rec_start; i < D.verts.count; i++) {
        Vertex v = D.verts.items[i];
        v.x -= ox;
        v.y -= oy;
        c->verts.items[c->verts.count++] = v;
    }
    c->valid = true;
}

void dr_cache_draw(const DrCache *c, float ox, float oy) {
    if (c->verts.count == 0) return;
    use_texture(font_atlas());
    da_reserve(&D.verts, D.verts.count + c->verts.count);
    for (int i = 0; i < c->verts.count; i++) {
        Vertex v = c->verts.items[i];
        v.x += ox;
        v.y += oy;
        D.verts.items[D.verts.count++] = v;
    }
    D.cmds.items[D.cmds.count - 1].count += c->verts.count;
}

void dr_cache_free(DrCache *c) {
    da_free(&c->verts);
    c->valid = false;
}
