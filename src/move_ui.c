/* Choosing where a move action goes (see move_ui.h). */
#include "move_ui.h"
#include "moves.h"
#include "ops.h"

#define MM (gh->moveMode)

static Hex start_tile; /* where the character stood when the mode started */
static Hex hovered;    /* the tile MM.path leads to */
static bool has_hovered;

bool move_ui_active(void) { return MM.active; }

void move_ui_cancel(void) {
    if (!MM.active) return;
    MM.active = false;
    ts_free(&MM.range);
    da_clear(&MM.path);
}

bool move_ui_start(uint32_t id, int action) {
    move_ui_cancel();
    Entity *e = entity_by_id(id);
    OpResult r = move_check(e, (MoveAction)action);
    if (r != OPR_OK) {
        console_log("%s", move_result_text(r));
        return false;
    }
    MM.active = true;
    MM.id = id;
    MM.action = action;
    start_tile = e->tile;
    move_range(e, (MoveAction)action, &MM.range);
    entities_clear_highlights(); /* the normal movement range would mix with the action's */
    char name[CHAR_NAME_MAX + 16];
    entity_display_name(e, name, sizeof(name));
    console_log("%s: %s up to %d tiles; click where to go (Esc cancels)", name, MOVE_ACTIONS[action].name,
                move_distance(e, (MoveAction)action));
    has_hovered = false;
    move_ui_update();
    return true;
}

static void hover(const Entity *e, Hex h, bool has_tile) {
    if (has_tile == has_hovered && (!has_tile || hex_eq(h, hovered))) return;
    hovered = h;
    has_hovered = has_tile;
    da_clear(&MM.path);
    if (!has_tile || hex_eq(h, e->tile) || !ts_has(&MM.range, h)) return;
    astar_path_within(e->tile, h, move_distance(e, (MoveAction)MM.action), (Entity *)e, &MM.path);
}

bool move_ui_click(Hex h) {
    if (!MM.active) return false;
    Entity *e = entity_by_id(MM.id);
    if (!e || !ts_has(&MM.range, h) || hex_eq(h, e->tile)) {
        move_ui_cancel();
        console_log("Move cancelled");
        return true;
    }
    Op op = { .type = OP_MOVE_ACTION, .id = e->id, .action = MM.action, .tile = h };
    move_ui_cancel();
    if (ops_submit(&op) != OPR_OK) return true;
    /* offline and on the host it moved already: show its new range; a client selects it when the host answers */
    if (hex_eq(e->tile, h) && entity_by_id(op.id) == e) io_select_entity(e);
    return true;
}

void move_ui_update(void) {
    if (!MM.active) return;
    Entity *e = entity_by_id(MM.id);
    if (!e || !hex_eq(e->tile, start_tile) || move_check(e, (MoveAction)MM.action) != OPR_OK) {
        move_ui_cancel();
        return;
    }
    hover(e, gh->tileUnderMouse, gh->hasTileUnderMouse);
}
