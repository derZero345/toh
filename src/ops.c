/* The op layer: rules for who may change what (ops_check) and the changes themselves (ops_apply). */
#include "ops.h"
#include "savefmt.h"
#include "session.h"

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

/* Offline and on the host the local user is the game master. */
OpAuthor ops_local_author(void) {
    if (!session_is_client()) return (OpAuthor){ .gm = true };
    OpAuthor a = { .gm = false };
    snprintf(a.name, sizeof(a.name), "%s", session_my_name());
    return a;
}

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
    case OPR_ALREADY_OWNED: return "That character already has an owner";
    case OPR_CLAIM_PENDING: return "Wait for the GM to answer your earlier claim";
    case OPR_TOO_FAST:      return "Too many rolls; wait a moment";
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

static void apply_spawn(Op *op) {
    bool keep = op->keepIds && op->ids.count == op->specs.count;
    uint32_t *given = NULL; /* the host's ids; op->ids gets refilled below */
    if (keep) {
        given = op->ids.items;
        memset(&op->ids, 0, sizeof(op->ids));
    }
    da_clear(&op->ids);
    for (int i = 0; i < op->specs.count; i++) {
        const EntitySpec *s = &op->specs.items[i];
        Entity *e;
        if (s->type == ENTITY_WALL) {
            e = entity_new_wall(s->image, s->tile);
        } else {
            e = entity_new_character(s->image, s->tile, s->size, s->maxHealth, s->ac, s->speed, s->initiative);
            e->health = s->health;
            e->attack = s->attack;
        }
        if (keep) entity_register_as(e, given[i]);
        else entity_register(e);
        da_push(&op->ids, e->id);
    }
    free(given);
}

/* The entity whose selection highlights are shown, if any. */
static Entity *selected_entity(void) {
    return gh->hasSelectedEntity && gh->selectedEntityTiles.count > 0 ? entity_at(gh->selectedEntityTiles.items[0]) : NULL;
}

static void apply_delete(const Op *op) {
    bool deleted = false, deleted_selected = false;
    Entity *sel = selected_entity();
    for (int i = 0; i < op->ids.count; i++) {
        Entity *e = entity_by_id(op->ids.items[i]);
        if (!e) continue;
        if (e == sel) deleted_selected = true;
        entity_delete(e);
        deleted = true;
    }
    /* someone else's delete only clears the local highlights when they showed a deleted entity */
    if (op->remote ? deleted_selected : deleted) entities_clear_highlights();
}

static void apply_move(const Op *op) {
    Entity *e = entity_by_id(op->id);
    if (!e) return;
    if (!op->remote) {
        game_move_character(op->tile, e); /* also shows the path it took */
        return;
    }
    bool was_selected = e == selected_entity();
    e->tile = op->tile;
    if (was_selected) entities_clear_highlights(); /* the range shown belonged to the old tile */
}

/* The characters among op->ids; walls are skipped like in the Java version. */
static Entity *character_at(const Op *op, int i) {
    Entity *e = entity_by_id(op->ids.items[i]);
    return e && e->type == ENTITY_CHARACTER ? e : NULL;
}

static void apply_stats(const Op *op) {
    for (int i = 0; i < op->ids.count; i++) {
        Entity *e = character_at(op, i);
        if (!e) continue;
        if (op->statMask & STAT_SIZE)       e->size = op->size;
        if (op->statMask & STAT_MAXHEALTH)  e->maxHealth = op->maxHealth;
        if (op->statMask & STAT_AC)         e->ac = op->ac;
        if (op->statMask & STAT_SPEED)      e->speed = op->speed > MAX_SPEED ? MAX_SPEED : op->speed;
        if (op->statMask & STAT_INITIATIVE) e->initiative = op->initiative;
    }
}

static Waypoint *waypoint_named(const char *name) {
    for (int i = 0; i < gh->waypoints.count; i++)
        if (strcasecmp(gh->waypoints.items[i].name, name) == 0) return &gh->waypoints.items[i];
    return NULL;
}

static void apply_waypoint_set(const Op *op) {
    Waypoint *w = waypoint_named(op->name);
    if (w) {
        w->tile = op->tile;
        return;
    }
    Waypoint nw = { .tile = op->tile };
    snprintf(nw.name, sizeof(nw.name), "%s", op->name);
    da_push(&gh->waypoints, nw);
}

static void apply_init_add(const Op *op) {
    EntityList es = { 0 };
    DA(double) vs = { 0 };
    for (int i = 0; i < op->ids.count; i++) {
        Entity *e = entity_by_id(op->ids.items[i]);
        if (!e) continue;
        if (i >= op->values.count) break;
        da_push(&es, e);
        da_push(&vs, op->values.items[i]);
    }
    game_initiative_list(es.items, vs.items, es.count);
    da_free(&es);
    da_free(&vs);
}

void ops_apply(Op *op) {
    switch (op->type) {
    case OP_MOVE:
        apply_move(op);
        break;
    case OP_SPAWN:
        apply_spawn(op);
        break;
    case OP_DELETE:
        apply_delete(op);
        break;
    case OP_SET_STATS:
        apply_stats(op);
        break;
    case OP_SET_ICON:
        for (int i = 0; i < op->ids.count; i++) {
            Entity *e = character_at(op, i);
            if (e) entity_set_image(e, op->image);
        }
        break;
    case OP_SET_BACKGROUND:
        gfx_set_background_texture(op->image);
        break;
    case OP_GRID_ORIENTATION:
        gfx_toggle_grid_orientation();
        break;
    case OP_WAYPOINT_SET:
        apply_waypoint_set(op);
        break;
    case OP_WAYPOINT_DELETE: {
        Waypoint *w = waypoint_named(op->name);
        if (w) da_remove_at(&gh->waypoints, (int)(w - gh->waypoints.items));
        break;
    }
    case OP_INIT_ADD:
        apply_init_add(op);
        break;
    case OP_INIT_REMOVE:
        for (int i = 0; i < op->ids.count; i++) {
            Entity *e = entity_by_id(op->ids.items[i]);
            if (e) game_remove_from_initiative(e);
        }
        break;
    case OP_INIT_CLEAR:
        game_clear_initiative();
        break;
    case OP_INIT_SHOW:
        game_toggle_show_initiative();
        break;
    case OP_INIT_NEXT:
        game_next_turn();
        break;
    case OP_SET_OWNER:
        for (int i = 0; i < op->ids.count; i++) {
            Entity *e = character_at(op, i);
            if (e) snprintf(e->owner, sizeof(e->owner), "%.*s", PLAYER_NAME_MAX - 1, op->name);
        }
        break;
    case OP_COUNT:
        break;
    }
}

OpResult ops_submit(Op *op) {
    OpAuthor me = ops_local_author();
    OpResult r = ops_check(op, &me);
    if (r != OPR_OK) {
        console_log("%s", ops_result_text(r));
        return r;
    }
    if (session_is_client()) {
        session_send_op(op); /* applied when the host broadcasts it */
        return OPR_OK;
    }
    ops_apply(op);
    if (session_is_host()) session_broadcast_op(op);
    return OPR_OK;
}
