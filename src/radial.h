/*
 * Radial (ring) context menu: right click a hex and a ring of segments opens around it, with a round button in the
 * centre. A segment either runs an action (and closes the menu) or opens a submenu: the current ring then shrinks
 * into a smaller inner ring, the submenu's ring grows around it, the segment that opened it stays highlighted and
 * the centre button becomes "back". Only the current level and its parent are shown, at any depth.
 *
 * The menu knows nothing about the game. Its parts:
 *   radial.h / radial.c        the controller: state, animation, geometry, hit-testing, input, events, the config
 *                              check and the entry kinds (what clicking an entry does)
 *   radial_widgets.c           the components: RadialRing (one level; every level uses it), RingSegment,
 *                              CenterButton and the tooltip. They only draw what they are given.
 *   radial_config.c            the data: every menu as a tree of entries
 *   hud_theme.c                the tokens (radial* fields), shared with the HUD
 *   hud_icons.c                the icon registry, shared with the HUD
 *   radial_game.c              the adapter, the only file that touches game state: it decides which menu a hex
 *                              gets, registers the rules the conditions name, and turns actions into game code.
 *                              input.c only tells it about right clicks; it calls radial_open / radial_close.
 *
 * How to ...
 *   Add an action:      add an entry to a menu's array in radial_config.c:
 *                         { .id = "heal", .icon = "placeholder", .label = "Heal", .accent = HUD_ACCENT_GREEN,
 *                           .kind = RADIAL_ACTION }
 *                       and react to its id in radial_game.c's on_event (RADIAL_EV_ACTION brings the id, the menu's
 *                       id and the hex). Order in the array = clockwise order from the top.
 *   Add a submenu:      an entry with .kind = RADIAL_SUBMENU and .children / .nchildren pointing at another array
 *                       (RADIAL_CHILDREN(array) fills both). Submenus nest to RADIAL_MAX_DEPTH levels. A submenu
 *                       with no children (NULL, 0) is a placeholder: it shows, disabled, "Nothing to choose here".
 *   Add a condition:    .visible_if / .enabled_if name a rule ("occupied"; "!occupied" negates it). A hidden entry
 *                       is left out of its ring; a disabled one stays, greyed out, and its tooltip says why. Rules
 *                       live in radial_game.c's RULES table: a key, a function of the hex, and the reasons shown
 *                       when the rule fails ("unmet") and when its negation fails ("met"); or a check function that
 *                       returns the reason itself (NULL = holds). Conditions are checked when a ring opens
 *                       (visibility) and every drawn frame (enabled).
 *   Menus per hex type: add a RadialMenu to RADIAL_MENUS with its own id, and return that id from radial_game.c's
 *                       menu_for(); a menu's .center is the centre button's action on the top level (NULL: the
 *                       centre closes the menu).
 *   Add an entry kind:  add it to RadialKind (before RADIAL_KIND_COUNT) and give it a RadialKindDef in radial.c's
 *                       RADIAL_KINDS table; activate() may use radial_emit_action, radial_enter, radial_close.
 *                       The components ask the table how to draw it (opensRing), so they need no change.
 *   Swap icons:         change hud_icons.c; entries refer to icons by key. A key without a loaded icon draws
 *                       nothing in its place, so the label in the tooltip still tells it apart.
 *   Change the look:    the radial* tokens in hud_theme.c (sizes, gaps, alphas, durations, colours).
 *
 * radial_config_check (run by make test) catches empty or repeated ids, unknown kinds, accents and rules, empty
 * menus, too full rings and too deep trees.
 */
#ifndef TOH_RADIAL_H
#define TOH_RADIAL_H

#include <stdbool.h>
#include <stddef.h>

#include "hud_theme.h"

#define RADIAL_MAX_ENTRIES 12 /* per ring */
#define RADIAL_MAX_DEPTH 6    /* submenu levels below the top */

/* The hex the menu was opened for; every action carries it. */
typedef struct { int row, col; } RadialTarget;

/* ---- data (radial_config.c) ---- */

typedef enum { RADIAL_ACTION, RADIAL_SUBMENU, RADIAL_KIND_COUNT } RadialKind;

typedef struct RadialEntry RadialEntry;
struct RadialEntry {
    const char *id;    /* unique in its menu; sent with RADIAL_EV_ACTION */
    const char *icon;  /* icon key (hud_icons.c) */
    const char *label; /* tooltip; may be NULL */
    HudAccent accent;
    RadialKind kind;
    const RadialEntry *children; /* RADIAL_SUBMENU: its ring */
    int nchildren;
    const char *visible_if, *enabled_if; /* rule keys, "!" negates; NULL = always */
};

#define RADIAL_CHILDREN(array) .children = (array), .nchildren = (int)(sizeof(array) / sizeof((array)[0]))

typedef struct {
    const char *id;            /* chosen by the adapter, e.g. per hex type */
    const RadialEntry *center; /* the centre button's action on the top level; NULL = it closes the menu */
    const RadialEntry *entries;
    int count;
} RadialMenu;

extern const RadialMenu RADIAL_MENUS[];
extern const int RADIAL_NMENUS;

