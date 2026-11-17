/* A* pathfinding and movement range on the (unbounded) hex grid (calc/AStar). */
#include "app.h"

#include <float.h>
#include <string.h>

/* ---- occupancy lookup ------------------------------------------------------------ */

typedef struct {
    Hex h;
    Entity *e;
    bool used, multi; /* multi: more than one entity occupies this tile */
} OccSlot;

typedef struct {
    OccSlot *slots;
    uint32_t mask;
} OccTable;

static uint32_t hex_hash(Hex h) {
    return ((uint32_t)h.row * 73856093u) ^ ((uint32_t)h.col * 19349663u);
}

static OccTable occ_build(void) {
    int n = 0;
    for (int i = 0; i < gh->entities.count; i++) n += MAX_OCCUPIED;
    uint32_t cap = 64;
    while (cap < (uint32_t)n * 2) cap <<= 1;
    OccTable t = { calloc(cap, sizeof(OccSlot)), cap - 1 };
    if (!t.slots) abort();
    for (int i = 0; i < gh->entities.count; i++) {
        Entity *e = gh->entities.items[i];
        Hex tiles[MAX_OCCUPIED];
        int k = entity_occupied_tiles(e, tiles);
        for (int j = 0; j < k; j++) {
            uint32_t s = hex_hash(tiles[j]) & t.mask;
            while (t.slots[s].used && !hex_eq(t.slots[s].h, tiles[j])) s = (s + 1) & t.mask;
            if (!t.slots[s].used) {
                t.slots[s] = (OccSlot){ tiles[j], e, true, false };
            } else if (t.slots[s].e != e) {
                t.slots[s].multi = true;
            }
        }
    }
    return t;
}

/* ---- cached occupancy ---------------------------------------------------------------- */
/* Entities are changed from many places (moves, spawning, deleting, console edits), so
 * instead of tracking every change the cache keeps a snapshot of the fields the table
 * depends on and rebuilds only when the snapshot no longer matches. Comparing the
 * snapshot is much cheaper than rebuilding (no allocation, no hashing). */

typedef struct {
    const Entity *e;
    Hex tile;
    int size;
    EntityType type;
} OccSnap;

static struct {
    OccTable table;
    DA(OccSnap) snap;
    bool valid, has_tiles;
    Hex min, max; /* bounding box of all occupied tiles */
} OC;

static OccSnap snap_of(const Entity *e) {
    OccSnap s;
    memset(&s, 0, sizeof(s)); /* padding takes part in memcmp */
    s.e = e;
    s.tile = e->tile;
    s.size = e->size;
    s.type = e->type;
    return s;
}

static bool occ_cache_current(void) {
    if (!OC.valid || OC.snap.count != gh->entities.count) return false;
    for (int i = 0; i < gh->entities.count; i++) {
        OccSnap s = snap_of(gh->entities.items[i]);
        if (memcmp(&s, &OC.snap.items[i], sizeof(s)) != 0) return false;
    }
    return true;
}

static const OccTable *occ_get(void) {
    if (occ_cache_current()) return &OC.table;
    free(OC.table.slots);
    OC.table = occ_build();
    da_clear(&OC.snap);
    OC.has_tiles = false;
    for (int i = 0; i < gh->entities.count; i++) {
        Entity *e = gh->entities.items[i];
        da_push(&OC.snap, snap_of(e));
        Hex tiles[MAX_OCCUPIED];
        int k = entity_occupied_tiles(e, tiles);
        for (int j = 0; j < k; j++) {
            Hex h = tiles[j];
            if (!OC.has_tiles) { OC.min = OC.max = h; OC.has_tiles = true; continue; }
            if (h.row < OC.min.row) OC.min.row = h.row;
            if (h.col < OC.min.col) OC.min.col = h.col;
            if (h.row > OC.max.row) OC.max.row = h.row;
            if (h.col > OC.max.col) OC.max.col = h.col;
        }
    }
    OC.valid = true;
    return &OC.table;
}

static const OccSlot *occ_find(const OccTable *t, Hex h) {
    uint32_t s = hex_hash(h) & t->mask;
    while (t->slots[s].used) {
        if (hex_eq(t->slots[s].h, h)) return &t->slots[s];
        s = (s + 1) & t->mask;
    }
    return NULL;
}

/* Is h occupied by an entity other than `except`? */
static bool occ_other(const OccTable *t, Hex h, const Entity *except) {
    const OccSlot *s = occ_find(t, h);
    return s && (s->e != except || s->multi);
}

/* AStar.findEntityOccupyingHex: the first entity (in list order) covering h. The table
 * keeps the first entity inserted for each tile, and it is built in list order. */
static Entity *entity_occupying(const OccTable *t, Hex h) {
    const OccSlot *s = occ_find(t, h);
    return s ? s->e : NULL;
}

