/* The radial menu's controller: config check, rules, levels, the ring animation, hit-testing, input and events.
 * No game state here; see radial.h for how the parts fit together. */
#include "radial.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define T (&HUD_THEME)
#define MAX_RINGS 8

/* ---- entry kinds: what activating an entry does ---- */

static unsigned opens; /* counts radial_open calls, so an action can tell whether its handler opened a menu */

static void activate_action(const RadialEntry *e) {
    unsigned before = opens;
    radial_emit_action(e);
    if (opens == before) radial_close(); /* unless the handler opened another menu */
}

static void activate_submenu(const RadialEntry *e) { radial_enter(e); }

const RadialKindDef RADIAL_KINDS[RADIAL_KIND_COUNT] = {
    [RADIAL_ACTION] = { false, activate_action },
    [RADIAL_SUBMENU] = { true, activate_submenu },
    [RADIAL_TOGGLE] = { false, radial_toggle },
};

/* the toggles that are on in the open menu */
#define MAX_ON (RADIAL_MAX_ENTRIES * (RADIAL_MAX_DEPTH + 1))
static const RadialEntry *on_entries[MAX_ON];
static int non;

static int on_index(const RadialEntry *e) {
    for (int i = 0; i < non; i++)
        if (on_entries[i] == e) return i;
    return -1;
}

static bool entry_on(const RadialEntry *e) { return on_index(e) >= 0; }

static void refresh_enabled(void);

void radial_toggle(const RadialEntry *e) {
    int i = on_index(e);
    if (i >= 0) on_entries[i] = on_entries[--non];
    else if (non < MAX_ON) on_entries[non++] = e;
    refresh_enabled(); /* rules may ask for toggles (the centre's "choose one first") */
}

bool radial_toggled(const char *id) {
    for (int i = 0; id && i < non; i++)
        if (strcmp(on_entries[i]->id, id) == 0) return true;
    return false;
}

/* ---- config and rules ---- */

static const RadialMenu *menus = RADIAL_MENUS;
static int nmenus = -1; /* -1: RADIAL_NMENUS (not a constant expression) */
static const RadialRule *rules;
static int nrules;
static bool config_checked, config_ok;

static int menu_count(void) { return nmenus < 0 ? RADIAL_NMENUS : nmenus; }

static const RadialRule *find_rule(const RadialRule *r, int n, const char *spec) {
    if (!spec) return NULL;
    if (spec[0] == '!') spec++;
    for (int i = 0; i < n; i++)
        if (r[i].key && strcmp(r[i].key, spec) == 0) return &r[i];
    return NULL;
}

typedef struct {
    const RadialRule *rules;
    int nrules;
    const char *ids[256]; /* of the menu being checked */
    int nids;
    char *err;
    size_t cap;
} Check;

static int check_id(Check *c, const char *menu, const char *id) {
    if (!id || !id[0]) {
        snprintf(c->err, c->cap, "menu \"%s\": an entry without an id", menu);
        return -1;
    }
    for (int i = 0; i < c->nids; i++)
        if (strcmp(c->ids[i], id) == 0) {
            snprintf(c->err, c->cap, "menu \"%s\": id \"%s\" is used twice", menu, id);
            return -1;
        }
    if (c->nids == (int)(sizeof(c->ids) / sizeof(c->ids[0]))) {
        snprintf(c->err, c->cap, "menu \"%s\": more than %d entries", menu, c->nids);
        return -1;
    }
    c->ids[c->nids++] = id;
    return 0;
}

static int check_entry(Check *c, const char *menu, const RadialEntry *e, int level);

/* level: 0 = the top ring, 1 = a submenu of it, ... */
static int check_ring(Check *c, const char *menu, const RadialEntry *e, int n, int level) {
    if (!e || n < 1 || n > RADIAL_MAX_ENTRIES) {
        snprintf(c->err, c->cap, "menu \"%s\": a ring with %d entries (1 to %d)", menu, n, RADIAL_MAX_ENTRIES);
        return -1;
    }
    for (int i = 0; i < n; i++)
        if (check_entry(c, menu, &e[i], level) != 0) return -1;
    return 0;
}

