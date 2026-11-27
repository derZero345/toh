/* Choosing where an attack goes (see attack_ui.h). */
#include "attack_ui.h"
#include "attacks.h"
#include "ops.h"
#include "radial_game.h"

#define AM (gh->attackMode)

static Hex start_tile; /* where the attacker stood when the mode started */

bool attack_ui_active(void) { return AM.active; }

void attack_ui_cancel(void) {
    if (!AM.active) return;
    AM.active = false;
    if (AM.hasTarget) { /* the direction menu belongs to the mode */
        AM.hasTarget = false;
        radial_game_close();
    }
    ts_free(&AM.range);
}

bool attack_ui_start(uint32_t id, int action) {
    attack_ui_cancel();
    Entity *e = entity_by_id(id);
    OpResult r = attack_check(e, (AttackAction)action);
    if (r != OPR_OK) {
        console_log("%s", move_result_text(r));
        return false;
    }
    AM.active = true;
    AM.id = id;
    AM.action = action;
    AM.hasTarget = false;
    start_tile = e->tile;
    attack_range(e, (AttackAction)action, &AM.range);
    entities_clear_highlights(); /* the movement range would mix with the attack's */
    char name[CHAR_NAME_MAX + 16];
    entity_display_name(e, name, sizeof(name));
    console_log("%s: %s, range %d; click the tile to attack (Esc cancels)", name, ATTACK_ACTIONS[action].name,
                ATTACK_ACTIONS[action].range);
    return true;
}

bool attack_ui_click(Hex h) {
    if (!AM.active) return false;
    if (AM.hasTarget) return true; /* the direction menu is open and takes the presses */
    if (!ts_has(&AM.range, h)) {
        attack_ui_cancel();
        console_log("Attack cancelled");
        return true;
    }
    AM.target = h;
    AM.hasTarget = radial_game_open_directions(h);
    return true;
}

void attack_ui_choose(int d) {
    if (!AM.active || !AM.hasTarget) return;
    Op op = { .type = OP_ATTACK_ACTION, .id = AM.id, .action = AM.action, .tile = AM.target, .value = d };
    AM.hasTarget = false; /* the menu closes by itself after the choice */
    attack_ui_cancel();
    ops_submit(&op);
    op_free(&op);
}

void attack_ui_back(void) {
    if (AM.active) AM.hasTarget = false;
}

void attack_ui_update(void) {
    if (!AM.active) return;
    Entity *e = entity_by_id(AM.id);
    if (!e || !hex_eq(e->tile, start_tile) || attack_check(e, (AttackAction)AM.action) != OPR_OK) attack_ui_cancel();
}
