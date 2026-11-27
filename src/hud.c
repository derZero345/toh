/* The HUD core: published values, the config check, the layout (cached), hit-testing, input and events.
 * No game state here; see hud.h for how the parts fit together. */
#include "hud.h"

#include "draw.h"

#include <ctype.h>
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
            bool sized = e->type == HUD_BUTTON || e->type == HUD_TEXTFIELD || e->type == HUD_STEPPER ||
                         e->type == HUD_CHOICE || e->type == HUD_LABEL || e->type == HUD_IMAGE;
            if (sized && !(e->width > 0)) {
                snprintf(err, cap, "row %d element %d: needs a width", i + 1, k + 1);
                return -1;
            }
            if (e->type == HUD_TEXTFIELD && (e->max_len < 0 || e->max_len > HUD_TEXT_MAX - 1)) {
                snprintf(err, cap, "row %d element %d: max_len 0 to %d", i + 1, k + 1, HUD_TEXT_MAX - 1);
                return -1;
            }
            if (strcmp(e->key, "panel") == 0 || strcmp(e->key, "tab") == 0 || strcmp(e->key, "tab.close") == 0 ||
                strcmp(e->key, "plus") == 0) {
                snprintf(err, cap, "key \"%s\" is reserved", e->key);
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
    /* "<hotbar key>.<digits>" names a slot part */
    for (int i = 0; i < n; i++)
        for (int k = 0; k < r[i].count; k++) {
            const char *key = r[i].elements[k].key;
            const char *dot = strrchr(key, '.');
            if (!dot || !dot[1] || strspn(dot + 1, "0123456789") != strlen(dot + 1)) continue;
            for (int pi = 0; pi < n; pi++)
                for (int pk = 0; pk < r[pi].count; pk++) {
                    const HudElement *hb = &r[pi].elements[pk];
                    if (hb->type == HUD_HOTBAR && strlen(hb->key) == (size_t)(dot - key) &&
                        strncmp(hb->key, key, (size_t)(dot - key)) == 0) {
                        snprintf(err, cap, "key \"%s\" is reserved", key);
                        return -1;
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
static bool plus;

void hud_begin_frame(void) {
    nvalues = 0;
    memset(&target, 0, sizeof(target));
    plus = false;
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
    snprintf(target.name, sizeof(target.name), "%s", name ? name : "");
    snprintf(target.extra, sizeof(target.extra), "%s", extra ? extra : "");
    target.canClose = can_close;
}

void hud_set_target_tip(const char *tip) { snprintf(target.tip, sizeof(target.tip), "%s", tip ? tip : ""); }
void hud_set_plus(bool shown) { plus = shown; }

const HudTarget *hud_target(void) { return &target; }

void hud_set_bar(const char *key, const HudBar *bar) {
    HudValue *v = value_for(key, HUD_BAR);
    if (v) v->bar = *bar;
}

void hud_set_pill(const char *key, const char *label, const char *value) {
    HudValue *v = value_for(key, HUD_PILL);
    if (!v) return;
    snprintf(v->pill.label, sizeof(v->pill.label), "%s", label ? label : "");
    snprintf(v->pill.value, sizeof(v->pill.value), "%s", value ? value : "");
}

void hud_set_slider(const char *key, const char *label, int value, int min, int max) {
    HudValue *v = value_for(key, HUD_SLIDER);
    if (!v) return;
    snprintf(v->slider.label, sizeof(v->slider.label), "%s", label ? label : "");
    v->slider.value = value;
    v->slider.min = min;
    v->slider.max = max;
}

void hud_set_slot(const char *key, int index, const HudSlot *slot) {
    if (index < 0 || index >= HUD_MAX_SLOTS) return;
    HudValue *v = value_for(key, HUD_HOTBAR);
    if (v) v->slots[index] = *slot;
}

void hud_set_button(const char *key, const char *label, bool enabled, const char *why) {
    HudValue *v = value_for(key, HUD_BUTTON);
    if (!v) return;
    snprintf(v->button.label, sizeof(v->button.label), "%s", label ? label : "");
    v->button.enabled = enabled;
    snprintf(v->button.why, sizeof(v->button.why), "%s", why ? why : "");
}

void hud_set_textfield(const char *key, const char *text) {
    HudValue *v = value_for(key, HUD_TEXTFIELD);
    if (v) snprintf(v->text.text, sizeof(v->text.text), "%s", text ? text : "");
}

void hud_set_stepper(const char *key, const char *label, int value, int min, int max) {
    HudValue *v = value_for(key, HUD_STEPPER);
    if (!v) return;
    snprintf(v->stepper.label, sizeof(v->stepper.label), "%s", label ? label : "");
    v->stepper.value = value;
    v->stepper.min = min;
    v->stepper.max = max;
}

void hud_set_choice(const char *key, const char *const *options, const char *const *hints, int n, int selected) {
    HudValue *v = value_for(key, HUD_CHOICE);
    if (!v) return;
    if (n > HUD_MAX_CHOICES) n = HUD_MAX_CHOICES;
    v->choice.count = n < 0 ? 0 : n;
    v->choice.selected = selected;
    for (int i = 0; i < v->choice.count; i++) {
        snprintf(v->choice.options[i], sizeof(v->choice.options[i]), "%s", options[i] ? options[i] : "");
        snprintf(v->choice.hints[i], sizeof(v->choice.hints[i]), "%s", hints && hints[i] ? hints[i] : "");
    }
}

void hud_set_label(const char *key, const char *text) {
    HudValue *v = value_for(key, HUD_LABEL);
    if (v) snprintf(v->label.text, sizeof(v->label.text), "%s", text ? text : "");
}

void hud_set_image(const char *key, Texture *tex) {
    HudValue *v = value_for(key, HUD_IMAGE);
    if (v) v->image.tex = tex;
}

/* ---- screen, visibility and layout ---- */

static HudScreen screen = { 800, 600, 600, 0, 1 };
static bool visible = true;
static Color border;
static bool border_set;
static const HudUiState UI_NONE = { .hover = -1, .press = -1, .hoverSub = -1, .pressSub = -1, .focus = -1,
                                    .editText = "" };
static HudUiState ui = { .hover = -1, .press = -1, .hoverSub = -1, .pressSub = -1, .focus = -1, .editText = "" };
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
    bool plus;
    HudRect plusRect;
    HudRect elems[HUD_MAX_ROWS][HUD_MAX_ROW_ELEMENTS];
} Layout;

static Layout layout;
static int layout_count;

/* the focused text field: its flat element index, its text and the caret (a byte offset) */
static struct { int idx; char text[HUD_TEXT_MAX]; int caret; } edit = { -1, "", 0 };

static void ui_reset(void) {
    ui = UI_NONE;
    pressActive = false;
    edit.idx = -1;
}

static bool config_checked, config_ok;

void hud_use_config(const HudRow *r, int n) {
    rows = r ? r : HUD_ROWS;
    nrows = r ? n : -1;
    config_checked = false;
    layout.valid = false;
    ui_reset();
}

/* Checks the config once per change; an invalid one is reported on stderr and the HUD then has no rows. */
static bool cfg(void) {
    if (!config_checked) {
        char err[160];
        config_ok = hud_config_check(rows, row_count(), err, sizeof(err)) == 0;
        if (!config_ok) fprintf(stderr, "HUD config: %s\n", err);
        config_checked = true;
    }
    return config_ok;
}

/* the HUD takes part: shown and its config is valid */
static bool active(void) { return visible && cfg(); }

void hud_set_screen(const HudScreen *s) { screen = *s; }
bool hud_visible(void) { return visible; }

void hud_set_border(Color c) {
    border = c;
    border_set = true;
}

Color hud_border(void) { return border_set ? border : HUD_THEME.border; }

void hud_set_visible(bool on) {
    visible = on;
    ui_reset();
}

static const HudElement *element(int idx) {
    return &rows[idx / HUD_MAX_ROW_ELEMENTS].elements[idx % HUD_MAX_ROW_ELEMENTS];
}
static HudRect element_rect(int idx) {
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
    float side = t->plusSize * s, gap = t->elemGap * s;
    if (plus && x < screen.left + t->margin * s + side + gap) x = screen.left + t->margin * s + side + gap;
    float y = screen.bottom - t->margin * s - h;
    layout.panel = (HudRect){ x, y, w, h };
    layout.plusRect = plus ? (HudRect){ x - gap - side, y + h - side, side, side } : (HudRect){ 0 };
    layout.plus = plus;

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
        float tx = x + (w - tw) / 2, hi = screen.winW - t->margin * s - tw, lo = screen.left + t->margin * s;
        if (tx > hi) tx = hi;
        if (tx < lo) tx = lo;
        layout.tab = (HudRect){ tx, y - t->tabGap * s - th, tw, th };
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
           strcmp(a->extra, b->extra) == 0 && strcmp(a->tip, b->tip) == 0;
}

/* Recomputes only when something the layout depends on changed. */
static void ensure_layout(void) {
    if (layout.valid && layout.rows == rows && layout.nrows == row_count() && layout.plus == plus &&
        memcmp(&layout.screen, &screen, sizeof(screen)) == 0 && same_target(&layout.target, &target))
        return;
    layout_compute();
}

float hud_layout_scale(void) {
    if (!cfg()) return screen.scale > 0 ? screen.scale : 1;
    ensure_layout();
    return layout.scale;
}

int hud_layout_count(void) { return layout_count; }

bool hud_part_rect(const char *part, HudRect *out) {
    if (!active()) return false;
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
    if (strcmp(part, "plus") == 0) {
        if (!plus) return false;
        *out = layout.plusRect;
        return true;
    }
    for (int i = 0; i < row_count(); i++)
        for (int k = 0; k < rows[i].count; k++)
            if (strcmp(rows[i].elements[k].key, part) == 0) {
                *out = layout.elems[i][k];
                return true;
            }
    /* "<stepper key>.minus" / ".plus" */
    const char *sdot = strrchr(part, '.');
    if (sdot && (strcmp(sdot, ".minus") == 0 || strcmp(sdot, ".plus") == 0))
        for (int i = 0; i < row_count(); i++)
            for (int k = 0; k < rows[i].count; k++) {
                const HudElement *e = &rows[i].elements[k];
                if (e->type == HUD_STEPPER && strlen(e->key) == (size_t)(sdot - part) &&
                    strncmp(e->key, part, (size_t)(sdot - part)) == 0) {
                    *out = hud_stepper_button(layout.elems[i][k], sdot[1] == 'p', layout.scale);
                    return true;
                }
            }
    /* "<hotbar key>.<slot>" with slot 1..slots */
    const char *dot = strrchr(part, '.');
    if (!dot || !dot[1]) return false;
    char *end;
    if (dot[1] == '0') return false;
    for (const char *p = dot + 1; *p; p++)
        if (!isdigit((unsigned char)*p)) return false;
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
    if (!active()) return false;
    ensure_layout();
    return hud_rect_contains(layout.panel, x, y) || (target.shown && hud_rect_contains(layout.tab, x, y)) ||
           (plus && hud_rect_contains(layout.plusRect, x, y));
}

/* ---- input and events ---- */

static void (*handler)(const HudEvent *ev);

void hud_set_event_handler(void (*fn)(const HudEvent *ev)) { handler = fn; }

static void send(const HudEvent *ev) {
    if (handler) handler(ev);
}

static int element_at(float x, float y) {
    for (int i = 0; i < row_count(); i++)
        for (int k = 0; k < rows[i].count; k++)
            if (hud_rect_contains(layout.elems[i][k], x, y)) return i * HUD_MAX_ROW_ELEMENTS + k;
    return -1;
}

/* the value an element shows: published under its key with its type, else NULL */
static HudValue *element_value(const HudElement *e) {
    HudValue *v = find_value(e->key);
    return v && v->type == e->type ? v : NULL;
}

static void element_mouse(int idx, HudMouseKind kind, float x, float y) {
    const HudElement *e = element(idx);
    const HudComponent *c = component(e);
    if (!c->mouse) return;
    HudEvent ev = { 0 };
    if (c->mouse(e, element_rect(idx), element_value(e), (HudMouse){ kind, x, y }, &ui, idx, layout.scale, &ev)) {
        ev.key = e->key;
        send(&ev);
    }
}

/* ---- text fields: focus and editing ---- */

static void sync_edit(void) {
    ui.focus = edit.idx;
    ui.editText = edit.text;
    ui.caret = edit.caret;
}

static void focus_index(int idx) {
    HudValue *v = element_value(element(idx));
    edit.idx = idx;
    snprintf(edit.text, sizeof(edit.text), "%s", v ? v->text.text : "");
    edit.caret = (int)strlen(edit.text);
    sync_edit();
}

bool hud_focus(const char *key) {
    if (!active()) return false;
    ensure_layout();
    for (int i = 0; i < row_count(); i++)
        for (int k = 0; k < rows[i].count; k++)
            if (rows[i].elements[k].type == HUD_TEXTFIELD && strcmp(rows[i].elements[k].key, key) == 0) {
                focus_index(i * HUD_MAX_ROW_ELEMENTS + k);
                return true;
            }
    return false;
}

void hud_blur(void) {
    edit.idx = -1;
    sync_edit();
}

bool hud_focused(void) { return edit.idx >= 0 && active(); }

static int field_max(const HudElement *e) { return e->max_len > 0 ? e->max_len : HUD_TEXT_MAX - 1; }

static void text_changed(void) {
    sync_edit();
    send(&(HudEvent){ .type = HUD_EV_TEXT, .key = element(edit.idx)->key, .text = edit.text });
}

bool hud_text_input(const char *s) {
    if (!s || !hud_focused()) return false;
    size_t n = strlen(s), len = strlen(edit.text);
    if (n == 0 || len + n > (size_t)field_max(element(edit.idx))) return true; /* too long: ignored */
    memmove(edit.text + edit.caret + n, edit.text + edit.caret, len - (size_t)edit.caret + 1);
    memcpy(edit.text + edit.caret, s, n);
    edit.caret += (int)n;
    text_changed();
    return true;
}

/* UTF-8: the start of the character before / after byte i */
static int char_before(const char *t, int i) {
    if (i <= 0) return 0;
    do i--; while (i > 0 && ((unsigned char)t[i] & 0xC0) == 0x80);
    return i;
}
static int char_after(const char *t, int i) {
    int n = (int)strlen(t);
    if (i >= n) return n;
    do i++; while (i < n && ((unsigned char)t[i] & 0xC0) == 0x80);
    return i;
}

bool hud_text_key(HudEditKey k) {
    if (!hud_focused()) return false;
    int len = (int)strlen(edit.text);
    switch (k) {
    case HUD_EDIT_LEFT: edit.caret = char_before(edit.text, edit.caret); break;
    case HUD_EDIT_RIGHT: edit.caret = char_after(edit.text, edit.caret); break;
    case HUD_EDIT_HOME: edit.caret = 0; break;
    case HUD_EDIT_END: edit.caret = len; break;
    case HUD_EDIT_BACKSPACE:
        if (edit.caret > 0) {
            int p = char_before(edit.text, edit.caret);
            memmove(edit.text + p, edit.text + edit.caret, (size_t)(len - edit.caret + 1));
            edit.caret = p;
            text_changed();
            return true;
        }
        break;
    case HUD_EDIT_DELETE:
        if (edit.caret < len) {
            int nx = char_after(edit.text, edit.caret);
            memmove(edit.text + edit.caret, edit.text + nx, (size_t)(len - nx + 1));
            text_changed();
            return true;
        }
        break;
    case HUD_EDIT_ENTER:
        send(&(HudEvent){ .type = HUD_EV_SUBMIT, .key = element(edit.idx)->key });
        return true;
    case HUD_EDIT_ESCAPE:
        hud_blur();
        return true;
    }
    sync_edit();
    return true;
}

/* ---- pointer ---- */

/* hover as if the pointer moved to x, y with no press held */
static void refresh_hover(float x, float y) {
    ui.hoverClose = target.shown && target.canClose && hud_rect_contains(layout.close, x, y);
    ui.hoverTab = target.shown && hud_rect_contains(layout.tab, x, y);
    ui.hoverPlus = plus && hud_rect_contains(layout.plusRect, x, y);
    int idx = element_at(x, y);
    if (idx != ui.hover) {
        ui.hover = idx;
        ui.hoverSub = -1;
    }
    if (idx >= 0) element_mouse(idx, HUD_MOUSE_MOVE, x, y);
}

void hud_clear_hover(void) {
    ui.hover = ui.hoverSub = -1;
    ui.hoverClose = ui.hoverTab = ui.hoverPlus = false;
}

void hud_cancel_press(void) {
    ui.press = ui.pressSub = -1;
    ui.pressClose = ui.pressPlus = false;
    pressActive = false;
}

bool hud_mouse_down(float x, float y) {
    hud_cancel_press(); /* a press whose release was lost must not stay captured */
    if (!active()) return false;
    ensure_layout();
    refresh_hover(x, y); /* the pressed element is the hovered one, so its hoverSub follows the held press */
    if (plus && hud_rect_contains(layout.plusRect, x, y)) {
        hud_blur();
        ui.pressPlus = true;
        pressActive = true;
        return true;
    }
    if (target.shown && hud_rect_contains(layout.tab, x, y)) {
        ui.pressClose = target.canClose && hud_rect_contains(layout.close, x, y);
        pressActive = true;
        hud_blur();
        return true;
    }
    if (!hud_rect_contains(layout.panel, x, y)) {
        hud_blur();
        return false;
    }
    pressActive = true;
    ui.press = element_at(x, y);
    if (ui.press >= 0 && element(ui.press)->type == HUD_TEXTFIELD) {
        if (edit.idx != ui.press) focus_index(ui.press);
    } else {
        hud_blur();
    }
    if (ui.press >= 0) element_mouse(ui.press, HUD_MOUSE_DOWN, x, y);
    return true;
}

bool hud_mouse_move(float x, float y) {
    if (!active()) return false;
    ensure_layout();
    if (pressActive) {
        ui.hoverClose = target.shown && target.canClose && hud_rect_contains(layout.close, x, y);
        ui.hoverPlus = plus && hud_rect_contains(layout.plusRect, x, y);
        if (ui.press >= 0) element_mouse(ui.press, HUD_MOUSE_MOVE, x, y);
        return true;
    }
    refresh_hover(x, y);
    return false;
}

bool hud_mouse_up(float x, float y) {
    if (!pressActive) return false;
    bool close = ui.pressClose, plusPressed = ui.pressPlus;
    pressActive = false;
    ui.pressClose = ui.pressPlus = false;
    if (active()) {
        ensure_layout();
        if (close && target.shown && target.canClose && hud_rect_contains(layout.close, x, y))
            send(&(HudEvent){ .type = HUD_EV_TARGET_CLOSED });
        if (plusPressed && plus && hud_rect_contains(layout.plusRect, x, y)) send(&(HudEvent){ .type = HUD_EV_PLUS });
        if (ui.press >= 0) element_mouse(ui.press, HUD_MOUSE_UP, x, y);
        ui.press = ui.pressSub = -1;
        refresh_hover(x, y);
    }
    ui.press = ui.pressSub = -1;
    return true;
}

bool hud_key(int slot) {
    if (!active() || slot < 0 || slot >= HUD_MAX_SLOTS) return false;
    for (int i = 0; i < row_count(); i++)
        for (int k = 0; k < rows[i].count; k++) {
            const HudElement *e = &rows[i].elements[k];
            if (e->type != HUD_HOTBAR) continue;
            HudValue *v = element_value(e);
            if (slot >= e->slots || !v || !hud_slot_usable(&v->slots[slot])) return false;
            send(&(HudEvent){ .type = HUD_EV_SLOT_USED, .key = e->key, .slot = slot });
            return true;
        }
    return false;
}

/* ---- tooltips and drawing ---- */

const char *hud_tooltip(void) {
    if (!active()) return NULL;
    ensure_layout();
    if (ui.hoverTab && target.shown && target.tip[0]) return target.tip;
    if (ui.hover < 0) return NULL;
    const HudElement *e = element(ui.hover);
    const HudComponent *c = component(e);
    return c->tooltip ? c->tooltip(e, element_value(e), &ui, ui.hover) : NULL;
}

void hud_draw(void) {
    if (!active()) return;
    ensure_layout();
    float s = layout.scale;
    hud_draw_panel(layout.panel, s);
    for (int i = 0; i < row_count(); i++)
        for (int k = 0; k < rows[i].count; k++) {
            const HudElement *e = &rows[i].elements[k];
            component(e)->draw(e, layout.elems[i][k], element_value(e), &ui, i * HUD_MAX_ROW_ELEMENTS + k, s);
        }
    hud_draw_tab(layout.tab, layout.close, &target, &ui, s);
    if (plus) {
        HudRect p = layout.plusRect;
        hud_draw_panel(p, s);
        Color c = ui.pressPlus && ui.hoverPlus ? HUD_THEME.pressed : ui.hoverPlus ? HUD_THEME.hover : HUD_THEME.text;
        float fs = HUD_THEME.font * 2 * s, a, d;
        dr_font_metrics(FONT_BOLD, fs, &a, &d);
        dr_text(FONT_BOLD, fs, p.x + (p.w - dr_text_width(FONT_BOLD, fs, "+")) / 2, p.y + (p.h + a - d) / 2, c, "+");
    }
    const char *tip = hud_tooltip();
    if (tip) {
        float fs = HUD_THEME.font * s, pad = HUD_THEME.tipPad * s;
        float w = dr_text_width(FONT_PLAIN, fs, tip) + 2 * pad, h = fs + 2 * pad;
        float gap = HUD_THEME.tipGap * s;
        HudRect over = ui.hover >= 0 ? element_rect(ui.hover) : layout.tab;
        float x = over.x + over.w / 2 - w / 2;
        /* above the panel, and above the tab when it is shown */
        float top = target.shown && layout.tab.y < layout.panel.y ? layout.tab.y : layout.panel.y;
        float y = top - gap - h;
        if (ui.hoverTab) y = layout.tab.y + layout.tab.h + gap; /* below the tab, over the panel */
        if (y < 2) y = over.y + over.h + gap;                   /* no room above: below what it belongs to */
        if (y < 2) y = 2;
        /* keep it right of the toolbox; wider than the window: cut at the right, not on the left */
        if (x + w > screen.winW - 2) x = screen.winW - 2 - w;
        if (x < screen.left + 2) x = screen.left + 2;
        hud_draw_tooltip((HudRect){ x, y, w, h }, s, tip);
    }
}