static int check_entry(Check *c, const char *menu, const RadialEntry *e, int level) {
    if (check_id(c, menu, e->id) != 0) return -1;
    if (!e->icon || !e->icon[0]) {
        snprintf(c->err, c->cap, "\"%s\": no icon key", e->id);
        return -1;
    }
    if ((int)e->kind < 0 || e->kind >= RADIAL_KIND_COUNT || !RADIAL_KINDS[e->kind].activate) {
        snprintf(c->err, c->cap, "\"%s\": unknown kind", e->id);
        return -1;
    }
    if ((int)e->accent < 0 || e->accent >= HUD_ACCENT_COUNT) {
        snprintf(c->err, c->cap, "\"%s\": unknown accent", e->id);
        return -1;
    }
    const char *conds[2] = { e->visible_if, e->enabled_if };
    for (int k = 0; k < 2; k++)
        if (conds[k] && !find_rule(c->rules, c->nrules, conds[k])) {
            snprintf(c->err, c->cap, "\"%s\": unknown rule \"%s\"", e->id, conds[k]);
            return -1;
        }
    if (e->kind == RADIAL_SUBMENU) {
        if (level >= RADIAL_MAX_DEPTH) {
            snprintf(c->err, c->cap, "\"%s\": submenus nest deeper than %d levels", e->id, RADIAL_MAX_DEPTH);
            return -1;
        }
        if (!e->children && e->nchildren == 0) return 0; /* a placeholder: shown disabled */
        return check_ring(c, menu, e->children, e->nchildren, level + 1);
    }
    if (e->children || e->nchildren) {
        snprintf(c->err, c->cap, "\"%s\": only a submenu has children", e->id);
        return -1;
    }
    return 0;
}

int radial_config_check(const RadialMenu *m, int n, const RadialRule *r, int nr, char *err, size_t cap) {
    Check c = { .rules = r, .nrules = nr, .err = err, .cap = cap };
    if (n < 1) {
        snprintf(err, cap, "no menus");
        return -1;
    }
    for (int i = 0; i < n; i++) {
        if (!m[i].id || !m[i].id[0]) {
            snprintf(err, cap, "menu %d has no id", i + 1);
            return -1;
        }
        for (int k = 0; k < i; k++)
            if (strcmp(m[k].id, m[i].id) == 0) {
                snprintf(err, cap, "menu id \"%s\" is used twice", m[i].id);
                return -1;
            }
        c.nids = 0;
        if (m[i].center) {
            if (check_entry(&c, m[i].id, m[i].center, 0) != 0) return -1;
            if (m[i].center->kind != RADIAL_ACTION) {
                snprintf(err, cap, "menu \"%s\": the centre must be an action", m[i].id);
                return -1;
            }
        }
        if (check_ring(&c, m[i].id, m[i].entries, m[i].count, 0) != 0) return -1;
    }
    return 0;
}

static void reset(void);

void radial_set_rules(const RadialRule *r, int n) {
    rules = r;
    nrules = r ? n : 0;
    config_checked = false;
}

void radial_use_config(const RadialMenu *m, int n) {
    menus = m ? m : RADIAL_MENUS;
    nmenus = m ? n : -1;
    config_checked = false;
    reset();
}

/* Checks the config once per change; an invalid one is reported on stderr and no menu opens. */
static bool cfg(void) {
    if (!config_checked) {
        char err[160];
        config_ok = radial_config_check(menus, menu_count(), rules, nrules, err, sizeof(err)) == 0;
        if (!config_ok) fprintf(stderr, "Radial menu config: %s\n", err);
        config_checked = true;
    }
    return config_ok;
}

/* ---- state ---- */

typedef struct {
    const RadialEntry *parent; /* whose ring this is; NULL = the top level */
    const RadialEntry *items[RADIAL_MAX_ENTRIES];
    const char *why[RADIAL_MAX_ENTRIES]; /* NULL = enabled */
    int n;
} Level;

