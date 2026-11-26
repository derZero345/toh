/* The HUD core: published values, the config check, the layout (cached), hit-testing, input and events.
 * No game state here; see hud.h for how the parts fit together. */
#include "hud.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const HudRow *rows = HUD_ROWS;
static int nrows = -1; /* -1: HUD_NROWS (not a constant expression, so it can't initialise this) */

static int row_count(void) { return nrows < 0 ? HUD_NROWS : nrows; }

/* ---- config check ---- */

int hud_config_check(const HudRow *r, int n, char *err, size_t cap) {
    if (n < 1 || n > HUD_MAX_ROWS) {
        snprintf(err, cap, "%d rows (1 to %d)", n, HUD_MAX_ROWS);
        return -1;
    }
    int hotbars = 0;
    for (int i = 0; i < n; i++) {
        if (r[i].count < 1 || r[i].count > HUD_MAX_ROW_ELEMENTS) {
            snprintf(err, cap, "row %d has %d elements (1 to %d)", i + 1, r[i].count, HUD_MAX_ROW_ELEMENTS);
            return -1;
        }
        for (int k = 0; k < r[i].count; k++) {
            const HudElement *e = &r[i].elements[k];
            if ((int)e->type < 0 || e->type >= HUD_ELEMENT_COUNT || !HUD_COMPONENTS[e->type]) {
                snprintf(err, cap, "row %d element %d: unknown type", i + 1, k + 1);
                return -1;
            }
            if ((int)e->accent < 0 || e->accent >= HUD_ACCENT_COUNT) {
                snprintf(err, cap, "row %d element %d: unknown accent", i + 1, k + 1);
                return -1;
            }
            if (!e->key || !e->key[0] || strlen(e->key) >= HUD_KEY_MAX) {
                snprintf(err, cap, "row %d element %d: key missing or longer than %d", i + 1, k + 1,
                         HUD_KEY_MAX - 1);
                return -1;
            }
            for (int pi = 0; pi <= i; pi++)
                for (int pk = 0; pk < (pi < i ? r[pi].count : k); pk++)
                    if (strcmp(r[pi].elements[pk].key, e->key) == 0) {
                        snprintf(err, cap, "key \"%s\" is used twice", e->key);
                        return -1;
                    }
            if (e->type == HUD_HOTBAR) {
                hotbars++;
                if (e->slots < 1 || e->slots > HUD_MAX_SLOTS) {
                    snprintf(err, cap, "hotbar \"%s\" has %d slots (1 to %d)", e->key, e->slots, HUD_MAX_SLOTS);
                    return -1;
                }
            }
        }
    }
    if (hotbars > 1) {
        snprintf(err, cap, "%d hotbars (at most one: it owns the number keys)", hotbars);
        return -1;
    }
    return 0;
}

/* ---- values ---- */

static HudValue values[HUD_MAX_VALUES];
static int nvalues;
static HudTarget target;

void hud_begin_frame(void) {
    nvalues = 0;
    memset(&target, 0, sizeof(target));
}

static HudValue *find_value(const char *key) {
    for (int i = 0; i < nvalues; i++)
        if (strcmp(values[i].key, key) == 0) return &values[i];
    return NULL;
}

const HudValue *hud_value(const char *key) { return find_value(key); }

/* The value under key with this type: the existing one, a cleared one if the type differs, or a new one. */
static HudValue *value_for(const char *key, HudElementType type) {
    HudValue *v = find_value(key);
    if (v && v->type == type) return v;
    if (!v) {
        if (nvalues == HUD_MAX_VALUES) return NULL;
        v = &values[nvalues++];
    }
    memset(v, 0, sizeof(*v));
    snprintf(v->key, sizeof(v->key), "%s", key);
    v->type = type;
    return v;
}

void hud_set_target(const char *name, const char *extra, bool can_close) {
    target.shown = true;
    snprintf(target.name, sizeof(target.name), "%s", name);
    snprintf(target.extra, sizeof(target.extra), "%s", extra ? extra : "");
    target.canClose = can_close;
}

const HudTarget *hud_target(void) { return &target; }

void hud_set_bar(const char *key, const HudBar *bar) {
    HudValue *v = value_for(key, HUD_BAR);
    if (v) v->bar = *bar;
}

