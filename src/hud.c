/* The HUD core: published values, the config check, the layout (cached), hit-testing, input and events.
 * No game state here; see hud.h for how the parts fit together. */
#include "hud.h"

#include <stdio.h>
#include <string.h>

__attribute__((unused)) static const HudRow *rows = HUD_ROWS;
static int nrows = -1; /* -1: HUD_NROWS (not a constant expression, so it can't initialise this) */

__attribute__((unused)) static int row_count(void) { return nrows < 0 ? HUD_NROWS : nrows; }

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