/* ALONE: the top level while no submenu is open; INNER: the parent level; OUTER: the submenu; GONE: fading out */
typedef enum { ROLE_ALONE, ROLE_INNER, ROLE_OUTER, ROLE_GONE } Role;

typedef struct {
    Level lv;
    Role role;
    int highlight;
    RadialGeo from, to, cur;
} Ring;

typedef struct { float from, to, cur; } Anim;

typedef enum { PART_NONE, PART_CENTER, PART_RING, PART_PARENT } PartKind;
typedef struct {
    PartKind kind;
    int idx;
} Part;

static enum { ST_CLOSED, ST_OPEN, ST_CLOSING } state;
static const RadialMenu *menu;
static RadialTarget target;
static float wantX, wantY; /* where it was asked to be; drawn moved inside the window */
static RadialScreen screen = { 800, 600, 1 };
static int depth;
static const RadialEntry *path[RADIAL_MAX_DEPTH]; /* path[k]: the submenu entry opened on level k */
static Ring rings[MAX_RINGS];
static int nrings;
static float t = 1, durMs; /* transition progress 0..1 */
static Anim centerAlpha, centerBack;
static const char *centerWhy;
static Part hover, press;
static bool pressActive;
static float hoverX, hoverY;
static void (*handler)(const RadialEvent *ev);

static void reset(void) {
    state = ST_CLOSED;
    menu = NULL;
    non = 0; /* every toggle starts off */
    depth = nrings = 0;
    t = 1;
    hover = press = (Part){ PART_NONE, -1 };
    pressActive = false;
}

void radial_set_event_handler(void (*fn)(const RadialEvent *ev)) { handler = fn; }

static void send(RadialEventType type, const char *id) {
    RadialEvent ev = { type, id, menu ? menu->id : NULL, target };
    if (handler) handler(&ev);
}

void radial_set_screen(const RadialScreen *s) { screen = *s; }

static float sc(void) { return screen.scale > 0 ? screen.scale : 1; }

/* ---- rules and levels ---- */

/* true if the condition holds; else *why says why not */
static bool rule_holds(const char *spec, const char **why) {
    if (!spec) return true;
    const RadialRule *r = find_rule(rules, nrules, spec);
    bool neg = spec[0] == '!';
    if (!r || (!r->check && !r->fn)) {
        if (why) *why = "Unknown rule";
        return false;
    }
    /* the reason it fails, NULL when it holds */
    const char *reason = r->check ? r->check(&target, r->arg) : r->fn(&target) ? NULL : r->unmet;
    bool holds = reason == NULL;
    if (holds == neg && why) *why = neg ? r->met : reason;
    return holds != neg;
}

static const RadialEntry *level_entries(const RadialEntry *parent, int *n) {
    *n = parent ? parent->nchildren : menu->count;
    return parent ? parent->children : menu->entries;
}

static bool any_visible(const RadialEntry *parent) {
    int n;
    const RadialEntry *e = level_entries(parent, &n);
    for (int i = 0; i < n; i++)
        if (rule_holds(e[i].visible_if, NULL)) return true;
    return false;
}

static const char *why_disabled(const RadialEntry *e) {
    const char *why = NULL;
    if (!rule_holds(e->enabled_if, &why)) return why ? why : "Not available";
    if (e->kind == RADIAL_SUBMENU && !any_visible(e)) return "Nothing to choose here";
    return NULL;
}

static void build_level(Level *lv, const RadialEntry *parent) {
    int n;
    const RadialEntry *e = level_entries(parent, &n);
    lv->parent = parent;
    lv->n = 0;
    for (int i = 0; i < n && lv->n < RADIAL_MAX_ENTRIES; i++)
        if (rule_holds(e[i].visible_if, NULL)) lv->items[lv->n++] = &e[i];
    for (int i = 0; i < lv->n; i++) lv->why[i] = why_disabled(lv->items[i]);
}

static int level_index(const Level *lv, const RadialEntry *e) {
    for (int i = 0; i < lv->n; i++)
        if (lv->items[i] == e) return i;
    return -1;
}

/* ---- rings and their animation ---- */

