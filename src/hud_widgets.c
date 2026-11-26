/* HUD components: one per element type (HUD_COMPONENTS), plus the panel, the target tab and the hotbar geometry.
 * They only draw what they are given; every size and colour comes from HUD_THEME. */
#include "draw.h"
#include "hud.h"

#include <math.h>
#include <stdio.h>

#define T (&HUD_THEME)

/* ---- helpers ---- */

static void round_rect(HudRect r, float radius, Color c) {
    if (r.w <= 0 || r.h <= 0) return;
    dr_fill_round_rect(r.x, r.y, r.w, r.h, radius * 2, radius * 2, c);
}

/* Outline of a rounded rectangle as ONE closed polyline, stroked once: separate lines and arcs would overlap at the
 * joints and show up brighter there when the colour is translucent. Corners in order TR, TL, BL, BR. */
static void stroke_round_rect(HudRect r, float rad, float th, Color c) {
    enum { SEG = 8 };
    Vec2 p[4 * (SEG + 1)];
    int n = 0;
    rad = fminf(rad, fminf(r.w, r.h) / 2);
    float cx[4] = { r.x + r.w - rad, r.x + rad, r.x + rad, r.x + r.w - rad };
    float cy[4] = { r.y + rad, r.y + rad, r.y + r.h - rad, r.y + r.h - rad };
    for (int k = 0; k < 4; k++)
        for (int i = 0; i <= SEG; i++) {
            double a = (k * 90.0 + 90.0 * i / SEG) * 3.14159265358979 / 180.0;
            p[n++] = (Vec2){ cx[k] + rad * cos(a), cy[k] - rad * sin(a) };
        }
    dr_polygon(p, n, th, c);
}

static float baseline(FontStyle st, float size, HudRect r) {
    float a, d;
    dr_font_metrics(st, size, &a, &d);
    return r.y + (r.h + a - d) / 2;
}

static void text_centered(FontStyle st, float size, HudRect r, Color c, const char *s) {
    dr_text(st, size, r.x + (r.w - dr_text_width(st, size, s)) / 2, baseline(st, size, r), c, s);
}

static void text_left(FontStyle st, float size, float x, HudRect r, Color c, const char *s) {
    dr_text(st, size, x, baseline(st, size, r), c, s);
}

