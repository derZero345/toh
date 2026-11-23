/* Collecting and drawing presence. See presence.h. */
#include "presence.h"

#include "app.h"
#include "draw.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- coordinates ------------------------------------------------------------------------------- */

static int32_t to_world(double px, int offset) {
    double w = (px - offset) / gh->hexSize * NET_WORLD_SCALE;
    if (w > NETMSG_MAX_WORLD) w = NETMSG_MAX_WORLD;
    if (w < -NETMSG_MAX_WORLD) w = -NETMSG_MAX_WORLD;
    return (int32_t)lround(w);
}

int32_t presence_world_x(double px) { return to_world(px, gh->gridOffsetX); }
int32_t presence_world_y(double py) { return to_world(py, gh->gridOffsetY); }
double presence_px_x(int32_t wx) { return gh->gridOffsetX + (double)wx * gh->hexSize / NET_WORLD_SCALE; }
double presence_px_y(int32_t wy) { return gh->gridOffsetY + (double)wy * gh->hexSize / NET_WORLD_SCALE; }

static int32_t world_len(double px) {
    double w = px / gh->hexSize * NET_WORLD_SCALE;
    return w > NETMSG_MAX_WORLD ? NETMSG_MAX_WORLD : (int32_t)lround(w);
}

static int32_t clamp_label(long v) { return v < 0 ? 0 : v > NETMSG_MAX_LABEL ? NETMSG_MAX_LABEL : (int32_t)v; }

/* ---- collecting ------------------------------------------------------------------------------------ */

static NetHex *hexes_from(const Hex *h, int n, int max, uint32_t *count) {
    if (n > max) n = max;
    *count = (uint32_t)(n > 0 ? n : 0);
    if (n <= 0) return NULL;
    NetHex *out = malloc(sizeof(NetHex) * (size_t)n);
    if (!out) abort();
    for (int i = 0; i < n; i++) out[i] = (NetHex){ h[i].row, h[i].col };
    return out;
}

static bool coord_fits(Hex h) {
    return h.row >= -SAVEFMT_MAX_COORD && h.row <= SAVEFMT_MAX_COORD && h.col >= -SAVEFMT_MAX_COORD &&
           h.col <= SAVEFMT_MAX_COORD;
}

static void add_line(NetPresence *p, int *cap, Vec2 a, Vec2 b, int label) {
    if (p->nlines >= NETMSG_MAX_LINES) return;
    if ((int)p->nlines == *cap) {
        *cap = *cap ? *cap * 2 : 8;
        p->lines = realloc(p->lines, sizeof(NetLine) * (size_t)*cap);
        if (!p->lines) abort();
    }
    p->lines[p->nlines++] = (NetLine){ presence_world_x(a.x), presence_world_y(a.y), presence_world_x(b.x),
                                       presence_world_y(b.y), clamp_label(label) };
}