static RadialGeo role_geo(Role r) {
    switch (r) {
    case ROLE_ALONE: return (RadialGeo){ T->radialAloneIn, T->radialAloneOut, 1 };
    case ROLE_INNER: return (RadialGeo){ T->radialInnerIn, T->radialInnerOut, 1 };
    case ROLE_OUTER: return (RadialGeo){ T->radialOuterIn, T->radialOuterOut, 1 };
    default: return (RadialGeo){ 0, 0, 0 };
    }
}

/* a submenu ring grows out of this and shrinks back into it: hugging the inner ring, invisible */
static RadialGeo outer_hidden(void) {
    return (RadialGeo){ T->radialInnerOut, T->radialInnerOut + 0.6f * (T->radialOuterOut - T->radialOuterIn), 0 };
}

/* a grandparent ring shrinks into the centre button */
static RadialGeo inner_hidden(void) { return (RadialGeo){ T->radialCenterR, T->radialInnerIn, 0 }; }

static int ring_with(Role r) {
    for (int i = 0; i < nrings; i++)
        if (rings[i].role == r) return i;
    return -1;
}

static int current_ring(void) { return ring_with(depth > 0 ? ROLE_OUTER : ROLE_ALONE); }
static int parent_ring(void) { return depth > 0 ? ring_with(ROLE_INNER) : -1; }

/* Starts a transition: everything moves from where it is now. Set the targets after this. */
static void begin(float ms) {
    for (int i = 0; i < nrings; i++) rings[i].from = rings[i].cur;
    centerAlpha.from = centerAlpha.cur;
    centerBack.from = centerBack.cur;
    t = 0;
    durMs = ms;
}

static void drop_ring(int i) {
    memmove(&rings[i], &rings[i + 1], (size_t)(nrings - i - 1) * sizeof(Ring));
    nrings--;
}

/* A new ring that starts at `from` (after begin, so it is not overwritten). Full: the oldest fading ring goes. */
static Ring *add_ring(Role role, RadialGeo from, const RadialEntry *parent) {
    if (nrings == MAX_RINGS) {
        int gone = ring_with(ROLE_GONE);
        drop_ring(gone >= 0 ? gone : 0);
    }
    Ring *r = &rings[nrings++];
    memset(r, 0, sizeof(*r));
    r->role = role;
    r->highlight = -1;
    r->from = r->cur = from;
    r->to = role_geo(role);
    build_level(&r->lv, parent);
    return r;
}

static float ease(float x) { return 1 - (1 - x) * (1 - x) * (1 - x); } /* ease-out cubic */
static float lerp(float a, float b, float u) { return a + (b - a) * u; }

static void step_to(float u) {
    for (int i = 0; i < nrings; i++) {
        Ring *r = &rings[i];
        r->cur = (RadialGeo){ lerp(r->from.rin, r->to.rin, u), lerp(r->from.rout, r->to.rout, u),
                              lerp(r->from.alpha, r->to.alpha, u) };
    }
    centerAlpha.cur = lerp(centerAlpha.from, centerAlpha.to, u);
    centerBack.cur = lerp(centerBack.from, centerBack.to, u);
}

/* ---- opening, levels, closing ---- */

static const RadialMenu *find_menu(const char *id) {
    for (int i = 0; id && i < menu_count(); i++)
        if (strcmp(menus[i].id, id) == 0) return &menus[i];
    return NULL;
}

bool radial_open(const char *menu_id, const RadialTarget *tg, float x, float y) {
    reset();
    const RadialMenu *m = cfg() ? find_menu(menu_id) : NULL;
    if (!m || !tg) return false;
    menu = m;
    target = *tg;
    wantX = x;
    wantY = y;
    opens++;
    Level top;
    build_level(&top, NULL);
    if (top.n == 0) {
        reset();
        return false;
    }
    state = ST_OPEN;
    RadialGeo g = role_geo(ROLE_ALONE);
    float f = T->radialOpenScale > 0 ? T->radialOpenScale : 1;
    add_ring(ROLE_ALONE, (RadialGeo){ g.rin * f, g.rout * f, 0 }, NULL); /* grows out of a smaller ring */
    centerAlpha.cur = 0;
    centerBack.cur = 0;
    begin(T->radialOpenMs);
    centerAlpha.to = 1;
    centerBack.to = 0;
    centerWhy = menu->center ? why_disabled(menu->center) : NULL;
    return true;
}

