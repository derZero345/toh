/* Toolbox (main/Toolbox, tools/Tool) and the measuring/attack tools (tools package). */
#include "app.h"
#include "image.h"

#include <math.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- toolbox ----------------------------------------------------------------------- */

static Texture *load_icon(const char *dir, const char *name) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/images/toolIcons/%s", dir, name);
    Image img;
    if (!image_load_png(path, &img)) return NULL;
    Texture *t = vkr_texture_create(img.w, img.h, img.px, false);
    image_free(&img);
    return t;
}

void toolbox_init(Toolbox *tb, const char *resource_dir) {
    static const struct { const char *file; int mode; } defs[] = {
        { "move.png",   DRAG_MODE },
        { "length.png", LENGTH_MODE },
        { "area.png",   AREA_MODE },
        { "cone.png",   CONE_MODE },
        { "line.png",   LINE_MODE },
    };
    tb->size = 32;
    tb->selected = -1;
    tb->ntools = 5;
    for (int i = 0; i < 5; i++) {
        Tool *t = &tb->tools[i];
        t->icon = load_icon(resource_dir, defs[i].file);
        t->mode = defs[i].mode;
        t->sort = i;
        t->hitbox = (Rect){ 1, i * 32 - 1, 30, 30 };
    }
}

void toolbox_free(Toolbox *tb) {
    for (int i = 0; i < tb->ntools; i++) vkr_texture_release(tb->tools[i].icon);
    tb->ntools = 0;
}

void toolbox_change_size(Toolbox *tb, int delta) {
    tb->size += delta;
    for (int i = 0; i < tb->ntools; i++) {
        Tool *t = &tb->tools[i];
        t->hitbox = (Rect){ 1, t->sort * tb->size - 1, tb->size - 2, tb->size - 2 };
    }
}

/* Target point for tools: exact mouse position with Ctrl, otherwise the tile centre. */
static Vec2 tool_target(void) {
    if (gh->isCtrlDown && gh->mouseActive) return gh->mousePos;
    return hex_center(gh->tileUnderMouse);
}

/* ---- measure ------------------------------------------------------------------------ */

bool measure_line(Measure *m, Vec2 *a, Vec2 *b) {
    if (!m->finished && !gh->hasTileUnderMouse) return false;
    *a = hex_center(m->origin);
    *b = hex_center(m->finished ? m->finishedPoint : gh->tileUnderMouse);
    return true;
}

bool measure_finish(Measure *m) {
    if (!m->finished) {
        if (!gh->hasTileUnderMouse) return false;
        m->finishedPoint = gh->tileUnderMouse;
        m->finished = true;
    }
    /* finishing on the origin (double click) clears all measurements */
    if (hex_eq(m->finishedPoint, m->origin)) {
        da_clear(&gh->measures);
        return true;
    }
    return false;
}

int measure_length(const Measure *m) {
    if (!m->finished) {
        if (!gh->hasTileUnderMouse) return 0;
        HexList path = { 0 };
        astar_path(m->origin, gh->tileUnderMouse, true, &path);
        int l = path.count;
        da_free(&path);
        return l == 0 ? 0 : (l - 1) * 5;
    }
    /* finished: straight-line distance between the grid coordinates */
    double dr = m->origin.row - m->finishedPoint.row, dc = m->origin.col - m->finishedPoint.col;
    return 5 * (int)sqrt(dr * dr + dc * dc);
}

/* ---- line attack ----------------------------------------------------------------------- */

bool line_get(LineAttack *l, Vec2 *a, Vec2 *b) {
    if (gh->hasTileUnderMouse) {
        l->a = hex_center(l->origin);
        l->b = tool_target();
        l->hasLine = true;
    }
    *a = l->a;
    *b = l->b;
    return l->hasLine;
}

int line_length(const LineAttack *l) {
    if (!gh->hasTileUnderMouse) return 0;
    HexList path = { 0 };
    astar_path(l->origin, gh->tileUnderMouse, true, &path);
    int n = path.count;
    da_free(&path);
    return n == 0 ? 0 : (n - 1) * 5;
}

