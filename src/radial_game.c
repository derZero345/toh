/* The radial menu adapter: which menu a hex gets, the rules the conditions name, and what the actions do. This is
 * the only radial menu file that touches game state (see radial.h). */
#include "radial_game.h"
#include "blocks.h"
#include "dodges.h"
#include "phases.h"
#include "target_ui.h"
#include "charcreate.h"
#include "move_ui.h"
#include "moves.h"
#include "recover.h"
#include "session.h"

#include <string.h>
#include <strings.h>

/* ---- rules: a key the config names, a test of the hex, and why an entry is disabled ---- */

static bool on_tile(const RadialTarget *t, bool characters_only) {
    Hex h = { t->row, t->col };
    for (int i = 0; i < gh->entities.count; i++) {
        const Entity *e = gh->entities.items[i];
        if (entity_occupies(e, h) && (!characters_only || e->type == ENTITY_CHARACTER)) return true;
    }
    return false;
}

/* The character the "actions" menu belongs to (radial_game_open picks it). */
static uint32_t actor;

static bool rule_gm(const RadialTarget *t) { return !session_is_client(); } /* the GM, or playing alone */
static bool rule_occupied(const RadialTarget *t) { return on_tile(t, false); }
static bool rule_character(const RadialTarget *t) { return on_tile(t, true); }

/* Can the actor use move action arg now? */
static const char *rule_move(const RadialTarget *t, int arg) {
    (void)t;
    OpResult r = move_check(entity_by_id(actor), (MoveAction)arg);
    return r == OPR_OK ? NULL : move_result_text(r);
}
#define MOVE_RULE(key_, action_) { key_, NULL, NULL, "Not possible", rule_move, action_ }

/* Can the actor use recover action arg now? */
static const char *rule_recover(const RadialTarget *t, int arg) {
    (void)t;
    OpResult r = recover_check(entity_by_id(actor), (RecoverAction)arg);
    return r == OPR_OK ? NULL : move_result_text(r);
}
#define RECOVER_RULE(key_, action_) { key_, NULL, NULL, "Not possible", rule_recover, action_ }

/* Can the actor use attack arg now? */
static const char *rule_attack(const RadialTarget *t, int arg) {
    (void)t;
    OpResult r = attack_check(entity_by_id(actor), (AttackAction)arg);
    return r == OPR_OK ? NULL : move_result_text(r);
}
#define ATTACK_RULE(key_, action_) { key_, NULL, NULL, "Not possible", rule_attack, action_ }

/* Can the actor use block arg now? */
static const char *rule_block(const RadialTarget *t, int arg) {
    (void)t;
    OpResult r = block_check(entity_by_id(actor), (BlockAction)arg);
    return r == OPR_OK ? NULL : move_result_text(r);
}
#define BLOCK_RULE(key_, action_) { key_, NULL, NULL, "Not possible", rule_block, action_ }

/* Can the actor use dodge arg now? */
static const char *rule_dodge(const RadialTarget *t, int arg) {
    (void)t;
    OpResult r = dodge_check(entity_by_id(actor), (DodgeAction)arg);
    return r == OPR_OK ? NULL : move_result_text(r);
}

/* Can the actor take back its last declaration now (phases.h)? */
static const char *rule_take_back(const RadialTarget *t, int arg) {
    (void)t, (void)arg;
    OpResult r = phases_take_back_check(entity_by_id(actor));
    return r == OPR_OK ? NULL : move_result_text(r);
}

/* The block ring's toggles: their direction bits. */
static int toggled_dirs(void) {
    return (radial_toggled("block.up") ? DIR_BIT(DIR_UP) : 0) | (radial_toggled("block.left") ? DIR_BIT(DIR_LEFT) : 0) |
           (radial_toggled("block.right") ? DIR_BIT(DIR_RIGHT) : 0);
}
static bool rule_block_dirs(const RadialTarget *t) { return (void)t, toggled_dirs() != 0; }

static const RadialRule RULES[] = {
    { "gm", rule_gm, "Only the GM can do this", "The GM can't do this", NULL, 0 },
    { "occupied", rule_occupied, "Needs something on the tile", "Needs an empty tile", NULL, 0 },
    { "character", rule_character, "Needs a character on the tile", "Not with a character on the tile", NULL, 0 },
    /* one per move action; the keys are MOVE_ACTIONS' ids (tests/core_test.c checks that they match) */
    MOVE_RULE("move.step", MOVE_STEP),
    MOVE_RULE("move.walk", MOVE_WALK),
    MOVE_RULE("move.run", MOVE_RUN),
    MOVE_RULE("move.sprint", MOVE_SPRINT),
    MOVE_RULE("move.crawl", MOVE_CRAWL),
    /* one per recover action; the keys are RECOVER_ACTIONS' ids */
    RECOVER_RULE("stamina.recover", RECOVER_SMALL),
    RECOVER_RULE("stamina.full_recover", RECOVER_FULL),
    /* one per attack; the keys are ATTACK_ACTIONS' ids */
    ATTACK_RULE("attack.test", ATTACK_TEST),
    /* one per block; the keys are BLOCK_ACTIONS' ids */
    BLOCK_RULE("block.test", BLOCK_TEST),
    BLOCK_RULE("block.test_range", BLOCK_TEST_RANGE),
    { "block.dirs", rule_block_dirs, "Choose at least one direction", "", NULL, 0 },
    /* one per dodge; the keys are DODGE_ACTIONS' ids */
    { "dodge", NULL, NULL, "Not possible", rule_dodge, DODGE_BASIC },
    { "take_back", NULL, NULL, "Not possible", rule_take_back, 0 },
};
#define NRULES ((int)(sizeof(RULES) / sizeof(RULES[0])))