void radial_close(void) {
    if (state != ST_OPEN) return;
    begin(T->radialCloseMs);
    for (int i = 0; i < nrings; i++) {
        Ring *r = &rings[i];
        r->role = ROLE_GONE;
        r->to = (RadialGeo){ r->cur.rin * 0.9f, r->cur.rout * 0.9f, 0 };
    }
    centerAlpha.to = 0;
    centerBack.to = centerBack.cur;
    state = ST_CLOSING;
    hover = press = (Part){ PART_NONE, -1 };
    pressActive = false;
    send(RADIAL_EV_CLOSED, NULL);
}

static void go_back(void) {
    if (state != ST_OPEN || depth == 0) return;
    int cur = current_ring(), par = parent_ring();
    begin(T->radialMoveMs);
    if (cur >= 0) {
        rings[cur].role = ROLE_GONE;
        rings[cur].to = outer_hidden();
    }
    depth--;
    if (par >= 0) { /* the parent grows back into the current ring */
        rings[par].role = depth > 0 ? ROLE_OUTER : ROLE_ALONE;
        rings[par].to = role_geo(rings[par].role);
        rings[par].highlight = -1;
    }
    if (depth > 0) { /* the grandparent comes back out of the centre */
        Ring *g = add_ring(ROLE_INNER, inner_hidden(), depth >= 2 ? path[depth - 2] : NULL);
        g->highlight = level_index(&g->lv, path[depth - 1]);
    }
    centerBack.to = depth > 0;
}

void radial_enter(const RadialEntry *e) {
    if (state != ST_OPEN || !e || e->kind != RADIAL_SUBMENU) return;
    int cur = current_ring(), par = parent_ring(), idx;
    if (cur >= 0 && (idx = level_index(&rings[cur].lv, e)) >= 0) { /* one level deeper */
        if (depth >= RADIAL_MAX_DEPTH) return;
        begin(T->radialMoveMs);
        if (par >= 0) {
            rings[par].role = ROLE_GONE;
            rings[par].to = inner_hidden();
        }
        rings[cur].role = ROLE_INNER;
        rings[cur].to = role_geo(ROLE_INNER);
        rings[cur].highlight = idx;
        path[depth++] = e;
        add_ring(ROLE_OUTER, outer_hidden(), e);
        centerBack.to = 1;
    } else if (par >= 0 && (idx = level_index(&rings[par].lv, e)) >= 0) {
        if (idx == rings[par].highlight) { /* the open one again: back */
            go_back();
            return;
        }
        begin(T->radialMoveMs); /* another submenu of the parent: swap the outer ring */
        if (cur >= 0) {
            rings[cur].role = ROLE_GONE;
            rings[cur].to = outer_hidden();
        }
        rings[par].highlight = idx;
        path[depth - 1] = e;
        add_ring(ROLE_OUTER, outer_hidden(), e);
    }
}

void radial_emit_action(const RadialEntry *e) {
    if (state == ST_OPEN && e) send(RADIAL_EV_ACTION, e->id);
}

bool radial_is_open(void) { return state == ST_OPEN; }
const RadialTarget *radial_target(void) { return &target; }
const char *radial_menu_id(void) { return state == ST_CLOSED || !menu ? NULL : menu->id; }
int radial_depth(void) { return state == ST_OPEN ? depth : 0; }
const char *radial_submenu_id(void) { return state == ST_OPEN && depth > 0 ? path[depth - 1]->id : NULL; }

void radial_move_to(float x, float y) {
    wantX = x;
    wantY = y;
}

/* the centre, moved so that the largest ring stays inside the window */
static void centre(float *x, float *y) {
    float e = (T->radialOuterOut + T->radialHoverPop + T->radialMargin) * sc();
    *x = screen.winW < 2 * e ? screen.winW / 2 : fminf(fmaxf(wantX, e), screen.winW - e);
    *y = screen.winH < 2 * e ? screen.winH / 2 : fminf(fmaxf(wantY, e), screen.winH - e);
}