void presence_collect(NetPresence *out) {
    memset(out, 0, sizeof(*out));
    /* selected tiles (cut off at the limit; tiles outside the save format's range cannot travel) */
    int n = 0;
    for (int i = 0; i < gh->selectedTiles.count; i++) n += coord_fits(gh->selectedTiles.items[i]);
    if (n == gh->selectedTiles.count) {
        out->tiles = hexes_from(gh->selectedTiles.items, n, NETMSG_MAX_SEL_TILES, &out->ntiles);
    } else {
        HexList ok = { 0 };
        for (int i = 0; i < gh->selectedTiles.count; i++)
            if (coord_fits(gh->selectedTiles.items[i])) da_push(&ok, gh->selectedTiles.items[i]);
        out->tiles = hexes_from(ok.items, ok.count, NETMSG_MAX_SEL_TILES, &out->ntiles);
        da_free(&ok);
    }

    /* selected entities */
    if (gh->selectedEntityTiles.count > 0) {
        out->entities = malloc(sizeof(uint32_t) * (size_t)gh->selectedEntityTiles.count);
        if (!out->entities) abort();
        for (int i = 0; i < gh->selectedEntityTiles.count && out->nentities < NETMSG_MAX_SEL_ENTITIES; i++) {
            Entity *e = entity_at(gh->selectedEntityTiles.items[i]);
            if (!e || e->id == 0) continue;
            bool dup = false;
            for (uint32_t k = 0; k < out->nentities; k++) dup |= out->entities[k] == e->id;
            if (!dup) out->entities[out->nentities++] = e->id;
        }
    }

    /* movement preview (while Shift is held) */
    if (gh->entityPreviewTiles.count > 0) {
        bool fits = true;
        for (int i = 0; i < gh->entityPreviewTiles.count; i++) fits &= coord_fits(gh->entityPreviewTiles.items[i]);
        for (int i = 0; i < gh->path.count; i++) fits &= coord_fits(gh->path.items[i]);
        if (fits) {
            out->footprint = hexes_from(gh->entityPreviewTiles.items, gh->entityPreviewTiles.count,
                                        NETMSG_MAX_FOOTPRINT, &out->nfootprint);
            if (gh->path.count > 1)
                out->path = hexes_from(gh->path.items, gh->path.count, NETMSG_MAX_PATH, &out->npath);
        }
    }

    /* tools */
    int cap = 0;
    for (int i = 0; i < gh->measures.count; i++) {
        Measure *m = &gh->measures.items[i];
        Vec2 a, b;
        if (measure_line(m, &a, &b)) add_line(out, &cap, a, b, measure_length(m));
    }
    Vec2 a, b;
    if (gh->hasLineAttack && line_get(&gh->lineAttack, &a, &b)) add_line(out, &cap, a, b, line_length(&gh->lineAttack));
    if (gh->hasAreaAttack) {
        Vec2 o = hex_center(gh->areaAttack.origin);
        double r = area_radius(&gh->areaAttack);
        out->has_circle = true;
        out->circle = (NetCircle){ presence_world_x(o.x), presence_world_y(o.y), world_len(r),
                                   clamp_label((long)(area_radius_in_tiles(&gh->areaAttack) * 5)) };
    }
    if (gh->hasConeAttack) {
        ConeAttack *c = &gh->coneAttack;
        double r = cone_radius(c);
        if (r > gh->hexSize) {
            Vec2 o = hex_center(c->origin);
            out->has_cone = true;
            out->cone = (NetCircle){ presence_world_x(o.x), presence_world_y(o.y), world_len(r),
                                     clamp_label((long)(cone_radius_in_tiles(c) * 5)) };
            int start = (int)cone_start_angle(c) % 360, extent = (int)c->angle;
            out->cone_start = start < 0 ? start + 360 : start;
            out->cone_extent = extent < 0 ? 0 : extent > 360 ? 360 : extent;
        }
    }
}

/* ---- drawing ---------------------------------------------------------------------------------------- */

#define FILL_ALPHA  89 /* 35 % */
#define INSET_STEP  3

static void hex_inset(Hex h, double inset, Vec2 out[6]) {
    hex_corners(h, out);
    Vec2 c = hex_center(h);
    double hs = gh->hexSize;
    double k = hs > inset ? (hs - inset) / hs : 0;
    for (int i = 0; i < 6; i++) out[i] = (Vec2){ c.x + (out[i].x - c.x) * k, c.y + (out[i].y - c.y) * k };
}

static void outline_hex(Hex h, double inset, float th, Color c) {
    Vec2 pts[6];
    hex_inset(h, inset, pts);
    dr_polygon(pts, 6, th, c);
}

/* black or white, whichever contrasts more with c */
static Color text_on(Color c) {
    double l = 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
    return l > 140 ? COLOR_BLACK : COLOR_WHITE;
}

/* How often each visible tile was used by the people drawn so far (for insets and label stacking). */
typedef struct {
    int r0, r1, c0, c1;
    uint8_t *n;
} Counts;

