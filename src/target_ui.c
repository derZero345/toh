/* Choosing where an attack, a block or a dodge goes (see target_ui.h). */
#include "target_ui.h"
#include "blocks.h"
#include "dodges.h"
#include "ops.h"
#include "radial_game.h"

#define TM (gh->targetMode)

static Hex start_tile; /* where the character stood when the mode started */

bool target_ui_active(void) { return TM.active; }

static OpResult check(const Entity *e) {
    switch (TM.kind) {
    case TARGET_ATTACK: return attack_check(e, (AttackAction)TM.action);
    case TARGET_BLOCK:  return block_check(e, (BlockAction)TM.action);
    default:            return dodge_check(e, (DodgeAction)TM.action);
    }
}

const char *target_ui_name(void) {
    return TM.kind == TARGET_ATTACK ? "Attack" : TM.kind == TARGET_BLOCK ? "Block" : "Dodge";
}

static bool own_tiles(void) { return TM.kind == TARGET_BLOCK && BLOCK_ACTIONS[TM.action].range == 0; }

void target_ui_cancel(void) {
    if (!TM.active) return;
    TM.active = false;
    if (TM.hasTarget) { /* the ring belongs to the mode */
        TM.hasTarget = false;
        radial_game_close();
    }
    ts_free(&TM.range);
}

/* Opens the ring of the action on tile h. */
static void open_ring(Hex h) {
    TM.target = h;
    TM.hasTarget = TM.kind == TARGET_ATTACK ? radial_game_open_menu("direction", h) : radial_game_open_menu("block", h);
}

bool target_ui_start(TargetKind kind, uint32_t id, int action) {
    target_ui_cancel();
    Entity *e = entity_by_id(id);
    TM.kind = kind;
    TM.action = action;
    OpResult r = check(e);
    if (r != OPR_OK) {
        console_log("%s", move_result_text(r));
        return false;
    }
    TM.active = true;
    TM.id = id;
    TM.hasTarget = false;
    start_tile = e->tile;
    if (kind == TARGET_ATTACK) attack_range(e, (AttackAction)action, &TM.range);
    else if (kind == TARGET_BLOCK) block_range(e, (BlockAction)action, &TM.range);
    else dodge_range(e, (DodgeAction)action, &TM.range);
    entities_clear_highlights(); /* the movement range would mix with the action's */
    char name[CHAR_NAME_MAX + 16];
    entity_display_name(e, name, sizeof(name));
    if (own_tiles()) {
        console_log("%s: %s; choose the directions to block (Esc cancels)", name, BLOCK_ACTIONS[action].name);
        open_ring(e->tile);
    } else if (kind == TARGET_DODGE) {
        console_log("%s: %s; click the adjacent tile to dodge to (Esc cancels)", name, DODGE_ACTIONS[action].name);
    } else {
        const char *what = kind == TARGET_ATTACK ? ATTACK_ACTIONS[action].name : BLOCK_ACTIONS[action].name;
        int range = kind == TARGET_ATTACK ? ATTACK_ACTIONS[action].range : BLOCK_ACTIONS[action].range;
        console_log("%s: %s, range %d; click the tile to %s (Esc cancels)", name, what, range,
                    kind == TARGET_ATTACK ? "attack" : "block");
    }
    return true;
}

bool target_ui_click(Hex h) {
    if (!TM.active) return false;
    if (TM.hasTarget) return true; /* the ring is open and takes the presses */
    if (!ts_has(&TM.range, h)) {
        target_ui_cancel();
        console_log("%s cancelled", target_ui_name());
        return true;
    }
    if (TM.kind == TARGET_DODGE) { /* no ring: the step is the choice */
        Op op = { .type = OP_DODGE_ACTION, .id = TM.id, .action = TM.action, .tile = h };
        target_ui_cancel();
        ops_submit(&op);
        op_free(&op);
        return true;
    }
    open_ring(h);
    return true;
}

static void submit(OpType type, int value) {
    Op op = { .type = type, .id = TM.id, .action = TM.action, .tile = TM.target, .value = value };
    TM.hasTarget = false; /* the ring closes by itself after the choice */
    target_ui_cancel();
    ops_submit(&op);
    op_free(&op);
}

void target_ui_attack(int d) {
    if (TM.active && TM.hasTarget && TM.kind == TARGET_ATTACK) submit(OP_ATTACK_ACTION, d);
}

void target_ui_block(int dirs) {
    if (TM.active && TM.hasTarget && TM.kind == TARGET_BLOCK) submit(OP_BLOCK_ACTION, dirs);
}

void target_ui_back(void) {
    if (!TM.active || !TM.hasTarget) return;
    TM.hasTarget = false;
    if (own_tiles()) { /* nothing to choose but the directions */
        target_ui_cancel();
        console_log("Block cancelled");
    }
}

void target_ui_update(void) {
    if (!TM.active) return;
    Entity *e = entity_by_id(TM.id);
    if (!e || !hex_eq(e->tile, start_tile) || check(e) != OPR_OK) target_ui_cancel();
}

int target_ui_slot(void) {
    Entity *e = entity_by_id(TM.id);
    ActionSlot s = TM.kind == TARGET_ATTACK  ? ATTACK_ACTIONS[TM.action].slot
                   : TM.kind == TARGET_BLOCK ? BLOCK_ACTIONS[TM.action].slot
                                             : DODGE_ACTIONS[TM.action].slot;
    return e && action_slot(e, s) == SLOT_MINOR ? SLOT_MINOR : SLOT_MAJOR;
}
