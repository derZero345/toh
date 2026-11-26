/* Radial menu components: RadialRing (one level, used for every level), RingSegment, CenterButton and the tooltip.
 * They only draw what they are given; every size and colour comes from HUD_THEME's radial* tokens. */
#include "draw.h"
#include "hud.h"
#include "radial.h"

#include <math.h>

#define T (&HUD_THEME)
#define DEG (3.14159265f / 180.0f)

/* ---- geometry ---- */

void radial_segment_angles(int i, int n, float *a0, float *a1) {
    float span = 360.0f / (float)(n > 0 ? n : 1);
    *a0 = -90.0f + span * (float)i - span / 2;
    *a1 = *a0 + span;
}

int radial_segment_at(int n, float dx, float dy) {
    if (n <= 1) return 0;
    float span = 360.0f / (float)n;
    float rel = fmodf(atan2f(dy, dx) / DEG + 90.0f + span / 2, 360.0f);
    if (rel < 0) rel += 360.0f;
    int i = (int)(rel / span);
    return i >= n ? n - 1 : i;
}

static Vec2 polar(float cx, float cy, float r, float deg) {
    return (Vec2){ cx + r * cosf(deg * DEG), cy + r * sinf(deg * DEG) };
}

/* A ring sector from a0 to a1 degrees, with a gap of constant width on both sides (none for a full ring). It is
 * not convex, so it is filled as a strip of convex quads that share their edges. */
static void fill_sector(float cx, float cy, float rin, float rout, float a0, float a1, float gap, Color c) {
    if (rout <= rin || rout <= 0 || c.a == 0) return;
    bool full = a1 - a0 >= 359.9f;
    if (rin < 0) rin = 0;
    float gi = full || rin <= 0 ? 0 : gap / 2 / rin / DEG, go = full ? 0 : gap / 2 / rout / DEG;
    float i0 = a0 + gi, i1 = a1 - gi, o0 = a0 + go, o1 = a1 - go;
    if (o1 <= o0) return;
    if (i1 <= i0) i0 = i1 = (a0 + a1) / 2; /* the gap ate the inner arc: the sector ends in a point */
    int steps = (int)ceilf((a1 - a0) / 6.0f);
    if (steps < 2) steps = 2;
    for (int k = 0; k < steps; k++) {
        float u0 = (float)k / (float)steps, u1 = (float)(k + 1) / (float)steps;
        Vec2 q[4] = {
            polar(cx, cy, rin, i0 + (i1 - i0) * u0), polar(cx, cy, rout, o0 + (o1 - o0) * u0),
            polar(cx, cy, rout, o0 + (o1 - o0) * u1), polar(cx, cy, rin, i0 + (i1 - i0) * u1),
        };
        dr_fill_polygon(q, 4, c);
    }
}

static Color mix(Color a, Color b, float u, uint8_t alpha) {
    return (Color){ (uint8_t)(a.r + (b.r - a.r) * u), (uint8_t)(a.g + (b.g - a.g) * u),
                    (uint8_t)(a.b + (b.b - a.b) * u), alpha };
}

/* ---- RingSegment ---- */

void radial_draw_segment(const RadialRingView *v, int i) {
    const RadialEntry *e = v->items[i];
    float s = v->scale, a0, a1;
    radial_segment_angles(i, v->n, &a0, &a1);
    bool disabled = v->why && v->why[i];
    bool hover = v->interactive && v->hover == i && !disabled, press = hover && v->press == i;
    bool active = v->highlight == i, dimmed = v->highlight >= 0 && !active;
    float alpha = v->geo.alpha; /* the fill stays dark when dimmed or disabled; the icon and band fade */
    float content = alpha * (disabled ? T->radialDisabledAlpha : dimmed ? T->radialDimAlpha : 1);
    if (alpha <= 0.004f) return;
    dr_set_alpha(alpha);

    Color accent = T->accent[e->accent];
    float rin = v->geo.rin * s, rout = (v->geo.rout + (hover ? T->radialHoverPop : 0)) * s, gap = T->radialSegGap * s;
    Color fill = disabled ? T->radialSegDisabled : press ? T->radialSegPress : hover ? T->radialSegHover
               : dimmed ? T->radialSegDim : T->radialSeg;
    if (active) fill = mix(T->radialSeg, accent, 0.45f, T->radialSegHover.a);
    fill_sector(v->cx, v->cy, rin, rout, a0, a1, gap, fill);

    dr_set_alpha(content);
    float band = (hover || active ? T->radialBandHover : T->radialBand) * s;
    Color bc = disabled ? T->radialDisabledBand : hover || active ? T->accentLight[e->accent] : accent;
    fill_sector(v->cx, v->cy, fmaxf(rin, rout - band), rout, a0, a1, gap, bc);

    float mid = (a0 + a1) / 2, inner = rout - band, thick = inner - rin;
    if (thick <= 0) {
        dr_set_alpha(1);
        return;
    }
    float ic = fminf(T->radialIcon * s, thick * T->radialIconFill);
    Vec2 c = polar(v->cx, v->cy, rin + thick / 2, mid);
    Texture *tex = hud_icon(e->icon);
    if (tex && ic >= 2) dr_image(tex, (float)c.x - ic / 2, (float)c.y - ic / 2, ic, ic);

    if (RADIAL_KINDS[e->kind].opensRing) { /* a small triangle pointing outwards: this one opens a ring */
        float m = fminf(T->radialMarker * s, thick * 0.18f), tip = inner - 2 * s;
        Vec2 tp = polar(v->cx, v->cy, tip, mid), base = polar(v->cx, v->cy, tip - m, mid);
        float nx = -sinf(mid * DEG) * m * 0.9f, ny = cosf(mid * DEG) * m * 0.9f;
        Vec2 tri[3] = { tp, { base.x + nx, base.y + ny }, { base.x - nx, base.y - ny } };
        dr_fill_polygon(tri, 3, hover || active ? T->hover : T->textDim);
    }
    dr_set_alpha(1);
}