static uint8_t *count_at(Counts *c, Hex h) {
    if (!c->n || h.row < c->r0 || h.row > c->r1 || h.col < c->c0 || h.col > c->c1) return NULL;
    return &c->n[(size_t)(h.row - c->r0) * (size_t)(c->c1 - c->c0 + 1) + (size_t)(h.col - c->c0)];
}

static bool visible(const Counts *c, NetHex h) {
    return h.row >= c->r0 && h.row <= c->r1 && h.col >= c->c0 && h.col <= c->c1;
}

static void draw_label(Hex anchor, int stack, const char *name, Color col) {
    float fs = (float)gh->console.fontSize;
    float ascent, descent;
    dr_font_metrics(FONT_BOLD, fs, &ascent, &descent);
    float w = dr_text_width(FONT_BOLD, fs, name) + fs, h = (ascent + descent) * 1.3f;
    Vec2 c = hex_center(anchor);
    float x = (float)c.x - w / 2, y = (float)(c.y - gh->hexSize) - h - 2 - stack * (h + 2);
    dr_fill_round_rect(x, y, w, h, h, h, col);
    dr_text(FONT_BOLD, fs, x + fs / 2, y + (h + ascent - descent) / 2, text_on(col), name);
}

static void dashed(Vec2 a, Vec2 b, float th, Color c) {
    double len = vec_dist(a, b), dash = gh->hexSize / 4.0;
    if (len <= 0 || dash < 2) dash = 2;
    for (double t = 0; t < len; t += dash * 2) {
        double t1 = fmin(t + dash, len);
        dr_line((float)(a.x + (b.x - a.x) * t / len), (float)(a.y + (b.y - a.y) * t / len),
                (float)(a.x + (b.x - a.x) * t1 / len), (float)(a.y + (b.y - a.y) * t1 / len), th, c);
    }
}

static void feet_label(Vec2 at, int feet) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%dft", feet);
    dr_text(FONT_BOLD, gh->hexSize / 2, (int)at.x, (int)at.y, COLOR_BLACK, buf);
}

static Vec2 world_pt(int32_t x, int32_t y) { return (Vec2){ presence_px_x(x), presence_px_y(y) }; }

static void draw_tools(const NetPresence *p, Color col) {
    float th = (float)gh->thickness;
    double hs = gh->hexSize;
    for (uint32_t i = 0; i < p->nlines; i++) {
        const NetLine *l = &p->lines[i];
        Vec2 a = world_pt(l->ax, l->ay), b = world_pt(l->bx, l->by);
        dr_line(a.x, a.y, b.x, b.y, th * 5, col);
        feet_label((Vec2){ a.x + (b.x - a.x) / 2, a.y + (b.y - a.y) / 2 }, l->label);
    }
    if (p->has_circle) {
        Vec2 o = world_pt(p->circle.x, p->circle.y);
        double r = (double)p->circle.radius * hs / NET_WORLD_SCALE;
        dr_stroke_oval((int)(o.x - r), (int)(o.y - r), (int)r * 2, (int)r * 2, th * 2, col);
        feet_label(o, p->circle.label);
    }
    if (p->has_cone) {
        Vec2 o = world_pt(p->cone.x, p->cone.y);
        double r = (double)p->cone.radius * hs / NET_WORLD_SCALE, inner = hs / 2;
        double start = p->cone_start, ext = p->cone_extent;
        dr_stroke_arc((int)(o.x - r), (int)(o.y - r), (int)r * 2, (int)r * 2, (float)start, (float)ext, th * 2, col);
        dr_stroke_arc((int)(o.x - inner), (int)(o.y - inner), (int)inner * 2, (int)inner * 2, (float)start,
                      (float)ext, th * 2, col);
        double edges[2] = { start, start + ext };
        for (int k = 0; k < 2; k++) {
            double ang = edges[k] * -M_PI / 180;
            dr_line((int)(o.x + cos(ang) * inner), (int)(o.y + sin(ang) * inner), (int)(o.x + cos(ang) * r),
                    (int)(o.y + sin(ang) * r), th * 2, col);
        }
        feet_label(o, p->cone.label);
    }
}