static void text_right(FontStyle st, float size, float right, HudRect r, Color c, const char *s) {
    dr_text(st, size, right - dr_text_width(st, size, s), baseline(st, size, r), c, s);
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ---- panel and tab ---- */

void hud_draw_panel(HudRect r, float s) {
    float rad = T->radius * s;
    for (int i = T->shadowLayers; i >= 1; i--) {
        float o = T->shadowStep * s * (float)i;
        round_rect((HudRect){ r.x - o, r.y - o / 2, r.w + 2 * o, r.h + 2 * o }, rad + o, T->shadow);
    }
    round_rect(r, rad, T->panel);
    stroke_round_rect(r, rad, fmaxf(1, s), T->border);
}

static void tab_text(const HudTarget *t, char *out, size_t cap) {
    if (t->extra[0]) snprintf(out, cap, "%s %s", t->name, t->extra);
    else snprintf(out, cap, "%s", t->name);
}

/* [pad][icon][pad][text][pad] and, with a close button, [close][pad] */
float hud_tab_width(const HudTarget *t, float s) {
    char text[2 * HUD_TEXT_MAX + 2];
    tab_text(t, text, sizeof(text));
    float w = (3 * T->tabPad + T->tabIcon) * s + dr_text_width(FONT_BOLD, T->font * s, text);
    return w + (t->canClose ? (T->closeSize + T->tabPad) * s : 0);
}

void hud_draw_tab(HudRect r, HudRect close, const HudTarget *t, const HudUiState *ui, float s) {
    if (!t->shown) return;
    hud_draw_panel(r, s);
    float x = r.x + T->tabPad * s, ic = T->tabIcon * s;
    Texture *icon = hud_icon("placeholder"); /* until characters have HUD icons */
    if (icon) dr_image(icon, x, r.y + (r.h - ic) / 2, ic, ic);
    x += ic + T->tabPad * s;
    char text[2 * HUD_TEXT_MAX + 2];
    tab_text(t, text, sizeof(text));
    text_left(FONT_BOLD, T->font * s, x, r, T->text, text);
    if (t->canClose) {
        Color c = ui->pressClose && ui->hoverClose ? T->pressed : ui->hoverClose ? T->hover : T->textDim;
        float p = close.w * T->closeInset, th = fmaxf(1.5f, T->closeLine * s);
        dr_line(close.x + p, close.y + p, close.x + close.w - p, close.y + close.h - p, th, c);
        dr_line(close.x + close.w - p, close.y + p, close.x + p, close.y + close.h - p, th, c);
    }
}

/* ---- bar ---- */

static float bar_min_width(const HudElement *e, float s) { return T->barMinW * s; }
static float bar_height(const HudElement *e, float s) { return (e->thin ? T->thinBarH : T->barH) * s; }

static void bar_span(HudRect frame, float lo, float hi, bool two_sided, Color c) {
    lo = clampf(lo, two_sided ? -1 : 0, 1);
    hi = clampf(hi, two_sided ? -1 : 0, 1);
    if (hi > lo) dr_fill_rect(frame.x + lo * frame.w, frame.y, (hi - lo) * frame.w, frame.h, c);
}

static void bar_draw(const HudElement *e, HudRect r, const HudValue *v, const HudUiState *ui, int self, float s) {
    HudRect frame = r;
    if (e->two_sided) { /* left half: the zone below 0, right half: the frame (0..max) */
        frame.w = r.w / 2;
        frame.x = r.x + frame.w;
        dr_fill_rect(r.x, r.y, frame.w, r.h, T->dyingBg);
    }
    dr_fill_rect(frame.x, frame.y, frame.w, frame.h, T->barBg);
    float line = fmaxf(1, T->barFrameLine * s);
    if (!v) {
        dr_stroke_rect(frame.x, frame.y, frame.w, frame.h, line, T->emptyLine);
        return;
    }
    const HudBar *b = &v->bar;
    bar_span(frame, b->lightLo, b->lightHi, e->two_sided, T->accentLight[e->accent]);
    bar_span(frame, b->solidLo, b->solidHi, e->two_sided, T->accent[e->accent]);
    for (int i = 0; i < b->nmarkers && i < HUD_MAX_MARKERS; i++) {
        const HudMarker *m = &b->markers[i];
        if (m->at < (e->two_sided ? -1 : 0) || m->at > 1) continue;
        float mx = frame.x + m->at * frame.w;
        float over = m->style == HUD_MARK_TICK ? T->tickOverhang * s : 0;
        dr_line(mx, frame.y - over, mx, frame.y + frame.h + over, T->markerLine * s, T->marker);
    }
    dr_stroke_rect(frame.x, frame.y, frame.w, frame.h, line, T->barFrame);
    if (b->text[0]) text_centered(FONT_PLAIN, T->font * s, frame, T->text, b->text); /* thin bars too: readable */
}

/* ---- stat pill ---- */

static float pill_min_width(const HudElement *e, float s) { return T->pillW * s; }
static float pill_height(const HudElement *e, float s) { return T->pillH * s; }

static void pill_draw(const HudElement *e, HudRect r, const HudValue *v, const HudUiState *ui, int self, float s) {
    float rad = r.h / 2, edge = T->pillEdge * s;
    round_rect(r, rad, T->slotBg);
    /* the accent: a short upright bar inside the left rounding */
    round_rect((HudRect){ r.x + rad * 0.6f, r.y + r.h * 0.25f, edge, r.h * 0.5f }, edge / 2, T->accent[e->accent]);
    const char *label = v && v->pill.label[0] ? v->pill.label : e->label ? e->label : "";
    const char *value = v && v->pill.value[0] ? v->pill.value : "—";
    Color vc = v ? T->text : T->textDim;
    float pad = T->pillPad * s, x0 = r.x + rad * 0.6f + edge + pad, x1 = r.x + r.w - pad;
    if (label[0]) {
        text_left(FONT_PLAIN, T->smallFont * s, x0, r, T->textDim, label);
        text_right(FONT_BOLD, T->font * s, x1, r, vc, value);
    } else {
        text_centered(FONT_BOLD, T->font * s, (HudRect){ x0, r.y, x1 - x0, r.h }, vc, value);
    }
}

/* ---- slider ---- */

static float slider_min_width(const HudElement *e, float s) { return (T->sliderLabelW + T->sliderMinW) * s; }
static float slider_height(const HudElement *e, float s) { return fmaxf(T->sliderKnob, T->font + T->sliderTextPad) * s; }

/* the track starts after the label; half a knob of room at both ends */
static HudRect slider_track(HudRect r, float s) {
    float k = T->sliderKnob * s, th = T->sliderTrack * s;
    return (HudRect){ r.x + T->sliderLabelW * s + k / 2, r.y + (r.h - th) / 2, r.w - T->sliderLabelW * s - k, th };
}

static float slider_fraction(const HudSlider *sl) {
    return sl->max > sl->min ? clampf((float)(sl->value - sl->min) / (float)(sl->max - sl->min), 0, 1) : 0;
}

static void slider_draw(const HudElement *e, HudRect r, const HudValue *v, const HudUiState *ui, int self, float s) {
    const char *label = v && v->slider.label[0] ? v->slider.label : e->label ? e->label : "";
    char text[HUD_TEXT_MAX + 16];
    if (v) snprintf(text, sizeof(text), "%s %d", label, v->slider.value);
    else snprintf(text, sizeof(text), "%s", label);
    text_left(FONT_PLAIN, T->font * s, r.x, r, v ? T->text : T->textDim, text);
    HudRect tr = slider_track(r, s);
    round_rect(tr, tr.h / 2, T->barBg);
    if (!v) return;
    float f = slider_fraction(&v->slider), k = T->sliderKnob * s;
    round_rect((HudRect){ tr.x, tr.y, tr.w * f, tr.h }, tr.h / 2, T->accent[e->accent]);
    bool hot = ui->hover == self || ui->press == self;
    dr_fill_oval(tr.x + tr.w * f - k / 2, r.y + (r.h - k) / 2, k, k, hot ? T->hover : T->text);
}

static bool slider_mouse(const HudElement *e, HudRect r, HudValue *v, HudMouse m, HudUiState *ui, int self,
                         float s, HudEvent *ev) {
    if (!v || m.kind == HUD_MOUSE_UP || (m.kind == HUD_MOUSE_MOVE && ui->press != self)) return false;
    HudRect tr = slider_track(r, s);
    if (m.kind == HUD_MOUSE_DOWN && m.x < tr.x - T->sliderKnob * s / 2) { /* on the label: no drag follows */
        ui->press = -1;
        return false;
    }
    float f = tr.w > 0 ? clampf((m.x - tr.x) / tr.w, 0, 1) : 0;
    int val = v->slider.min + (int)lroundf(f * (float)(v->slider.max - v->slider.min));
    if (val == v->slider.value) return false;
    v->slider.value = val; /* drawn right away; the game publishes its own copy from the next frame on */
    ev->type = HUD_EV_SLIDER_CHANGED;
    ev->value = val;
    return true;
}

/* ---- hotbar ---- */

static float hotbar_min_width(const HudElement *e, float s) {
    return ((float)e->slots * T->slot + (float)(e->slots - 1) * T->slotGap) * s;
}
static float hotbar_height(const HudElement *e, float s) { return T->slot * s; }

HudRect hud_slot_rect(HudRect hotbar, int i, float s) {
    return (HudRect){ hotbar.x + (float)i * (T->slot + T->slotGap) * s, hotbar.y, T->slot * s, T->slot * s };
}

int hud_slot_at(const HudElement *e, HudRect hotbar, float x, float y, float s) {
    for (int i = 0; i < e->slots; i++)
        if (hud_rect_contains(hud_slot_rect(hotbar, i, s), x, y)) return i;
    return -1;
}

bool hud_slot_usable(const HudSlot *sl) { return sl->state == HUD_SLOT_AVAILABLE && sl->cooldown <= 0; }

static void hotbar_draw(const HudElement *e, HudRect r, const HudValue *v, const HudUiState *ui, int self, float s) {
    static const char *const KEYS[HUD_MAX_SLOTS] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0" };
    float rad = T->slotRadius * s, line = fmaxf(1, s), pad = T->slotIconPad * s, tp = T->slotTextPad * s;
    float fs = T->smallFont * s, a, d;
    dr_font_metrics(FONT_PLAIN, fs, &a, &d);
    for (int i = 0; i < e->slots && i < HUD_MAX_SLOTS; i++) {
        const HudSlot *sl = v ? &v->slots[i] : NULL;
        HudSlotState st = sl ? sl->state : HUD_SLOT_EMPTY;
        bool used = st != HUD_SLOT_EMPTY;
        bool hover = used && ui->hover == self && ui->hoverSub == i;
        bool pressed = used && ui->press == self && ui->pressSub == i && ui->hoverSub == i; /* pointer still on it */
        HudRect sr = hud_slot_rect(r, i, s);
        if (hover && !pressed) sr.y -= s; /* raised */
        round_rect(sr, rad, T->slotBg);
        Texture *icon = used && sl->icon[0] ? hud_icon(sl->icon) : NULL;
        if (icon) {
            if (st == HUD_SLOT_UNAVAILABLE) dr_set_alpha(T->unavailableAlpha);
            dr_image(icon, sr.x + pad, sr.y + pad, sr.w - 2 * pad, sr.h - 2 * pad);
            dr_set_alpha(1); /* the HUD is drawn at full alpha, so 1 is the right value to go back to */
        }
        if (st == HUD_SLOT_UNAVAILABLE) round_rect(sr, rad, T->veil);
        if (used && sl->cooldown > 0) {
            char cd[16];
            snprintf(cd, sizeof(cd), "%d", sl->cooldown);
            round_rect(sr, rad, T->veil);
            text_centered(FONT_BOLD, T->font * T->cooldownFontScale * s, sr, T->text, cd);
        }
        Color border = pressed                   ? T->pressed
                       : hover                   ? T->hover
                       : st == HUD_SLOT_AVAILABLE ? T->accent[e->accent]
                                                 : T->emptyLine;
        stroke_round_rect(sr, rad, line, border);
        dr_text(FONT_PLAIN, fs, sr.x + tp, sr.y + tp + a, used ? T->text : T->textDim, KEYS[i]);
        if (used && sl->count > 0) {
            char cnt[16];
            snprintf(cnt, sizeof(cnt), "%d", sl->count);
            dr_text(FONT_BOLD, fs, sr.x + sr.w - tp - dr_text_width(FONT_BOLD, fs, cnt), sr.y + sr.h - tp - d,
                    T->text, cnt);
        }
    }
}

static bool hotbar_mouse(const HudElement *e, HudRect r, HudValue *v, HudMouse m, HudUiState *ui, int self,
                         float s, HudEvent *ev) {
    int at = hud_slot_at(e, r, m.x, m.y, s);
    switch (m.kind) {
    case HUD_MOUSE_DOWN:
        ui->pressSub = at;
        return false;
    case HUD_MOUSE_MOVE:
        ui->hoverSub = at;
        return false;
    case HUD_MOUSE_UP:
        if (at < 0 || at != ui->pressSub || !v || !hud_slot_usable(&v->slots[at])) return false;
        ev->type = HUD_EV_SLOT_USED;
        ev->slot = at;
        return true;
    }
    return false;
}

/* ---- the component map: a new element type adds one entry ---- */

static const HudComponent BAR = { true, bar_min_width, bar_height, bar_draw, NULL };
static const HudComponent PILL = { false, pill_min_width, pill_height, pill_draw, NULL };
static const HudComponent SLIDER = { true, slider_min_width, slider_height, slider_draw, slider_mouse };
static const HudComponent HOTBAR = { false, hotbar_min_width, hotbar_height, hotbar_draw, hotbar_mouse };

const HudComponent *const HUD_COMPONENTS[HUD_ELEMENT_COUNT] = {
    [HUD_BAR] = &BAR,
    [HUD_PILL] = &PILL,
    [HUD_SLIDER] = &SLIDER,
    [HUD_HOTBAR] = &HOTBAR,
};
