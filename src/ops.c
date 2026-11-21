/* The op layer: rules for who may change what (ops_check) and the changes themselves (ops_apply). */
#include "ops.h"
#include "savefmt.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

_Static_assert(PLAYER_NAME_MAX <= WAYPOINT_NAME_MAX, "Op.name must hold player names");

void op_free(Op *op) {
    da_free(&op->ids);
    da_free(&op->values);
    da_free(&op->specs);
}

EntitySpec op_spec_from_entity(const Entity *e) {
    return (EntitySpec){ .type = e->type, .image = e->image, .tile = e->tile, .size = e->size,
                         .maxHealth = e->maxHealth, .health = e->health, .attack = e->attack, .ac = e->ac,
                         .speed = e->speed, .initiative = e->initiative };
}

/* Offline the local user is the game master. */
OpAuthor ops_local_author(void) { return (OpAuthor){ .gm = true }; }

const char *ops_result_text(OpResult r) {
    switch (r) {
    case OPR_OK:            return "OK";
    case OPR_GM_ONLY:       return "Only the GM can do that";
    case OPR_NOT_YOURS:     return "Not your character";
    case OPR_NOT_YOUR_TURN: return "It's not your character's turn";
    case OPR_OUT_OF_RANGE:  return "Out of range";
    case OPR_NO_ENTITY:     return "That entity no longer exists";
    case OPR_STALE:         return "The turn order changed in the meantime";
    case OPR_INVALID:       return "Invalid action";
    }
    return "Unknown error";
}

/* ---- ops_check --------------------------------------------------------------------------- */

static bool ids_exist(const Op *op) {
    for (int i = 0; i < op->ids.count; i++)
        if (!entity_by_id(op->ids.items[i])) return false;
    return true;
}

static bool is_owner(const Entity *e, const OpAuthor *a) {
    return e && e->owner[0] != '\0' && strcasecmp(e->owner, a->name) == 0;
}

static bool tile_ok(Hex h) {
    return h.row >= -SAVEFMT_MAX_COORD && h.row <= SAVEFMT_MAX_COORD &&
           h.col >= -SAVEFMT_MAX_COORD && h.col <= SAVEFMT_MAX_COORD;
}

static bool count_ok(int n) { return n >= 0 && (unsigned)n <= SAVEFMT_MAX_ENTITIES; }

/* Rules for everyone, the GM included: the op has to make sense for the current table. */
static OpResult check_structure(const Op *op) {
    if (!memchr(op->name, '\0', sizeof(op->name))) return OPR_INVALID;
    if (!count_ok(op->ids.count) || !count_ok(op->values.count) || !count_ok(op->specs.count)) return OPR_INVALID;
    switch (op->type) {
    case OP_MOVE: {
        if (!tile_ok(op->tile)) return OPR_INVALID;
        const Entity *e = entity_by_id(op->id);
        return e && e->type == ENTITY_CHARACTER ? OPR_OK : OPR_NO_ENTITY;
    }
    case OP_SPAWN:
        for (int i = 0; i < op->specs.count; i++) {
            const EntitySpec *s = &op->specs.items[i];
            if (!tile_ok(s->tile)) return OPR_INVALID;
            if (!s->image || (s->type != ENTITY_CHARACTER && s->type != ENTITY_WALL)) return OPR_INVALID;
        }
        return OPR_OK;
    case OP_SET_ICON:
        if (!op->image) return OPR_INVALID;
        return ids_exist(op) ? OPR_OK : OPR_NO_ENTITY;
    case OP_INIT_ADD:
        if (op->values.count != op->ids.count) return OPR_INVALID;
        for (int i = 0; i < op->values.count; i++)
            if (!isfinite(op->values.items[i])) return OPR_INVALID;
        return ids_exist(op) ? OPR_OK : OPR_NO_ENTITY;
    case OP_SET_OWNER:
        if (op->name[0] != '\0' && !savefmt_owner_valid(op->name)) return OPR_INVALID;
        return ids_exist(op) ? OPR_OK : OPR_NO_ENTITY;
    case OP_DELETE:
    case OP_SET_STATS:
    case OP_INIT_REMOVE:
        return ids_exist(op) ? OPR_OK : OPR_NO_ENTITY;
    case OP_WAYPOINT_SET:
        if (!tile_ok(op->tile)) return OPR_INVALID;
        return op->name[0] != '\0' ? OPR_OK : OPR_INVALID;
    case OP_WAYPOINT_DELETE:
        return op->name[0] != '\0' ? OPR_OK : OPR_INVALID;
    case OP_INIT_NEXT:
        /* op->id names the entity whose turn it is now, so a stale op is not applied twice */
        return op->id == (gh->currentTurn ? gh->currentTurn->id : 0) ? OPR_OK : OPR_STALE;
    case OP_SET_BACKGROUND:
    case OP_GRID_ORIENTATION:
    case OP_INIT_CLEAR:
    case OP_INIT_SHOW:
        return OPR_OK;
    case OP_COUNT:
        break;
    }
    return OPR_INVALID;
}

/* Players may move their own character (in range, on its turn) and pass their own turn. */
static OpResult check_player(const Op *op, const OpAuthor *a) {
    switch (op->type) {
    case OP_MOVE: {
        Entity *e = entity_by_id(op->id);
        if (!is_owner(e, a)) return OPR_NOT_YOURS;
        if (gh->init.count > 0 && gh->currentTurn != e) return OPR_NOT_YOUR_TURN;
        if (hex_distance(e->tile, op->tile) > e->speed) return OPR_OUT_OF_RANGE;
        TileSet range = { 0 };
        astar_range(e->tile, e->speed, e, &range);
        bool in_range = ts_has(&range, op->tile);
        ts_free(&range);
        return in_range ? OPR_OK : OPR_OUT_OF_RANGE;
    }
    case OP_INIT_NEXT:
        return is_owner(gh->currentTurn, a) ? OPR_OK : OPR_NOT_YOUR_TURN;
    default:
        return OPR_GM_ONLY;
    }
}

OpResult ops_check(const Op *op, const OpAuthor *author) {
    if (!memchr(author->name, '\0', sizeof(author->name))) return OPR_INVALID;
    if (!author->gm && op->type != OP_MOVE && op->type != OP_INIT_NEXT) return OPR_GM_ONLY;
    OpResult r = check_structure(op);
    if (r != OPR_OK || author->gm) return r;
    return check_player(op, author);
}

/* ---- ops_apply ---------------------------------------------------------------------------- */

void ops_apply(Op *op) {
    (void)op; /* Task 5 */
}

OpResult ops_submit(Op *op) {
    OpAuthor me = ops_local_author();
    OpResult r = ops_check(op, &me);
    if (r != OPR_OK) {
        console_log("%s", ops_result_text(r));
        return r;
    }
    ops_apply(op);
    return OPR_OK;
}