/* ---- rules (registered by the adapter) ---- */

typedef struct {
    const char *key;
    bool (*fn)(const RadialTarget *t);
    const char *unmet; /* why an entry needing the rule is disabled */
    const char *met;   /* why an entry needing "!key" is disabled */
    /* Instead of fn, for a rule whose reason changes: NULL if it holds, else why not (replaces unmet). arg is given
     * back, so one function can serve several rules. */
    const char *(*check)(const RadialTarget *t, int arg);
    int arg;
} RadialRule;

void radial_set_rules(const RadialRule *rules, int n);

/* 0 if menus[0..n) are valid with these rules, else -1 with the reason in err. */
int radial_config_check(const RadialMenu *menus, int n, const RadialRule *rules, int nrules, char *err, size_t cap);
/* Uses other menus (tests); NULL = RADIAL_MENUS. Closes the menu. */
void radial_use_config(const RadialMenu *menus, int n);

/* ---- events ---- */

typedef enum { RADIAL_EV_ACTION, RADIAL_EV_CLOSED } RadialEventType;
typedef struct {
    RadialEventType type;
    const char *id;   /* RADIAL_EV_ACTION: the entry's id (the config's static string) */
    const char *menu; /* the menu's id */
    RadialTarget target;
} RadialEvent;

/* The handler runs in the middle of input handling; it may call radial_open and radial_close. */
void radial_set_event_handler(void (*fn)(const RadialEvent *ev));

/* ---- the interface to the game ---- */

typedef struct {
    float winW, winH;
    float scale; /* 1 = the token sizes */
} RadialScreen;

void radial_set_screen(const RadialScreen *s);
/* Opens menu `menu_id` for hex t around x, y (moved inside the window). False (and closed) if there is no such menu,
 * its config is invalid or none of its entries is visible. Opening while open starts over. */
bool radial_open(const char *menu_id, const RadialTarget *t, float x, float y); /* no RADIAL_EV_CLOSED for the old */
void radial_close(void); /* animated; sends RADIAL_EV_CLOSED. Nothing happens if it is closed already */
bool radial_is_open(void); /* taking input; false while the close animation runs */
const RadialTarget *radial_target(void);
const char *radial_menu_id(void); /* NULL while closed */
void radial_move_to(float x, float y); /* follow the hex when the view moves */
bool radial_contains(float x, float y); /* over the centre or a ring; false while closed */

/* Left button. While open every press is used: on the menu it presses a part, elsewhere it closes the menu. */
bool radial_mouse_down(float x, float y);
/* Updates hover; true while over the menu or while a press that started on it is held. */
bool radial_mouse_move(float x, float y);
/* Left button. Runs the pressed part if released over it. True if the press had started on the menu. */
bool radial_mouse_up(float x, float y);
void radial_cancel_press(void);
void radial_clear_hover(void);

/* Advances the animation by dt seconds; true while it still moves. */
bool radial_update(float dt);
void radial_draw(void);

/* What a segment's centre point is, for scripts and tests: "center", "ring.<n>" (the current level, 1 = the top
 * segment, clockwise) or "parent.<n>" (the shrunk parent level). False if there is no such part. */
bool radial_part_point(const char *part, float *x, float *y);
int radial_depth(void);                  /* 0 = top level */
const char *radial_submenu_id(void);     /* the entry whose ring is open; NULL at the top level */

/* ---- entry kinds (radial.c) ---- */

typedef struct {
    bool opensRing; /* draws the "opens a ring" marker */
    void (*activate)(const RadialEntry *e);
} RadialKindDef;

extern const RadialKindDef RADIAL_KINDS[RADIAL_KIND_COUNT];

/* For RadialKindDef.activate: */
void radial_emit_action(const RadialEntry *e); /* sends RADIAL_EV_ACTION */
void radial_enter(const RadialEntry *e);       /* opens e's ring (e is on the current or the parent ring) */

/* ---- components (radial_widgets.c) ---- */

typedef struct { float rin, rout, alpha; } RadialGeo; /* unscaled radii */

typedef struct {
    float cx, cy, scale;
    RadialGeo geo;
    int n;
    const RadialEntry *const *items;
    const char *const *why; /* per item: NULL = enabled, else why it is disabled */
    int hover, press;       /* item index, -1 = none */
    int highlight;          /* the segment whose ring is open, -1 = none */
    bool interactive;       /* false while it fades out */
} RadialRingView;

/* Segment i's angles in degrees, 0 = right, clockwise (screen y points down); segment 0 is centred at the top. */
void radial_segment_angles(int i, int n, float *a0, float *a1);
int radial_segment_at(int n, float dx, float dy); /* by angle from the centre */

void radial_draw_ring(const RadialRingView *v);
void radial_draw_segment(const RadialRingView *v, int i);
/* back: 0 = shows `action`'s icon, 1 = the back arrow; in between they cross-fade. */
void radial_draw_center(float cx, float cy, float scale, float alpha, const RadialEntry *action, float back,
                        bool hover, bool press, bool disabled);
void radial_draw_tooltip(float x, float y, float scale, float winW, float winH, const char *label, const char *why);

#endif