const RadialRule *radial_game_rules(int *n) {
    *n = NRULES;
    return RULES;
}

/* ---- which menu ---- */

bool radial_game_controls(const Entity *e) {
    if (!e || e->type != ENTITY_CHARACTER) return false;
    if (!session_is_client()) return true; /* the GM controls every character */
    return e->owner[0] && strcasecmp(e->owner, session_my_name()) == 0;
}

/* The character on h the menu is for: the selected one, else the one whose turn it is, else the first one, of
 * those the local user controls. 0 if none. */
/* on h: where it will be, or where its token is drawn until its declared moves resolve (phases.h) */
static bool is_on(const Entity *e, Hex h) { return entity_occupies(e, h) || phases_shown_on(e, h); }

static uint32_t actor_on(Hex h) {
    Entity *sel = gh->selectedEntities.count > 0 ? entity_by_id(gh->selectedEntities.items[0]) : NULL;
    if (radial_game_controls(sel) && is_on(sel, h)) return sel->id;
    if (radial_game_controls(gh->currentTurn) && is_on(gh->currentTurn, h)) return gh->currentTurn->id;
    for (int i = 0; i < gh->entities.count; i++) {
        const Entity *e = gh->entities.items[i];
        if (radial_game_controls(e) && is_on(e, h)) return e->id;
    }
    return 0;
}

const char *radial_game_menu_for(Hex h) {
    if (actor_on(h)) return "actions";
    RadialTarget t = { h.row, h.col };
    return rule_character(&t) ? "character" : "tile";
}

uint32_t radial_game_actor(void) {
    const char *m = radial_menu_id();
    return m && strcmp(m, "actions") == 0 ? actor : 0;
}

/* ---- events ---- */

/* The direction menu's entry id "dir.<name>" -> AttackDir, -1 if none. */
static int direction_of(const char *id) {
    if (strncmp(id, "dir.", 4) != 0) return -1;
    for (int d = 0; d < DIR_COUNT; d++)
        if (strcmp(id + 4, ATTACK_DIR_NAMES[d]) == 0) return d;
    return -1;
}

static void on_event(const RadialEvent *ev) {
    bool directions = ev->menu && strcmp(ev->menu, "direction") == 0;
    bool blocking = ev->menu && strcmp(ev->menu, "block") == 0;
    switch (ev->type) {
    case RADIAL_EV_ACTION: {
        if (directions) {
            int d = direction_of(ev->id);
            if (d >= 0) target_ui_attack(d);
            break;
        }
        if (blocking) { /* the centre: block in the directions that are on */
            if (strcmp(ev->id, "block.confirm") == 0) target_ui_block(toggled_dirs());
            break;
        }
        int move = move_action_by_id(ev->id), recover = recover_action_by_id(ev->id);
        int attack = attack_action_by_id(ev->id), block = block_action_by_id(ev->id);
        int dodge = dodge_action_by_id(ev->id);
        if (move >= 0) {
            move_ui_start(actor, move);
        } else if (attack >= 0) {
            target_ui_start(TARGET_ATTACK, actor, attack);
        } else if (block >= 0) {
            target_ui_start(TARGET_BLOCK, actor, block);
        } else if (dodge >= 0) {
            target_ui_start(TARGET_DODGE, actor, dodge);
        } else if (recover >= 0) { /* nothing to choose: done at once */
            Op op = { .type = OP_RECOVER_ACTION, .id = actor, .action = recover };
            ops_submit(&op);
        } else if (strcmp(ev->id, "char.create") == 0) { /* the creation steps, with this tile */
            Hex h = { ev->target.row, ev->target.col };
            cc_start(&h);
        } else if (strcmp(ev->id, "take_back") == 0) {
            Op op = { .type = OP_TAKE_BACK, .id = actor };
            ops_submit(&op);
        } else if (strcmp(ev->id, "info") == 0) {
            /* nothing yet */
        } else { /* placeholder entries: they only say what was chosen */
            console_log("Menu: %s on [%d|%d]", ev->id, ev->target.row, ev->target.col);
        }
        break;
    }
    case RADIAL_EV_CLOSED:
        if (directions || blocking) target_ui_back(); /* nothing chosen: choose the tile again */
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
    move_ui_cancel(); /* a new menu replaces choosing a tile */
    target_ui_cancel();
    actor = actor_on(h);
    return radial_open(radial_game_menu_for(h), &t, (float)c.x, (float)c.y);
}

bool radial_game_open_menu(const char *menu, Hex h) {
    radial_game_refresh_screen();
    RadialTarget t = { h.row, h.col };
    Vec2 c = hex_center(h);
    return radial_open(menu, &t, (float)c.x, (float)c.y);
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