/* ---- RadialRing ---- */

void radial_draw_ring(const RadialRingView *v) {
    if (v->n <= 0 || v->geo.alpha <= 0.004f || v->geo.rout <= v->geo.rin) return;
    for (int i = 0; i < v->n; i++) radial_draw_segment(v, i);
}

/* ---- CenterButton ---- */

void radial_draw_center(float cx, float cy, float s, float alpha, const RadialEntry *action, float back, bool hover,
                        bool press, bool disabled) {
    if (alpha <= 0.004f) return;
    float r = T->radialCenterR * s;
    bool dim = disabled && back < 0.5f;
    dr_set_alpha(alpha);
    for (int i = T->shadowLayers; i >= 1; i--) {
        float o = T->shadowStep * s * (float)i;
        dr_fill_oval(cx - r - o, cy - r - o / 2, 2 * (r + o), 2 * (r + o), T->shadow);
    }
    Color fill = press && !dim ? T->radialSegPress : hover && !dim ? T->radialSegHover : T->radialCenter;
    dr_fill_oval(cx - r, cy - r, 2 * r, 2 * r, fill);
    dr_stroke_oval(cx - r, cy - r, 2 * r, 2 * r, fmaxf(1, s), hover && !dim ? T->hover : hud_border());

    Color line = hover && !dim ? T->hover : T->text;
    float th = fmaxf(1.5f, 2.2f * s);
    if (back < 1) { /* the top level: the menu's own action, or a cross that closes the menu */
        dr_set_alpha(alpha * (1 - back) * (dim ? T->radialDisabledAlpha : 1));
        Texture *tex = action ? hud_icon(action->icon) : NULL;
        float ic = r * 1.1f, p = r * 0.32f;
        if (tex) dr_image(tex, cx - ic / 2, cy - ic / 2, ic, ic);
        else if (!action) {
            dr_line(cx - p, cy - p, cx + p, cy + p, th, line);
            dr_line(cx + p, cy - p, cx - p, cy + p, th, line);
        }
    }
    if (back > 0) { /* in a submenu: a back arrow */
        dr_set_alpha(alpha * back);
        float w = r * 0.22f, h = r * 0.38f;
        dr_line(cx + w, cy - h, cx - w, cy, th, line);
        dr_line(cx - w, cy, cx + w, cy + h, th, line);
    }
    dr_set_alpha(1);
}

/* ---- tooltip ---- */

void radial_draw_tooltip(float x, float y, float s, float winW, float winH, const char *label, const char *why) {
    if ((!label || !label[0]) && (!why || !why[0])) return;
    float fs = T->font * s, small = T->smallFont * s, pad = T->radialTipPad * s, off = T->radialTipOffset * s;
    float la = 0, ld = 0, wa = 0, wd = 0;
    bool hasLabel = label && label[0], hasWhy = why && why[0];
    if (hasLabel) dr_font_metrics(FONT_BOLD, fs, &la, &ld);
    if (hasWhy) dr_font_metrics(FONT_PLAIN, small, &wa, &wd);
    float lw = hasLabel ? dr_text_width(FONT_BOLD, fs, label) : 0, ww = hasWhy ? dr_text_width(FONT_PLAIN, small, why) : 0;
    float w = fmaxf(lw, ww) + 2 * pad, h = 2 * pad + (la + ld) + (wa + wd) + (hasLabel && hasWhy ? 2 * s : 0);
    float px = x + off, py = y + off, m = T->radialMargin * s;
    if (px + w > winW - m) px = x - off - w; /* no room right of the pointer: left of it */
    if (py + h > winH - m) py = y - off - h;
    if (px < m) px = m;
    if (py < m) py = m;
    hud_draw_panel((HudRect){ px, py, w, h }, s * 0.6f);
    float ty = py + pad;
    if (hasLabel) {
        dr_text(FONT_BOLD, fs, px + pad, ty + la, T->text, label);
        ty += la + ld + 2 * s;
    }
    if (hasWhy) dr_text(FONT_PLAIN, small, px + pad, ty + wa, T->radialWhy, why);
}