void hud_set_pill(const char *key, const char *label, const char *value) {
    HudValue *v = value_for(key, HUD_PILL);
    if (!v) return;
    snprintf(v->pill.label, sizeof(v->pill.label), "%s", label);
    snprintf(v->pill.value, sizeof(v->pill.value), "%s", value);
}

void hud_set_slider(const char *key, const char *label, int value, int min, int max) {
    HudValue *v = value_for(key, HUD_SLIDER);
    if (!v) return;
    snprintf(v->slider.label, sizeof(v->slider.label), "%s", label);
    v->slider.value = value;
    v->slider.min = min;
    v->slider.max = max;
}

void hud_set_slot(const char *key, int index, const HudSlot *slot) {
    if (index < 0 || index >= HUD_MAX_SLOTS) return;
    HudValue *v = value_for(key, HUD_HOTBAR);
    if (v) v->slots[index] = *slot;
}

/* ---- screen, visibility and layout ---- */

static HudScreen screen = { 800, 600, 600, 0, 1 };
static bool visible = true;
static HudUiState ui = { -1, -1, -1, -1, false, false };
static bool pressActive; /* a left press started on the HUD and is still held */

typedef struct {
    bool valid;
    /* what it was computed from */
    const HudRow *rows;
    int nrows;
    HudScreen screen;
    HudTarget target;
    /* the result */
    float scale;
    HudRect panel, tab, close;
    HudRect elems[HUD_MAX_ROWS][HUD_MAX_ROW_ELEMENTS];
} Layout;

static Layout layout;
static int layout_count;

static void ui_reset(void) {
    ui = (HudUiState){ -1, -1, -1, -1, false, false };
    pressActive = false;
}

void hud_use_config(const HudRow *r, int n) {
    rows = r ? r : HUD_ROWS;
    nrows = r ? n : -1;
    layout.valid = false;
    ui_reset();
}

void hud_set_screen(const HudScreen *s) { screen = *s; }
bool hud_visible(void) { return visible; }

void hud_set_visible(bool on) {
    visible = on;
    ui_reset();
}

__attribute__((unused)) static const HudElement *element(int idx) {
    return &rows[idx / HUD_MAX_ROW_ELEMENTS].elements[idx % HUD_MAX_ROW_ELEMENTS];
}
__attribute__((unused)) static HudRect element_rect(int idx) {
    return layout.elems[idx / HUD_MAX_ROW_ELEMENTS][idx % HUD_MAX_ROW_ELEMENTS];
}
static const HudComponent *component(const HudElement *e) { return HUD_COMPONENTS[e->type]; }

static float row_min_width(const HudRow *row, float s) {
    float w = 0;
    for (int k = 0; k < row->count; k++) w += component(&row->elements[k])->min_width(&row->elements[k], s);
    return w + (float)(row->count - 1) * HUD_THEME.elemGap * s;
}

static float panel_width(float s) {
    float w = 0;
    for (int i = 0; i < row_count(); i++) {
        float rw = row_min_width(&rows[i], s);
        if (rw > w) w = rw;
    }
    return w + 2 * HUD_THEME.padding * s;
}