/* ---- hit-testing (against where things are going, so a click mid-animation means what the user sees next) ---- */

static Part part_at(float x, float y) {
    Part none = { PART_NONE, -1 };
    if (state != ST_OPEN) return none;
    float ox, oy, s = sc();
    centre(&ox, &oy);
    float dx = x - ox, dy = y - oy, d = sqrtf(dx * dx + dy * dy) / s;
    if (d <= T->radialCenterR) return (Part){ PART_CENTER, 0 };
    int idx[2] = { current_ring(), parent_ring() };
    for (int k = 0; k < 2; k++) {
        if (idx[k] < 0) continue;
        const Ring *r = &rings[idx[k]];
        RadialGeo g = role_geo(r->role);
        float out = g.rout + (k == 0 ? T->radialHoverPop : 0);
        if (r->lv.n > 0 && d >= g.rin - T->radialSegGap / 2 && d <= out)
            return (Part){ k == 0 ? PART_RING : PART_PARENT, radial_segment_at(r->lv.n, dx, dy) };
    }
    return none;
}

bool radial_contains(float x, float y) {
    if (state != ST_OPEN) return false;
    float ox, oy, s = sc();
    centre(&ox, &oy);
    float dx = x - ox, dy = y - oy;
    Role r = depth > 0 ? ROLE_OUTER : ROLE_ALONE;
    float out = (role_geo(r).rout + T->radialHoverPop) * s;
    return dx * dx + dy * dy <= out * out;
}

static const Level *part_level(Part p) {
    int i = p.kind == PART_RING ? current_ring() : p.kind == PART_PARENT ? parent_ring() : -1;
    return i >= 0 ? &rings[i].lv : NULL;
}

static void activate(Part p) {
    if (p.kind == PART_CENTER) {
        if (depth > 0) go_back();
        else if (!menu->center) radial_close();
        else if (!centerWhy) RADIAL_KINDS[menu->center->kind].activate(menu->center);
        return;
    }
    const Level *lv = part_level(p);
    if (!lv || p.idx < 0 || p.idx >= lv->n || lv->why[p.idx]) return; /* disabled: nothing happens */
    const RadialEntry *e = lv->items[p.idx];
    RADIAL_KINDS[e->kind].activate(e);
}

static bool same_part(Part a, Part b) { return a.kind == b.kind && a.idx == b.idx; }

/* ---- input ---- */

bool radial_mouse_down(float x, float y) {
    radial_cancel_press(); /* a press whose release was lost must not stay captured */
    if (state != ST_OPEN) return false;
    Part p = part_at(x, y);
    hoverX = x;
    hoverY = y;
    hover = p;
    if (p.kind == PART_NONE && !radial_contains(x, y)) {
        radial_close(); /* a click outside */
        return true;
    }
    press = p; /* on a gap inside the menu: a press of nothing, so its release is kept from the map too */
    pressActive = true;
    return true;
}

bool radial_mouse_move(float x, float y) {
    if (state != ST_OPEN) return false;
    hoverX = x;
    hoverY = y;
    hover = part_at(x, y);
    return pressActive || hover.kind != PART_NONE || radial_contains(x, y);
}

bool radial_mouse_up(float x, float y) {
    if (!pressActive) return false;
    Part pressed = press;
    pressActive = false;
    press = (Part){ PART_NONE, -1 };
    if (state == ST_OPEN && same_part(part_at(x, y), pressed)) activate(pressed);
    if (state == ST_OPEN) radial_mouse_move(x, y); /* the parts may have moved */
    return true;
}

void radial_cancel_press(void) {
    pressActive = false;
    press = (Part){ PART_NONE, -1 };
}

void radial_clear_hover(void) { hover = (Part){ PART_NONE, -1 }; }

/* ---- frame ---- */

static void refresh_enabled(void) {
    for (int i = 0; i < nrings; i++)
        if (rings[i].role != ROLE_GONE)
            for (int k = 0; k < rings[i].lv.n; k++) rings[i].lv.why[k] = why_disabled(rings[i].lv.items[k]);
    centerWhy = menu && menu->center ? why_disabled(menu->center) : NULL;
}

