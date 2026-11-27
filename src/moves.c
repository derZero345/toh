/* Move actions (see moves.h). */
#include "moves.h"

#include <string.h>

const MoveActionDef MOVE_ACTIONS[MOVE_COUNT] = {
    [MOVE_STEP] = { "move.step", "Step", SLOT_MINOR, 1, DIST_ONE, false },
    [MOVE_WALK] = { "move.walk", "Walk", SLOT_MAJOR, 0, DIST_HALF_SPEED, false },
    [MOVE_RUN] = { "move.run", "Run", SLOT_MAJOR, 2, DIST_SPEED, false },
    [MOVE_SPRINT] = { "move.sprint", "Sprint", SLOT_MINOR, 3, DIST_SPEED, true },
    [MOVE_CRAWL] = { "move.crawl", "Crawl", SLOT_MAJOR, 2, DIST_HALF_SPEED, false },
};

int move_action_by_id(const char *id) {
    for (int i = 0; id && i < MOVE_COUNT; i++)
        if (strcmp(MOVE_ACTIONS[i].id, id) == 0) return i;
    return -1;
}

static bool valid(MoveAction a) { return (int)a >= 0 && a < MOVE_COUNT; }

int move_distance(const Entity *e, MoveAction a) {
    if (!e || !valid(a)) return 0;
    int speed = e->speed < 0 ? 0 : e->speed > MAX_SPEED ? MAX_SPEED : e->speed;
    switch (MOVE_ACTIONS[a].distance) {
    case DIST_ONE: return 1;
    case DIST_HALF_SPEED: return (speed + 1) / 2;
    case DIST_SPEED: return speed;
    }
    return 0;
}

int move_slot(const Entity *e, MoveAction a) {
    if (!e || !valid(a)) return -1;
    if (MOVE_ACTIONS[a].slot == SLOT_MINOR && !(e->turnUsed & TURN_MINOR)) return SLOT_MINOR;
    return e->turnUsed & TURN_MAJOR ? -1 : SLOT_MAJOR; /* a Major action can be used as a Minor one */
}

/* A character without a stamina stat (max 0) pays nothing. */
static bool tracks_stamina(const Entity *e) { return e->maxStamina > 0; }

OpResult move_check(const Entity *e, MoveAction a) {
    if (!e || e->type != ENTITY_CHARACTER) return OPR_NO_ENTITY;
    if (!valid(a)) return OPR_INVALID;
    if (!gh->testMode) { /* test mode: these requirements never block (moves.h) */
        if (gh->init.count == 0) return OPR_NOT_IN_COMBAT;
        if (gh->currentTurn != e) return OPR_NOT_YOUR_TURN;
        if (move_slot(e, a) < 0) return OPR_ACTION_USED;
        if (MOVE_ACTIONS[a].afterMove && !(e->turnUsed & TURN_MOVED)) return OPR_MOVE_FIRST;
        if (tracks_stamina(e) && e->stamina < MOVE_ACTIONS[a].stamina) return OPR_NO_STAMINA;
    }
    if (move_distance(e, a) <= 0) return OPR_CANT_MOVE;
    return OPR_OK;
}

void move_range(const Entity *e, MoveAction a, TileSet *out) {
    astar_range(e->tile, move_distance(e, a), (Entity *)e, out);
}

OpResult move_check_to(const Entity *e, MoveAction a, Hex tile) {
    OpResult r = move_check(e, a);
    if (r != OPR_OK) return r;
    if (hex_eq(tile, e->tile) || hex_distance(e->tile, tile) > move_distance(e, a)) return OPR_OUT_OF_RANGE;
    TileSet range = { 0 };
    move_range(e, a, &range);
    bool ok = ts_has(&range, tile);
    ts_free(&range);
    return ok ? OPR_OK : OPR_OUT_OF_RANGE;
}

void move_apply(Entity *e, MoveAction a, Hex tile, HexList *path, bool log) {
    const MoveActionDef *d = &MOVE_ACTIONS[a];
    int slot = move_slot(e, a);
    if (slot < 0) slot = (int)d->slot; /* test mode: none is free; mark its own */
    e->turnUsed |= (slot == SLOT_MINOR ? TURN_MINOR : TURN_MAJOR) | TURN_MOVED;
    if (path) astar_path_within(e->tile, tile, move_distance(e, a), e, path);
    e->tile = tile;
    bool spent = tracks_stamina(e) && d->stamina > 0;
    int before = e->stamina;
    if (spent) {
        long long left = (long long)e->stamina - d->stamina;
        if (left < 0) left = before < 0 ? before : 0; /* test mode: spends what there is, never below 0 */
        e->stamina = hp_clamp(left, e->maxStamina);
    }
    char name[CHAR_NAME_MAX + 16];
    entity_display_name(e, name, sizeof(name));
    if (log) {
        if (spent) console_log("%s: %s, stamina %d/%d", name, d->name, e->stamina, e->maxStamina);
        else console_log("%s: %s", name, d->name);
    }
    /* exhausted: the turn ends (everyone sees it); only when this action used up the last stamina */
    if (spent && before > 0 && e->stamina <= 0 && gh->currentTurn == e) {
        console_log("%s is exhausted; the turn ends", name);
        game_next_turn();
    }
}

const char *move_result_text(OpResult r) {
    switch (r) {
    case OPR_NOT_IN_COMBAT: return "Only during initiative";
    case OPR_NOT_YOUR_TURN: return "Not this character's turn";
    case OPR_ACTION_USED:   return "No action of that kind left this turn";
    case OPR_MOVE_FIRST:    return "Needs another move action first this turn";
    case OPR_NO_STAMINA:    return "Not enough stamina";
    case OPR_CANT_MOVE:     return "This character can't move (speed 0)";
    default:                return ops_result_text(r);
    }
}