static void layout_compute(void) {
    const HudTheme *t = &HUD_THEME;
    int n = row_count();
    float s = screen.scale > 0 ? screen.scale : 1;
    float w = panel_width(s), avail = screen.winW - screen.left - 2 * t->margin * s;
    if (w > avail) { /* narrow window: shrink, but not below minScale */
        float s2 = avail > 0 ? s * avail / w : 0;
        s = s2 > s * t->minScale ? s2 : s * t->minScale;
        w = panel_width(s);
    }
    float inner = w - 2 * t->padding * s, h = 2 * t->padding * s, rowh[HUD_MAX_ROWS];
    for (int i = 0; i < n; i++) {
        rowh[i] = 0;
        for (int k = 0; k < rows[i].count; k++) {
            float eh = component(&rows[i].elements[k])->height(&rows[i].elements[k], s);
            if (eh > rowh[i]) rowh[i] = eh;
        }
        h += rowh[i] + (i > 0 ? t->rowGap * s : 0);
    }
    float x = (screen.winW - w) / 2;
    if (x < screen.left + t->margin * s) x = screen.left + t->margin * s;
    float y = screen.bottom - t->margin * s - h;
    layout.panel = (HudRect){ x, y, w, h };

    float ry = y + t->padding * s;
    for (int i = 0; i < n; i++) {
        const HudRow *row = &rows[i];
        float free = inner - row_min_width(row, s);
        int nflex = 0;
        for (int k = 0; k < row->count; k++) nflex += component(&row->elements[k])->flex;
        float ex = x + t->padding * s + (nflex == 0 ? free / 2 : 0); /* rows without flex elements are centred */
        for (int k = 0; k < row->count; k++) {
            const HudElement *e = &row->elements[k];
            const HudComponent *c = component(e);
            float ew = c->min_width(e, s) + (c->flex ? free / (float)nflex : 0), eh = c->height(e, s);
            layout.elems[i][k] = (HudRect){ ex, ry + (rowh[i] - eh) / 2, ew, eh };
            ex += ew + t->elemGap * s;
        }
        ry += rowh[i] + t->rowGap * s;
    }

    layout.tab = layout.close = (HudRect){ 0 };
    if (target.shown) {
        float tw = hud_tab_width(&target, s), th = t->tabH * s, cs = t->closeSize * s;
        layout.tab = (HudRect){ x + (w - tw) / 2, y - t->tabGap * s - th, tw, th };
        if (target.canClose)
            layout.close = (HudRect){ layout.tab.x + tw - t->tabPad * s - cs, layout.tab.y + (th - cs) / 2, cs, cs };
    }
    layout.scale = s;
    layout.rows = rows;
    layout.nrows = n;
    layout.screen = screen;
    layout.target = target;
    layout.valid = true;
    layout_count++;
}

static bool same_target(const HudTarget *a, const HudTarget *b) {
    return a->shown == b->shown && a->canClose == b->canClose && strcmp(a->name, b->name) == 0 &&
           strcmp(a->extra, b->extra) == 0;
}

/* Recomputes only when something the layout depends on changed. */
static void ensure_layout(void) {
    if (layout.valid && layout.rows == rows && layout.nrows == row_count() &&
        memcmp(&layout.screen, &screen, sizeof(screen)) == 0 && same_target(&layout.target, &target))
        return;
    layout_compute();
}

float hud_layout_scale(void) {
    ensure_layout();
    return layout.scale;
}

int hud_layout_count(void) { return layout_count; }

bool hud_part_rect(const char *part, HudRect *out) {
    if (!visible) return false;
    ensure_layout();
    if (strcmp(part, "panel") == 0) {
        *out = layout.panel;
        return true;
    }
    if (strcmp(part, "tab") == 0 || strcmp(part, "tab.close") == 0) {
        if (!target.shown || (part[3] == '.' && !target.canClose)) return false;
        *out = part[3] == '.' ? layout.close : layout.tab;
        return true;
    }
    for (int i = 0; i < row_count(); i++)
        for (int k = 0; k < rows[i].count; k++)
            if (strcmp(rows[i].elements[k].key, part) == 0) {
                *out = layout.elems[i][k];
                return true;
            }
    /* "<hotbar key>.<slot>" with slot 1..slots */
    const char *dot = strrchr(part, '.');
    if (!dot || !dot[1]) return false;
    char *end;
    long slot = strtol(dot + 1, &end, 10);
    if (*end) return false;
    for (int i = 0; i < row_count(); i++)
        for (int k = 0; k < rows[i].count; k++) {
            const HudElement *e = &rows[i].elements[k];
            if (e->type != HUD_HOTBAR || strncmp(e->key, part, (size_t)(dot - part)) != 0 ||
                e->key[dot - part] != '\0')
                continue;
            if (slot < 1 || slot > e->slots) return false;
            *out = hud_slot_rect(layout.elems[i][k], (int)slot - 1, layout.scale);
            return true;
        }
    return false;
}

bool hud_contains(float x, float y) {
    if (!visible) return false;
    ensure_layout();
    return hud_rect_contains(layout.panel, x, y) || (target.shown && hud_rect_contains(layout.tab, x, y));
}