void line_attacked(LineAttack *l) {
    da_clear(&gh->attackTiles);
    if (!l->hasLine) return;
    double spacing = gh->hexSize * SQRT3;
    double len = vec_dist(l->a, l->b);
    int n = (int)ceil(len / spacing) + 1;
    if (n < 2) return;
    for (int i = 0; i < n; i++) {
        double t = (double)i / (n - 1);
        Vec2 p = { l->a.x + t * (l->b.x - l->a.x), l->a.y + t * (l->b.y - l->a.y) };
        Hex h;
        if (hex_find_closest_any(p, &h)) da_push(&gh->attackTiles, h);
    }
}

/* ---- area attack ------------------------------------------------------------------------- */

double area_radius(AreaAttack *a) {
    if (!gh->hasTileUnderMouse) return a->radius;
    a->originPoint = hex_center(a->origin);
    a->radius = vec_dist(tool_target(), a->originPoint);
    return a->radius;
}

double area_radius_in_tiles(AreaAttack *a) {
    return (area_radius(a) + 1) / (gh->hexSize * SQRT3);
}

void area_attacked(AreaAttack *a) {
    da_clear(&gh->attackTiles);
    area_radius(a);
    a->originPoint = hex_center(a->origin);
    int r0, r1, c0, c1;
    grid_visible_range(&r0, &r1, &c0, &c1);
    for (int r = r0; r <= r1; r++)
        for (int c = c0; c <= c1; c++) {
            Hex h = { r, c };
            if (a->radius + 1 > vec_dist(hex_center(h), a->originPoint)) da_push(&gh->attackTiles, h);
        }
}

/* ---- cone attack ----------------------------------------------------------------------- */

static double angle_between(Vec2 p1, Vec2 p2) {
    double a = atan2(-(p2.y - p1.y), p2.x - p1.x) * 180 / M_PI; /* y grows downward */
    if (a < 0) a += 360;
    return a;
}

static bool angle_in_range(double test, double start, double range) {
    double end = fmod(start + range, 360);
    if (start <= end) return test >= start && test <= end;
    return test >= start || test <= end; /* range wraps around 0° */
}

double cone_radius(ConeAttack *c) {
    c->originPoint = hex_center(c->origin);
    if (!gh->hasTileUnderMouse) return c->radius;
    c->radius = vec_dist(tool_target(), c->originPoint);
    return c->radius;
}

double cone_radius_in_tiles(ConeAttack *c) {
    return (cone_radius(c) + 1) / (gh->hexSize * SQRT3);
}

double cone_start_angle(ConeAttack *c) {
    if (!gh->hasTileUnderMouse) return c->startAngle;
    double mouse_angle = angle_between(c->originPoint, tool_target());
    c->startAngle = fmod(mouse_angle - c->angle / 2, 360);
    if (c->startAngle < 0) c->startAngle += 360;
    return c->startAngle;
}

void cone_change_angle(ConeAttack *c, int notches) {
    c->angle = fmod(c->angle + notches * 5, 360);
    if (c->angle < 0) c->angle += 360;
}

void cone_attacked(ConeAttack *c) {
    da_clear(&gh->attackTiles);
    cone_radius(c);
    cone_start_angle(c);
    int r0, r1, c0, c1;
    grid_visible_range(&r0, &r1, &c0, &c1);
    for (int r = r0; r <= r1; r++)
        for (int col = c0; col <= c1; col++) {
            Hex h = { r, col };
            Vec2 hc = hex_center(h);
            double d = vec_dist(hc, c->originPoint);
            if (d < gh->hexSize) continue; /* skip the origin hex */
            if (d <= c->radius + 1 && angle_in_range(angle_between(c->originPoint, hc), c->startAngle, c->angle))
                da_push(&gh->attackTiles, h);
        }
}