/* ---- search region ------------------------------------------------------------------ */
/* The grid is unbounded, so searches work on a rectangular region derived from the
 * involved tiles instead of the currently rendered part of the grid. That way paths
 * and movement ranges are the same no matter where the camera is or how far it is zoomed. */

typedef struct { int r0, r1, c0, c1, rows, cols; } Grid;

static Grid grid_around(Hex h, int margin) {
    return (Grid){ h.row - margin, h.row + margin, h.col - margin, h.col + margin, 2 * margin + 1, 2 * margin + 1 };
}

static void grid_include(Grid *g, Hex h, int margin) {
    if (h.row - margin < g->r0) g->r0 = h.row - margin;
    if (h.row + margin > g->r1) g->r1 = h.row + margin;
    if (h.col - margin < g->c0) g->c0 = h.col - margin;
    if (h.col + margin > g->c1) g->c1 = h.col + margin;
    g->rows = g->r1 - g->r0 + 1;
    g->cols = g->c1 - g->c0 + 1;
}

static inline bool grid_has(const Grid *g, Hex h) {
    return h.row >= g->r0 && h.row <= g->r1 && h.col >= g->c0 && h.col <= g->c1;
}
static inline int grid_idx(const Grid *g, Hex h) { return (h.row - g->r0) * g->cols + (h.col - g->c0); }

/* ---- per-tile search state (sparse, so large regions stay cheap) ------------------------ */

typedef struct {
    Hex h, came;
    double g;
    bool used, closed;
} NodeState;

typedef struct {
    NodeState *slots;
    uint32_t mask, count;
} StateMap;

static void sm_init(StateMap *m) {
    m->mask = 255;
    m->count = 0;
    m->slots = calloc(m->mask + 1, sizeof(NodeState));
    if (!m->slots) abort();
}

static NodeState *sm_get(StateMap *m, Hex h, bool create);

static void sm_grow(StateMap *m) {
    StateMap old = *m;
    m->mask = old.mask * 2 + 1;
    m->count = 0;
    m->slots = calloc(m->mask + 1, sizeof(NodeState));
    if (!m->slots) abort();
    for (uint32_t i = 0; i <= old.mask; i++)
        if (old.slots[i].used) *sm_get(m, old.slots[i].h, true) = old.slots[i];
    free(old.slots);
}

static NodeState *sm_get(StateMap *m, Hex h, bool create) {
    uint32_t s = hex_hash(h) & m->mask;
    while (m->slots[s].used) {
        if (hex_eq(m->slots[s].h, h)) return &m->slots[s];
        s = (s + 1) & m->mask;
    }
    if (!create) return NULL;
    if ((m->count + 1) * 2 > m->mask + 1) {
        sm_grow(m);
        return sm_get(m, h, true);
    }
    m->count++;
    m->slots[s] = (NodeState){ .h = h, .g = DBL_MAX, .used = true };
    return &m->slots[s];
}

/* ---- binary heap ------------------------------------------------------------------- */

typedef struct { double f; Hex h; } Node;
typedef DA(Node) Heap;

static void heap_push(Heap *h, Node n) {
    da_push(h, n);
    int i = h->count - 1;
    while (i > 0) {
        int p = (i - 1) / 2;
        if (h->items[p].f <= h->items[i].f) break;
        Node t = h->items[p]; h->items[p] = h->items[i]; h->items[i] = t;
        i = p;
    }
}

static Node heap_pop(Heap *h) {
    Node top = h->items[0];
    h->items[0] = h->items[--h->count];
    int i = 0;
    for (;;) {
        int l = 2 * i + 1, r = l + 1, m = i;
        if (l < h->count && h->items[l].f < h->items[m].f) m = l;
        if (r < h->count && h->items[r].f < h->items[m].f) m = r;
        if (m == i) break;
        Node t = h->items[m]; h->items[m] = h->items[i]; h->items[i] = t;
        i = m;
    }
    return top;
}

/* ---- A* --------------------------------------------------------------------------------- */

