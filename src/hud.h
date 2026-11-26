/*
 * Bottom HUD: a panel at the bottom centre with rows of elements, and a tab above it for the character shown.
 *
 * The HUD knows nothing about the game. Each drawn frame the game side (hud_game.c) publishes values under keys;
 * the config (hud_config.c) lists rows of elements, each showing the value with its key; components
 * (hud_widgets.c) draw one element type each; tokens (hud_theme.c) hold every size and colour; icons are looked up
 * by key (hud_icons.c). What the user does comes back as HudEvents to the handler hud_game.c registers.
 *
 * How to ...
 *   Add a stat:            publish it in hud_game.c's publish_placeholders (later: real data), e.g.
 *                          hud_set_pill("pill.left", "STR", "3"), and make sure a row in hud_config.c has an
 *                          element with that key.
 *   Add / move / remove a row element or a row:
 *                          edit HUD_ROWS in hud_config.c; nothing else changes. hud_config_check (run by make test)
 *                          catches unknown types, repeated keys and too many rows, elements or slots.
 *   Add a hotbar slot:     raise .slots of the hotbar element (at most 10: the number keys) and publish the slot
 *                          with hud_set_slot("hotbar", index, &slot). Unpublished slots are empty.
 *   Add an element type:   add it to HudElementType (before HUD_ELEMENT_COUNT), write its HudComponent in
 *                          hud_widgets.c and put it into HUD_COMPONENTS. If it shows a new kind of value, add the
 *                          struct to HudValue and a hud_set_* function in hud.c.
 *   Swap icons:            change the table in hud_icons.c; everything else refers to icons by key.
 *   Real content:          the placeholders (stat pills, the slot states) are published in hud_game.c's
 *                          publish_placeholders(); replace them with game data there, and react to slot events in
 *                          hud_game.c's on_event(). A slider (HUD_SLIDER) is available but not in the config.
 */
#ifndef TOH_HUD_H
#define TOH_HUD_H

#include <stdbool.h>
#include <stddef.h>

#include "hud_theme.h"
#include "vk_renderer.h"

#define HUD_MAX_ROWS 8
#define HUD_MAX_ROW_ELEMENTS 8
#define HUD_MAX_SLOTS 10 /* one per number key */
#define HUD_MAX_VALUES 32
#define HUD_MAX_MARKERS 4
#define HUD_TEXT_MAX 48
#define HUD_KEY_MAX 24

typedef struct { float x, y, w, h; } HudRect;

