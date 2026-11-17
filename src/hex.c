/* Hexagon geometry and grid helpers (fx/Hexagon, calc/Calc, parts of GraphicsHandler). */
#include "app.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

int hl_index(const HexList *l, Hex h) {
    for (int i = 0; i < l->count; i++)
        if (hex_eq(l->items[i], h)) return i;
    return -1;
}

bool hl_contains(const HexList *l, Hex h) { return hl_index(l, h) >= 0; }

bool hl_remove(HexList *l, Hex h) {
    int i = hl_index(l, h);
    if (i < 0) return false;
    da_remove_at(l, i);
    return true;
}

void hl_copy(HexList *dst, const HexList *src) {
    da_clear(dst);
    for (int i = 0; i < src->count; i++) da_push(dst, src->items[i]);
}

/* ---- TileSet ---------------------------------------------------------------------- */

void ts_reset(TileSet *s, Hex center, int radius) {
    if (radius < 0) radius = 0;
    int side = 2 * radius + 1;
    size_t bytes = ((size_t)side * (size_t)side + 7) / 8;
    uint8_t *bits = realloc(s->bits, bytes);
    if (!bits) abort();
    memset(bits, 0, bytes);
    s->bits = bits;
    s->side = side;
    s->origin = (Hex){ center.row - radius, center.col - radius };
}

void ts_free(TileSet *s) {
    free(s->bits);
    *s = (TileSet){ 0 };
}

/* Bit index of h, or -1 when h lies outside the covered square. */
static long ts_index(const TileSet *s, Hex h) {
    int r = h.row - s->origin.row, c = h.col - s->origin.col;
    if (r < 0 || c < 0 || r >= s->side || c >= s->side) return -1;
    return (long)r * s->side + c;
}

bool ts_has(const TileSet *s, Hex h) {
    long i = ts_index(s, h);
    return i >= 0 && (s->bits[i >> 3] >> (i & 7)) & 1;
}

void ts_add(TileSet *s, Hex h) {
    long i = ts_index(s, h);
    if (i >= 0) s->bits[i >> 3] |= (uint8_t)(1u << (i & 7));
}

Vec2 hex_center(Hex h) {
    double hs = gh->hexSize;
    double hex_w = hs * SQRT3, hex_h = hs * 2;
    double cx, cy;
    if (gh->isFlat) {
        cx = gh->gridOffsetX + h.col * hex_w * SQRT3 / 2;
        cy = gh->gridOffsetY + h.row * hex_h * SQRT3 / 2;
        if (h.col % 2 != 0) cy += hex_w / 2; /* offset every other column */
    } else {
        cx = gh->gridOffsetX + h.col * hex_h * SQRT3 / 2;
        cy = gh->gridOffsetY + h.row * hex_w * SQRT3 / 2;
        if (h.row % 2 != 0) cx += hex_w / 2; /* offset every other row */
    }
    return (Vec2){ cx, cy };
}

void hex_corners(Hex h, Vec2 out[6]) {
    Vec2 c = hex_center(h);
    double rot = gh->isFlat ? 0 : M_PI / 6;
    for (int i = 0; i < 6; i++) {
        double a = 2 * M_PI / 6 * i + rot;
        out[i] = (Vec2){ c.x + gh->hexSize * cos(a), c.y + gh->hexSize * sin(a) };
    }
}

/* Same bounds as the grid generation in GraphicsHandler.gridPanel.paintComponent. */
void grid_visible_range(int *first_row, int *last_row, int *first_col, int *last_col) {
    double hs = gh->hexSize;
    double hex_w = hs * SQRT3, hex_h = hs * 2;
    double col_d = gh->isFlat ? hex_w : hex_h;
    double row_d = gh->isFlat ? hex_h : hex_w;
    *first_col = -2 + (int)floor((-gh->gridOffsetX - col_d) * 2 / SQRT3 / col_d);
    *last_col  =  2 + (int)ceil((gh->winW - gh->gridOffsetX - col_d) * 2 / SQRT3 / col_d);
    *first_row = -2 + (int)floor((-gh->gridOffsetY - row_d) * 1.16 / row_d);
    *last_row  =  2 + (int)ceil((gh->winH - gh->gridOffsetY - row_d) * 1.16 / row_d);
}

bool hex_visible(Hex h) {
    int r0, r1, c0, c1;
    grid_visible_range(&r0, &r1, &c0, &c1);
    return h.row >= r0 && h.row <= r1 && h.col >= c0 && h.col <= c1;
}

Hex hex_neighbor_raw(Hex h, int i) {
    static const int even[6][2] = { {-1, 0}, {-1, 1}, {0, 1}, {1, 0}, {0, -1}, {-1, -1} };
    static const int odd[6][2]  = { {-1, 0}, {0, 1}, {1, 1}, {1, 0}, {1, -1}, {0, -1} };
    const int (*t)[2] = (h.col % 2 == 0) ? even : odd;
    return (Hex){ h.row + t[i][0], h.col + t[i][1] };
}

bool hex_neighbor(Hex h, int number, Hex *out) {
    if (number < 1 || number > 6) return false;
    Hex n = hex_neighbor_raw(h, number - 1);
    if (!hex_visible(n)) return false;
    *out = n;
    return true;
}

Cube hex_to_cube(int row, int col) {
    int x = row - floor_div2(col);
    int z = col;
    return (Cube){ x, -x - z, z };
}

Hex cube_to_hex(Cube c) {
    return (Hex){ c.x + floor_div2(c.z), c.z };
}

int hex_distance(Hex a, Hex b) {
    Cube ca = hex_to_cube(a.row, a.col), cb = hex_to_cube(b.row, b.col);
    int dx = abs(ca.x - cb.x), dy = abs(ca.y - cb.y), dz = abs(ca.z - cb.z);
    int m = dx > dy ? dx : dy;
    return m > dz ? m : dz;
}

static bool find_closest(Vec2 p, bool visible_only, Hex *out) {
    double hs = gh->hexSize;
    int er, ec;
    if (gh->isFlat) {
        ec = (int)lround((p.x - gh->gridOffsetX) / (1.5 * hs));
        er = (int)lround((p.y - gh->gridOffsetY) / (SQRT3 * hs));
    } else {
        er = (int)lround((p.y - gh->gridOffsetY) / (1.5 * hs));
        ec = (int)lround((p.x - gh->gridOffsetX) / (SQRT3 * hs));
    }
    int r0, r1, c0, c1;
    grid_visible_range(&r0, &r1, &c0, &c1);
    double best = hs - 1;
    bool found = false;
    for (int r = er - 2; r <= er + 2; r++) {
        for (int c = ec - 2; c <= ec + 2; c++) {
            if (visible_only && (r < r0 || r > r1 || c < c0 || c > c1)) continue;
            Hex h = { r, c };
            double d = vec_dist(p, hex_center(h));
            if (d < best) {
                best = d;
                *out = h;
                found = true;
            }
        }
    }
    return found;
}

bool hex_find_closest(Vec2 p, Hex *out) { return find_closest(p, true, out); }

bool hex_find_closest_any(Vec2 p, Hex *out) { return find_closest(p, false, out); }