bool radial_update(float dt) {
    if (state == ST_CLOSED) return false;
    if (state == ST_OPEN) refresh_enabled(); /* enabled can change while it is open; visibility only when a ring opens */
    if (t < 1) {
        t += durMs > 0 ? dt * 1000 / durMs : 1;
        if (t > 1) t = 1;
    }
    step_to(ease(t));
    if (t >= 1) {
        for (int i = nrings - 1; i >= 0; i--)
            if (rings[i].role == ROLE_GONE) drop_ring(i);
        if (state == ST_CLOSING) reset();
    }
    return t < 1;
}

static RadialRingView ring_view(const Ring *r, float ox, float oy) {
    PartKind kind = r->role == ROLE_INNER ? PART_PARENT : r->role == ROLE_GONE ? PART_NONE : PART_RING;
    bool live = state == ST_OPEN && kind != PART_NONE;
    RadialRingView v = { ox, oy, sc(), r->cur, r->lv.n, r->lv.items, r->lv.why, -1, -1, r->highlight, live, entry_on };
    if (live && hover.kind == kind) v.hover = hover.idx;
    if (live && pressActive && press.kind == kind && same_part(hover, press)) v.press = press.idx;
    return v;
}

void radial_draw(void) {
    if (state == ST_CLOSED) return;
    float ox, oy, s = sc();
    centre(&ox, &oy);
    /* fading rings first, then the parent, then the current one on top */
    Role order[] = { ROLE_GONE, ROLE_INNER, ROLE_ALONE, ROLE_OUTER };
    for (int o = 0; o < 4; o++)
        for (int i = 0; i < nrings; i++)
            if (rings[i].role == order[o]) {
                RadialRingView v = ring_view(&rings[i], ox, oy);
                radial_draw_ring(&v);
            }
    bool open = state == ST_OPEN, hc = open && hover.kind == PART_CENTER;
    bool disabled = depth == 0 && menu->center && centerWhy;
    radial_draw_center(ox, oy, s, centerAlpha.cur, menu->center, centerBack.cur, hc,
                       hc && pressActive && press.kind == PART_CENTER, disabled);

    if (!open || hover.kind == PART_NONE) return;
    const char *label = NULL, *why = NULL;
    if (hover.kind == PART_CENTER) {
        label = depth > 0 ? "Back" : menu->center ? menu->center->label : "Close";
        why = disabled ? centerWhy : NULL;
    } else {
        const Level *lv = part_level(hover);
        if (lv && hover.idx >= 0 && hover.idx < lv->n) {
            label = lv->items[hover.idx]->label;
            why = lv->why[hover.idx];
        }
    }
    radial_draw_tooltip(hoverX, hoverY, s, screen.winW, screen.winH, label, why);
}

bool radial_part_point(const char *part, float *x, float *y) {
    if (state != ST_OPEN) return false;
    float ox, oy, s = sc();
    centre(&ox, &oy);
    if (strcmp(part, "center") == 0) {
        *x = ox;
        *y = oy;
        return true;
    }
    int ri = -1;
    const char *num = NULL;
    if (strncmp(part, "ring.", 5) == 0) {
        ri = current_ring();
        num = part + 5;
    } else if (strncmp(part, "parent.", 7) == 0) {
        ri = parent_ring();
        num = part + 7;
    }
    if (ri < 0 || !num[0] || strspn(num, "0123456789") != strlen(num)) return false;
    int i = atoi(num) - 1;
    const Ring *r = &rings[ri];
    if (i < 0 || i >= r->lv.n) return false;
    float a0, a1;
    radial_segment_angles(i, r->lv.n, &a0, &a1);
    RadialGeo g = role_geo(r->role);
    float a = (a0 + a1) / 2 * 3.14159265f / 180, rad = (g.rin + g.rout) / 2 * s;
    *x = ox + rad * cosf(a);
    *y = oy + rad * sinf(a);
    return true;
}