static inline bool hud_rect_contains(HudRect r, float x, float y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

/* ---- config (hud_config.c) ---- */

typedef enum { HUD_BAR, HUD_PILL, HUD_SLIDER, HUD_HOTBAR, HUD_ELEMENT_COUNT } HudElementType;

typedef struct {
    HudElementType type;
    const char *key;   /* which published value it shows; unique */
    HudAccent accent;
    const char *label; /* placeholder text where the element has one (pill, slider); may be NULL */
    int slots;         /* hotbar: 1..HUD_MAX_SLOTS */
    bool two_sided;    /* bar: -max..max; the left half is the zone below 0 */
    bool thin;         /* bar: the thin variant */
} HudElement;

typedef struct {
    const HudElement *elements;
    int count;
} HudRow;

extern const HudRow HUD_ROWS[];
extern const int HUD_NROWS;

/* 0 if rows[0..n) is a valid config, else -1 with the reason in err. */
int hud_config_check(const HudRow *rows, int n, char *err, size_t cap);
/* Uses another config (tests); NULL = HUD_ROWS. */
void hud_use_config(const HudRow *rows, int n);

/* ---- values ---- */

typedef enum { HUD_MARK_LINE, HUD_MARK_TICK } HudMarkerStyle;
typedef struct {
    float at; /* fraction of the frame, like the spans */
    HudMarkerStyle style;
} HudMarker;

/* Spans are fractions of the frame: 0..1 is the frame; a two-sided bar also draws -1..0 left of it. */
typedef struct {
    float solidLo, solidHi; /* the solid fill */
    float lightLo, lightHi; /* the lighter "just lost" part; nothing when lo >= hi */
    HudMarker markers[HUD_MAX_MARKERS];
    int nmarkers;
    char text[HUD_TEXT_MAX];
} HudBar;

typedef struct { char label[HUD_TEXT_MAX], value[HUD_TEXT_MAX]; } HudPill;
typedef struct { char label[HUD_TEXT_MAX]; int value, min, max; } HudSlider;

typedef enum { HUD_SLOT_EMPTY, HUD_SLOT_UNAVAILABLE, HUD_SLOT_AVAILABLE } HudSlotState;
typedef struct {
    HudSlotState state;
    char icon[HUD_KEY_MAX]; /* icon key; "" = none */
    int count;              /* shown when > 0 */
    int cooldown;           /* > 0: shown over the icon, and the slot can't be used */
} HudSlot;

typedef struct {
    char key[HUD_KEY_MAX];
    HudElementType type;
    union {
        HudBar bar;
        HudPill pill;
        HudSlider slider;
        HudSlot slots[HUD_MAX_SLOTS]; /* HUD_HOTBAR */
    };
} HudValue;

void hud_begin_frame(void); /* forgets the previous frame's values and target */
void hud_set_target(const char *name, const char *extra, bool can_close);
void hud_set_bar(const char *key, const HudBar *bar);
void hud_set_pill(const char *key, const char *label, const char *value);
void hud_set_slider(const char *key, const char *label, int value, int min, int max);
void hud_set_slot(const char *key, int index, const HudSlot *slot); /* other slots of that key stay */
/* Published this frame under key, or NULL. The pointer is valid until the next hud_begin_frame. */
const HudValue *hud_value(const char *key);

typedef struct {
    bool shown;
    char name[HUD_TEXT_MAX], extra[HUD_TEXT_MAX]; /* extra: e.g. "+2 (exhausted)" */
    bool canClose;
} HudTarget;
const HudTarget *hud_target(void);

/* ---- screen and layout ---- */

typedef struct {
    float winW, winH;
    float bottom; /* the HUD stays above this y: the window height, or the top of the open console */
    float left;   /* and right of this x: the toolbox */
    float scale;  /* 1 = the token sizes */
} HudScreen;

void hud_set_screen(const HudScreen *s);
bool hud_visible(void);
void hud_set_visible(bool on);
/* The outline of the panel and tab; HUD_THEME.border until set. The game sets its colour theme's border colour. */
void hud_set_border(Color c);
Color hud_border(void);
/* A part of the current layout: an element key, "<hotbar key>.<1..slots>", "panel", "tab" or "tab.close".
 * False if the HUD is hidden, there is no such part, or the tab (or its close button) is not shown. */
bool hud_part_rect(const char *part, HudRect *out);
bool hud_contains(float x, float y); /* over the panel or the tab; false while hidden */
float hud_layout_scale(void);        /* the scale the current layout uses */
int hud_layout_count(void);          /* how often the layout was computed (tests) */
void hud_draw(void);

/* ---- input and events ---- */

typedef enum { HUD_EV_TARGET_CLOSED, HUD_EV_SLOT_USED, HUD_EV_SLIDER_CHANGED } HudEventType;
typedef struct {
    HudEventType type;
    const char *key; /* the config's static key string (compare with strcmp); NULL for HUD_EV_TARGET_CLOSED */
    int slot;        /* HUD_EV_SLOT_USED: 0..9 */
    int value;       /* HUD_EV_SLIDER_CHANGED */
} HudEvent;

/* The handler runs in the middle of input handling: it must not call hud_set_visible, hud_use_config or
 * hud_cancel_press. */
void hud_set_event_handler(void (*fn)(const HudEvent *ev));
/* Left button. True when the HUD used the press: the map must ignore it. */
bool hud_mouse_down(float x, float y);
/* Updates hover; true only while a press that started on the HUD is held (the map must ignore the motion). */
bool hud_mouse_move(float x, float y);
/* Left button. True if the press had started on the HUD. */
bool hud_mouse_up(float x, float y);
/* Forgets a held press, e.g. when the window loses focus. Visibility is untouched. */
void hud_cancel_press(void);
/* Forgets the hover (the pointer left the window); a held press stays. */
void hud_clear_hover(void);
/* Number key for slot 0..9 (keys 1..9, 0). True if the slot was used. */
bool hud_key(int slot);

/* ---- components (hud_widgets.c) ---- */

typedef struct {
    int hover, press;            /* flat element index row * HUD_MAX_ROW_ELEMENTS + element, -1 = none */
    int hoverSub, pressSub;      /* inside it, e.g. the hotbar slot; -1 = none */
    bool hoverClose, pressClose; /* the tab's close button */
} HudUiState;

typedef enum { HUD_MOUSE_DOWN, HUD_MOUSE_MOVE, HUD_MOUSE_UP } HudMouseKind;
typedef struct {
    HudMouseKind kind;
    float x, y;
} HudMouse;

typedef struct {
    bool flex; /* shares the row's free width */
    float (*min_width)(const HudElement *e, float scale);
    float (*height)(const HudElement *e, float scale);
    /* v is NULL when nothing (or a value of another type) is published under the key: draw the empty state.
     * self is the element's flat index, to compare with ui->hover / ui->press. */
    void (*draw)(const HudElement *e, HudRect r, const HudValue *v, const HudUiState *ui, int self, float scale);
    /* Optional (NULL = no mouse handling). DOWN and UP come to the element the press started on; MOVE comes to it
     * while the press is held, otherwise to the element under the pointer. Returns true after filling *ev (type,
     * slot, value; hud.c adds the key) to send an event. v may be NULL. */
    bool (*mouse)(const HudElement *e, HudRect r, HudValue *v, HudMouse m, HudUiState *ui, int self, float scale,
                  HudEvent *ev);
} HudComponent;

extern const HudComponent *const HUD_COMPONENTS[HUD_ELEMENT_COUNT];

/* fixed parts */
void hud_draw_panel(HudRect r, float scale);
float hud_tab_width(const HudTarget *t, float scale);
void hud_draw_tab(HudRect r, HudRect close, const HudTarget *t, const HudUiState *ui, float scale);
/* hotbar geometry, shared by drawing, input and hud_part_rect */
HudRect hud_slot_rect(HudRect hotbar, int i, float scale);
int hud_slot_at(const HudElement *e, HudRect hotbar, float x, float y, float scale); /* -1 = none */
bool hud_slot_usable(const HudSlot *s);

/* ---- icons (hud_icons.c) ---- */

void hud_icons_init(const char *resource_dir);
void hud_icons_free(void);
Texture *hud_icon(const char *key); /* NULL if the key is unknown or the icon isn't loaded */

#endif
