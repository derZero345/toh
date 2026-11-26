/* The radial menu adapter: which menu a hex gets, the rules the conditions name, and what the actions do. This is
 * the only radial menu file that touches game state (see radial.h). */
#include "radial_game.h"
#include "session.h"

/* ---- rules: a key the config names, a test of the hex, and why an entry is disabled ---- */

static bool on_tile(const RadialTarget *t, bool characters_only) {
    Hex h = { t->row, t->col };
    for (int i = 0; i < gh->entities.count; i++) {
        const Entity *e = gh->entities.items[i];
        if (entity_occupies(e, h) && (!characters_only || e->type == ENTITY_CHARACTER)) return true;
    }
    return false;
}

static bool rule_gm(const RadialTarget *t) { return !session_is_client(); } /* the GM, or playing alone */
static bool rule_occupied(const RadialTarget *t) { return on_tile(t, false); }
static bool rule_character(const RadialTarget *t) { return on_tile(t, true); }

static const RadialRule RULES[] = {
    { "gm", rule_gm, "Only the GM can do this", "The GM can't do this" },
    { "occupied", rule_occupied, "Needs something on the tile", "Needs an empty tile" },
    { "character", rule_character, "Needs a character on the tile", "Not with a character on the tile" },
};
#define NRULES ((int)(sizeof(RULES) / sizeof(RULES[0])))

const RadialRule *radial_game_rules(int *n) {
    *n = NRULES;
    return RULES;
}

/* ---- which menu ---- */

const char *radial_game_menu_for(Hex h) {
    RadialTarget t = { h.row, h.col };
    return rule_character(&t) ? "character" : "tile";
}

/* ---- events ---- */

static void on_event(const RadialEvent *ev) {
    switch (ev->type) {
    case RADIAL_EV_ACTION: /* placeholder entries: they only say what was chosen */
        console_log("Menu: %s on [%d|%d]", ev->id, ev->target.row, ev->target.col);
        break;
    case RADIAL_EV_CLOSED:
        break;
    }
}

void radial_game_init(void) {
    radial_set_rules(RULES, NRULES);
    radial_set_event_handler(on_event);
}

void radial_game_shutdown(void) {
    radial_set_event_handler(NULL);
    radial_set_rules(NULL, 0);
}

/* ---- screen and input ---- */

void radial_game_refresh_screen(void) {
    float scale = gh->console.fontSize > 0 ? (float)gh->console.fontSize / 16.0f : 1; /* like the HUD */
    RadialScreen s = { (float)gh->winW, (float)gh->winH, scale };
    radial_set_screen(&s);
}

static Hex target_hex(void) {
    const RadialTarget *t = radial_target();
    return (Hex){ t->row, t->col };
}

bool radial_game_open(Vec2 p) {
    Hex h;
    if (!hex_find_closest(p, &h)) return false;
    radial_game_refresh_screen();
    RadialTarget t = { h.row, h.col };
    Vec2 c = hex_center(h);
    return radial_open(radial_game_menu_for(h), &t, (float)c.x, (float)c.y);
}

bool radial_game_right_down(Vec2 p) {
    if (!radial_is_open()) return false;
    radial_game_refresh_screen();
    Hex h;
    bool used = radial_contains((float)p.x, (float)p.y) || (hex_find_closest(p, &h) && hex_eq(h, target_hex()));
    radial_close();
    return used;
}

void radial_game_close(void) { radial_close(); }

bool radial_game_mouse_down(Vec2 p) {
    radial_game_refresh_screen();
    return radial_mouse_down((float)p.x, (float)p.y);
}

bool radial_game_mouse_move(Vec2 p) {
    radial_game_refresh_screen();
    return radial_mouse_move((float)p.x, (float)p.y);
}

bool radial_game_mouse_up(Vec2 p) {
    radial_game_refresh_screen();
    return radial_mouse_up((float)p.x, (float)p.y);
}

bool radial_game_contains(Vec2 p) {
    radial_game_refresh_screen();
    return radial_contains((float)p.x, (float)p.y);
}

bool radial_game_update(float dt) {
    radial_game_refresh_screen();
    if (radial_menu_id()) { /* open or closing: stay on the hex while the view scrolls or zooms */
        Vec2 c = hex_center(target_hex());
        radial_move_to((float)c.x, (float)c.y);
    }
    return radial_update(dt);
}