static bool astar_search(Hex start, Hex end, bool ignore_obstacles, int max_steps, HexList *path) {
    da_clear(path);
    if (max_steps > 0 && hex_distance(start, end) > max_steps) {
        if (gh->debugMode) console_log("No path found");
        return false;
    }

    /* Region: start, end and every entity, plus a margin so detours around
     * obstacles at the edge are possible. Outside of it all tiles are free, so a
     * shortest path never has to leave it; it also bounds the search when the
     * target is unreachable. */
    Grid g = grid_around(start, 2);
    grid_include(&g, end, 2);

    const OccTable *occ = NULL;
    Entity *source = NULL;
    if (!ignore_obstacles) {
        occ = occ_get();
        source = entity_occupying(occ, start);
        if (OC.has_tiles) {
            grid_include(&g, OC.min, 2);
            grid_include(&g, OC.max, 2);
        }
    }
    if (max_steps > 0) {
        /* a path of at most max_steps steps can't leave start +/- max_steps */
        if (g.r0 < start.row - max_steps) g.r0 = start.row - max_steps;
        if (g.r1 > start.row + max_steps) g.r1 = start.row + max_steps;
        if (g.c0 < start.col - max_steps) g.c0 = start.col - max_steps;
        if (g.c1 > start.col + max_steps) g.c1 = start.col + max_steps;
    }

    StateMap st;
    sm_init(&st);
    Heap open = { 0 };
    sm_get(&st, start, true)->g = 0;
    heap_push(&open, (Node){ hex_distance(start, end), start });
    bool found = false;

    while (open.count > 0) {
        Node cur = heap_pop(&open);
        NodeState *cs = sm_get(&st, cur.h, false);
        if (cs->closed) continue;
        if (hex_eq(cur.h, end)) { found = true; break; }
        cs->closed = true;
        double cg = cs->g;
        for (int k = 0; k < 6; k++) {
            Hex nb = hex_neighbor_raw(cur.h, k);
            if (!grid_has(&g, nb)) continue;
            if (!ignore_obstacles && occ_other(occ, nb, source)) continue;
            double tg = cg + 1;
            /* hex_distance never overestimates, so this can't cut off a valid path */
            if (max_steps > 0 && tg + hex_distance(nb, end) > max_steps) continue;
            NodeState *ns = sm_get(&st, nb, true);
            if (ns->closed) continue;
            if (tg < ns->g) {
                ns->g = tg;
                ns->came = cur.h;
                heap_push(&open, (Node){ tg + hex_distance(nb, end), nb });
            }
        }
    }

    if (found) {
        if (gh->debugMode) {
            char num[32];
            fmt_java_double(num, sizeof(num), sm_get(&st, end, false)->g);
            console_log("Shortest path is %s tiles long.", num);
        }
        /* walk back from end to start, then reverse */
        for (Hex h = end; !hex_eq(h, start); h = sm_get(&st, h, false)->came) da_push(path, h);
        if (path->count > 0) da_push(path, start);
        for (int a = 0, b = path->count - 1; a < b; a++, b--) {
            Hex t = path->items[a]; path->items[a] = path->items[b]; path->items[b] = t;
        }
    } else if (gh->debugMode) {
        console_log("No path found");
    }

    free(st.slots);
    da_free(&open);
    return found;
}

bool astar_path(Hex start, Hex end, bool ignore_obstacles, HexList *path) {
    return astar_search(start, end, ignore_obstacles, 0, path);
}

bool astar_path_within(Hex start, Hex end, int max_steps, HexList *path) {
    if (max_steps <= 0) {
        da_clear(path);
        return hex_eq(start, end);
    }
    return astar_search(start, end, false, max_steps, path);
}

/* ---- movement range ------------------------------------------------------------------ */

void astar_range(Hex center, int speed, Entity *moving, TileSet *out) {
    if (speed > MAX_SPEED) speed = MAX_SPEED;
    if (speed < 0) speed = 0;
    /* each step changes row and column by at most one, so the result fits in
     * centre +/- speed; the set doubles as the "already visited" marker */
    ts_reset(out, center, speed);
    if (speed == 0) return;
    const OccTable *occ = occ_get();
    Entity *center_entity = moving ? NULL : entity_occupying(occ, center);

    Hex own[MAX_OCCUPIED];
    int nown = moving ? entity_occupied_tiles(moving, own) : 0;

    HexList current = { 0 }, next = { 0 };
    da_push(&current, center);
    for (int step = 0; step < speed; step++) {
        da_clear(&next);
        for (int i = 0; i < current.count; i++) {
            for (int k = 0; k < 6; k++) {
                Hex hex = hex_neighbor_raw(current.items[i], k);
                if (hex_eq(hex, center) || ts_has(out, hex)) continue;

                bool valid = true;
                if (moving) {
                    /* every tile the character would cover must be free of other entities
                     * (tiles it currently covers itself are not checked) */
                    Hex target[MAX_OCCUPIED];
                    int nt = character_occupied_tiles(hex, moving->size, target);
                    for (int t = 0; t < nt && valid; t++) {
                        bool own_tile = false;
                        for (int o = 0; o < nown; o++)
                            if (hex_eq(own[o], target[t])) { own_tile = true; break; }
                        if (!own_tile && occ_other(occ, target[t], moving)) valid = false;
                    }
                } else {
                    valid = !occ_other(occ, hex, center_entity);
                }
                if (valid) {
                    ts_add(out, hex);
                    da_push(&next, hex);
                }
            }
        }
        HexList tmp = current; current = next; next = tmp;
    }
    da_free(&current);
    da_free(&next);
}