void presence_draw(const RemotePresence *list, int n) {
    if (n <= 0) return;
    float th = (float)gh->thickness;
    Counts cnt = { 0 };
    grid_visible_range(&cnt.r0, &cnt.r1, &cnt.c0, &cnt.c1);
    size_t cells = (size_t)(cnt.r1 - cnt.r0 + 1) * (size_t)(cnt.c1 - cnt.c0 + 1);
    if (cnt.r1 >= cnt.r0 && cnt.c1 >= cnt.c0 && cells <= (1u << 22)) cnt.n = calloc(cells, 1);
    /* label anchors so far, for stacking */
    Hex anchors[256];
    int nanchors = 0;

    for (int i = 0; i < n; i++) {
        const NetPresence *p = list[i].p;
        Color col = rgb_hex(list[i].color);
        Color fill = color_alpha(col, FILL_ALPHA);

        /* selected tiles: fill, then an outline inset by the number of earlier people on that tile */
        for (uint32_t k = 0; k < p->ntiles; k++) {
            if (!visible(&cnt, p->tiles[k])) continue;
            Hex h = { p->tiles[k].row, p->tiles[k].col };
            uint8_t *c = count_at(&cnt, h);
            int depth = c ? *c : 0;
            if (c && *c < 255) (*c)++;
            Vec2 pts[6];
            hex_inset(h, 0, pts);
            dr_fill_polygon(pts, 6, fill);
            outline_hex(h, 1 + depth * INSET_STEP, 2, col);
        }

        /* selected entities: a ring around each */
        bool have_anchor = false;
        Hex anchor = { 0, 0 };
        double ay = 0, ax = 0;
        for (uint32_t k = 0; k < p->nentities; k++) {
            Entity *e = entity_by_id(p->entities[k]);
            if (!e) continue;
            double s = entity_draw_size(e) * gh->hexSize;
            Vec2 c = hex_center(e->tile);
            if (e->type == ENTITY_CHARACTER) {
                double cx = e->location.x + s / 2, cy = e->location.y + s / 2;
                dr_stroke_oval((float)(cx - s / 2 - 2), (float)(cy - s / 2 - 2), (float)(s + 4), (float)(s + 4), 3, col);
            } else {
                Hex tiles[MAX_OCCUPIED];
                int m = entity_occupied_tiles(e, tiles);
                for (int t = 0; t < m; t++) outline_hex(tiles[t], 1, 3, col);
            }
            if (!have_anchor || c.y < ay || (c.y == ay && c.x < ax)) {
                have_anchor = true;
                anchor = e->tile;
                ay = c.y;
                ax = c.x;
            }
        }
        for (uint32_t k = 0; k < p->ntiles; k++) {
            Hex h = { p->tiles[k].row, p->tiles[k].col };
            Vec2 c = hex_center(h);
            if (!have_anchor || c.y < ay || (c.y == ay && c.x < ax)) {
                have_anchor = true;
                anchor = h;
                ay = c.y;
                ax = c.x;
            }
        }

        /* movement preview: dashed path, footprint at its end */
        for (uint32_t k = 0; k + 1 < p->npath; k++) {
            Vec2 a = hex_center((Hex){ p->path[k].row, p->path[k].col });
            Vec2 b = hex_center((Hex){ p->path[k + 1].row, p->path[k + 1].col });
            dashed(a, b, th * 3, col);
        }
        for (uint32_t k = 0; k < p->nfootprint; k++)
            outline_hex((Hex){ p->footprint[k].row, p->footprint[k].col }, 1, th + 2, col);

        draw_tools(p, col);

        if (have_anchor && list[i].name) {
            int stack = 0;
            for (int a = 0; a < nanchors; a++) stack += hex_eq(anchors[a], anchor);
            if (nanchors < (int)(sizeof(anchors) / sizeof(anchors[0]))) anchors[nanchors++] = anchor;
            draw_label(anchor, stack, list[i].name, col);
        }
    }
    free(cnt.n);
}
