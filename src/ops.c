/* The op layer: rules for who may change what (ops_check) and the changes themselves (ops_apply). */
#include "ops.h"
#include "attacks.h"
#include "blocks.h"
#include "dodges.h"
#include "moves.h"
#include "recover.h"
#include "savefmt.h"
#include "session.h"

#include <math.h>
#include <stdlib.h>
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
    EntitySpec s = { .type = e->type, .image = e->image, .tile = e->tile, .size = e->size,
                     .maxHealth = e->maxHealth, .health = e->health, .attack = e->attack,
                     .speed = e->speed, .initiative = e->initiative,
                     .maxStamina = e->maxStamina, .stamina = e->stamina, .evasion = e->evasion };
    snprintf(s.name, sizeof(s.name), "%s", e->name);
    snprintf(s.party, sizeof(s.party), "%s", e->party);
    memcpy(s.attr, e->attr, sizeof(s.attr));
    return s;
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
    case OPR_CLAIM_PENDING: return "Wait for the GM to answer your earlier requests";
    case OPR_TOO_FAST:      return "Too many rolls; wait a moment";
    case OPR_TILE_TAKEN:    return "No room on that tile";
    case OPR_NO_STAMINA:    return "Not enough stamina";
    case OPR_NOT_IN_COMBAT: return "Actions are only for initiative";
    case OPR_ACTION_USED:   return "That action is used up this turn";
    case OPR_MOVE_FIRST:    return "Sprint needs another move action first";
    case OPR_CANT_MOVE:     return "That character can't move";
    case OPR_USE_ACTION:    return "During initiative, move with a move action: right click your character";
    case OPR_NO_STAMINA_STAT: return "That character has no stamina";
    case OPR_STAMINA_FULL:  return "Stamina is already full";
    case OPR_BLOCK_LOCKED:  return "Can't block this tile this turn: its last block there failed";
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
    if (!memchr(op->name, '\0', sizeof(op->name)) || !memchr(op->charName, '\0', sizeof(op->charName)) ||
        !memchr(op->party, '\0', sizeof(op->party)))
        return OPR_INVALID;
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
            if (!memchr(s->name, '\0', sizeof(s->name))) return OPR_INVALID;
            if (s->name[0] && (s->type != ENTITY_CHARACTER || !savefmt_char_name_valid(s->name))) return OPR_INVALID;
            if (!memchr(s->party, '\0', sizeof(s->party))) return OPR_INVALID;
            if (s->party[0] && (s->type != ENTITY_CHARACTER || !savefmt_char_name_valid(s->party))) return OPR_INVALID;
            if (s->maxStamina < 0 || s->maxStamina > SAVEFMT_MAX_HP) return OPR_INVALID; /* the wire's limit */
            if (s->type == ENTITY_CHARACTER && (s->evasion < 0 || s->evasion > EVASION_MAX)) return OPR_INVALID;
            for (int a = 0; a < ATTR_COUNT; a++)
                if (!attr_valid(s->attr[a])) return OPR_INVALID;
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
    case OP_SET_HEALTH:
        if (op->healthMode != HEALTH_SET && op->healthMode != HEALTH_CHANGE) return OPR_INVALID;
        if (op->value < -SAVEFMT_MAX_HP || op->value > SAVEFMT_MAX_HP) return OPR_INVALID;
        return ids_exist(op) ? OPR_OK : OPR_NO_ENTITY;
    case OP_SET_STAMINA:
        if (op->healthMode != HEALTH_SET && op->healthMode != HEALTH_CHANGE && op->healthMode != STAMINA_SPEND)
            return OPR_INVALID;
        if (op->value < -SAVEFMT_MAX_HP || op->value > SAVEFMT_MAX_HP) return OPR_INVALID;
        if (op->healthMode == STAMINA_SPEND && op->value < 0) return OPR_INVALID;
        if (!ids_exist(op)) return OPR_NO_ENTITY;
        if (op->healthMode == STAMINA_SPEND)
            for (int i = 0; i < op->ids.count; i++) {
                const Entity *e = entity_by_id(op->ids.items[i]);
                if (e->type == ENTITY_CHARACTER && (long long)e->stamina - op->value < 0) return OPR_NO_STAMINA;
            }
        return OPR_OK;
    case OP_SET_NAME:
        if (!savefmt_char_name_valid(op->charName)) return OPR_INVALID;
        return ids_exist(op) ? OPR_OK : OPR_NO_ENTITY;
    case OP_SET_PARTY:
        if (op->party[0] && !savefmt_char_name_valid(op->party)) return OPR_INVALID;
        return ids_exist(op) ? OPR_OK : OPR_NO_ENTITY;
    case OP_SET_STATS:
        if ((op->statMask & STAT_MAXSTAMINA) && (op->maxStamina < 0 || op->maxStamina > SAVEFMT_MAX_HP))
            return OPR_INVALID;
        if (op->statMask & ~STAT_ALL) return OPR_INVALID;
        if ((op->statMask & STAT_EVASION) && (op->evasion < 0 || op->evasion > EVASION_MAX)) return OPR_INVALID;
        for (int a = 0; a < ATTR_COUNT; a++)
            if ((op->statMask & STAT_ATTR(a)) && !attr_valid(op->attr[a])) return OPR_INVALID;
        return ids_exist(op) ? OPR_OK : OPR_NO_ENTITY;
    case OP_DELETE:
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
    case OP_MOVE_ACTION: /* the rules of the action hold for the GM too; only free moves (OP_MOVE) ignore them */
        if (!tile_ok(op->tile) || op->action < 0 || op->action >= MOVE_COUNT) return OPR_INVALID;
        return move_check_to(entity_by_id(op->id), (MoveAction)op->action, op->tile);
    case OP_SET_TEST_MODE:
        return op->value == 0 || op->value == 1 ? OPR_OK : OPR_INVALID;
    case OP_RECOVER_ACTION: /* like move actions, the rules hold for the GM too */
        if (op->action < 0 || op->action >= RECOVER_COUNT || op->value < 0 || op->value > RECOVER_DIE)
            return OPR_INVALID;
        return recover_check(entity_by_id(op->id), (RecoverAction)op->action);
    case OP_ATTACK_ACTION: { /* like move actions, the rules hold for the GM too */
        if (!tile_ok(op->tile) || op->action < 0 || op->action >= ATTACK_COUNT) return OPR_INVALID;
        /* from a client nothing; from the host the rolls, then one outcome per character hit */
        if (op->values.count == 0 ? op->ids.count != 0 : op->values.count != OUTCOME_COUNT + op->ids.count)
            return OPR_INVALID;
        for (int i = 0; i < op->values.count; i++) { /* whole numbers */
            double v = op->values.items[i];
            if (!isfinite(v) || v != floor(v) || v < 0 || v > (i < OUTCOME_COUNT ? ATTACK_MAX_DAMAGE : OUTCOME_COUNT - 1))
                return OPR_INVALID;
        }
        if (op->values.count) { /* the rolls: what the attack's dice can give */
            int rolled[OUTCOME_COUNT];
            for (int o = 0; o < OUTCOME_COUNT; o++) rolled[o] = (int)op->values.items[o];
            if (!attack_rolls_ok((AttackAction)op->action, rolled)) return OPR_INVALID;
        }
        return attack_check_at(entity_by_id(op->id), (AttackAction)op->action, op->tile, op->value);
    }
    case OP_BLOCK_ACTION: /* like move actions, the rules hold for the GM too */
        if (!tile_ok(op->tile) || op->action < 0 || op->action >= BLOCK_COUNT) return OPR_INVALID;
        return block_check_at(entity_by_id(op->id), (BlockAction)op->action, op->tile, op->value);
    case OP_DODGE_ACTION: /* like move actions, the rules hold for the GM too; value: 0 or the host's roll */
        if (!tile_ok(op->tile) || op->action < 0 || op->action >= DODGE_COUNT) return OPR_INVALID;
        if (op->value != 0 && !dodge_roll_ok((DodgeAction)op->action, op->value)) return OPR_INVALID;
        return dodge_check_to(entity_by_id(op->id), (DodgeAction)op->action, op->tile);
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

/* Players may move their own character (in range outside initiative, with move actions on its turn during it),
   use recover actions with it, pass their own turn and change the HP, stamina and name of their own characters. */
static OpResult check_player(const Op *op, const OpAuthor *a) {
    switch (op->type) {
    case OP_MOVE: {
        Entity *e = entity_by_id(op->id);
        if (!is_owner(e, a)) return OPR_NOT_YOURS;
        if (gh->init.count > 0) return OPR_USE_ACTION;
        if (hex_distance(e->tile, op->tile) > e->speed) return OPR_OUT_OF_RANGE;
        TileSet range = { 0 };
        astar_range(e->tile, e->speed, e, &range);
        bool in_range = ts_has(&range, op->tile);
        ts_free(&range);
        return in_range ? OPR_OK : OPR_OUT_OF_RANGE;
    }
    case OP_INIT_NEXT:
        return is_owner(gh->currentTurn, a) ? OPR_OK : OPR_NOT_YOUR_TURN;
    case OP_MOVE_ACTION:
    case OP_RECOVER_ACTION:
    case OP_ATTACK_ACTION:
    case OP_BLOCK_ACTION:
    case OP_DODGE_ACTION:
        return is_owner(entity_by_id(op->id), a) ? OPR_OK : OPR_NOT_YOURS;
    case OP_SET_HEALTH:
    case OP_SET_STAMINA:
    case OP_SET_NAME: /* only on their own characters */
        for (int i = 0; i < op->ids.count; i++)
            if (!is_owner(entity_by_id(op->ids.items[i]), a)) return OPR_NOT_YOURS;
        return OPR_OK;
    default:
        return OPR_GM_ONLY;
    }
}

OpResult ops_check(const Op *op, const OpAuthor *author) {
    if (!memchr(author->name, '\0', sizeof(author->name))) return OPR_INVALID;
    bool player_op = op->type == OP_MOVE || op->type == OP_INIT_NEXT || op->type == OP_SET_HEALTH ||
                     op->type == OP_SET_NAME || op->type == OP_SET_STAMINA || op->type == OP_MOVE_ACTION ||
                     op->type == OP_RECOVER_ACTION || op->type == OP_ATTACK_ACTION || op->type == OP_BLOCK_ACTION ||
                     op->type == OP_DODGE_ACTION;
    if (!author->gm && !player_op) return OPR_GM_ONLY;
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
            e = entity_new_character(s->image, s->tile, s->size, s->maxHealth, s->speed, s->initiative);
            e->health = s->health;
            e->maxStamina = s->maxStamina;
            e->stamina = hp_clamp(s->stamina, s->maxStamina);
            e->attack = s->attack;
            snprintf(e->name, sizeof(e->name), "%s", s->name);
            snprintf(e->party, sizeof(e->party), "%s", s->party);
            memcpy(e->attr, s->attr, sizeof(e->attr));
            e->evasion = s->evasion;
        }
        if (keep) entity_register_as(e, given[i]);
        else entity_register(e);
        da_push(&op->ids, e->id);
    }
    free(given);
}

/* The entity whose selection highlights are shown, if any. */
static Entity *selected_entity(void) {
    return gh->hasSelectedEntity && gh->selectedEntities.count > 0 ? entity_by_id(gh->selectedEntities.items[0]) : NULL;
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

/* "Mao: STR 1 -> 2, max HP 15 -> 18" for the person who made the change */
static void log_attributes(const Entity *e, int mask, const int old_attr[ATTR_COUNT], int maxHealth, int maxStamina,
                           int speed, int initiative) {
    char name[CHAR_NAME_MAX + 16], line[256];
    entity_display_name(e, name, sizeof(name));
    int n = snprintf(line, sizeof(line), "%s:", name);
    const char *sep = " ";
#define ADD(...)                                                                                     \
    do {                                                                                             \
        if (n < (int)sizeof(line)) n += snprintf(line + n, sizeof(line) - (size_t)n, "%s", sep);    \
        if (n < (int)sizeof(line)) n += snprintf(line + n, sizeof(line) - (size_t)n, __VA_ARGS__);  \
        sep = ", ";                                                                                  \
    } while (0)
    for (int a = 0; a < ATTR_COUNT; a++) {
        if (!(mask & STAT_ATTR(a))) continue;
        ADD("%s %d -> %d", ATTR_NAMES[a], old_attr[a], e->attr[a]);
        if (a == ATTR_STR) ADD("max HP %d -> %d", maxHealth, e->maxHealth);
        if (a == ATTR_AGI) {
            ADD("max stamina %d -> %d", maxStamina, e->maxStamina);
            ADD("speed %d -> %d", speed, e->speed);
        }
        if (a == ATTR_COG) ADD("initiative %d -> %d", initiative, e->initiative);
    }
#undef ADD
    console_log("%s", line);
}

static void apply_stats(const Op *op) {
    int m = op->statMask;
    for (int i = 0; i < op->ids.count; i++) {
        Entity *e = character_at(op, i);
        if (!e) continue;
        int old_attr[ATTR_COUNT];
        memcpy(old_attr, e->attr, sizeof(old_attr));
        int oldMaxH = e->maxHealth, oldMaxSt = e->maxStamina, oldSpeed = e->speed, oldIni = e->initiative;
        for (int a = 0; a < ATTR_COUNT; a++)
            if (m & STAT_ATTR(a)) e->attr[a] = op->attr[a];
        AttrStats d = attr_derive(e->attr);
        int maxH = e->maxHealth, maxSt = e->maxStamina, speed = e->speed, ini = e->initiative;
        if (m & STAT_STR) maxH = d.maxHealth;
        if (m & STAT_AGI) {
            maxSt = d.maxStamina;
            speed = d.speed;
        }
        if (m & STAT_COG) ini = d.initiative;
        /* a value given by hand in the same op wins over the attribute's */
        if (m & STAT_MAXHEALTH) maxH = op->maxHealth;
        if (m & STAT_MAXSTAMINA) maxSt = op->maxStamina;
        if (m & STAT_SPEED) speed = op->speed;
        if (m & STAT_INITIATIVE) ini = op->initiative;
        if (m & STAT_SIZE) e->size = op->size;
        if (m & STAT_EVASION) e->evasion = op->evasion;
        if (maxH != e->maxHealth) {
            e->health = hp_after_max_change(e->health, e->maxHealth, maxH);
            e->maxHealth = maxH;
        }
        if (maxSt != e->maxStamina) {
            e->stamina = hp_after_max_change(e->stamina, e->maxStamina, maxSt);
            e->maxStamina = maxSt;
        }
        e->speed = speed > MAX_SPEED ? MAX_SPEED : speed;
        e->initiative = ini;
        if ((m & STAT_ATTRS) && !op->remote) log_attributes(e, m, old_attr, oldMaxH, oldMaxSt, oldSpeed, oldIni);
    }
}

/* Logs the result for the person who made the change (spec: "Mao: 14/20 -> 10/20 (Injured)"). */
static void apply_health(const Op *op) {
    for (int i = 0; i < op->ids.count; i++) {
        Entity *e = character_at(op, i);
        if (!e) continue;
        int before = e->health;
        long long v = op->healthMode == HEALTH_SET ? op->value : (long long)e->health + op->value;
        e->health = hp_clamp(v, e->maxHealth);
        if (op->remote) continue;
        char name[CHAR_NAME_MAX + 16];
        entity_display_name(e, name, sizeof(name));
        console_log("%s: %d/%d -> %d/%d (%s)", name, before, e->maxHealth, e->health, e->maxHealth,
                    hp_stage_name(hp_stage(e->health, e->maxHealth)));
    }
}

static void apply_stamina(const Op *op) {
    for (int i = 0; i < op->ids.count; i++) {
        Entity *e = character_at(op, i);
        if (!e) continue;
        int before = e->stamina;
        long long v = op->healthMode == HEALTH_SET    ? op->value
                    : op->healthMode == STAMINA_SPEND ? (long long)e->stamina - op->value
                                                      : (long long)e->stamina + op->value;
        e->stamina = hp_clamp(v, e->maxStamina);
        if (op->remote) continue;
        char name[CHAR_NAME_MAX + 16];
        entity_display_name(e, name, sizeof(name));
        console_log("%s: stamina %d/%d -> %d/%d%s", name, before, e->maxStamina, e->stamina, e->maxStamina,
                    e->stamina <= 0 ? " (exhausted)" : "");
    }
}

/* Logs the result for the person who made the change: "Mao: party Heroes" / "Mao: no party". */
static void apply_party(const Op *op) {
    for (int i = 0; i < op->ids.count; i++) {
        Entity *e = character_at(op, i);
        if (!e) continue;
        snprintf(e->party, sizeof(e->party), "%s", op->party);
        if (op->remote) continue;
        char name[CHAR_NAME_MAX + 16];
        entity_display_name(e, name, sizeof(name));
        if (op->party[0]) console_log("%s: party %s", name, op->party);
        else console_log("%s: no party", name);
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
    case OP_SET_HEALTH:
        apply_health(op);
        break;
    case OP_SET_STAMINA:
        apply_stamina(op);
        break;
    case OP_SET_NAME:
        for (int i = 0; i < op->ids.count; i++) {
            Entity *e = character_at(op, i);
            if (e) snprintf(e->name, sizeof(e->name), "%s", op->charName);
        }
        break;
    case OP_SET_PARTY:
        apply_party(op);
        break;
    case OP_MOVE_ACTION: {
        Entity *e = entity_by_id(op->id);
        if (!e) break;
        bool was_selected = e == selected_entity();
        move_apply(e, (MoveAction)op->action, op->tile, op->remote ? NULL : &gh->path, !op->remote);
        if (op->remote && was_selected) entities_clear_highlights(); /* the range shown belonged to the old tile */
        break;
    }
    case OP_RECOVER_ACTION: {
        Entity *e = entity_by_id(op->id);
        if (!e) break;
        /* the host (or offline) rolls; clients apply the roll the host sent */
        if (!session_is_client()) op->value = recover_roll((RecoverAction)op->action);
        recover_apply(e, (RecoverAction)op->action, op->value);
        break;
    }
    case OP_ATTACK_ACTION: {
        Entity *e = entity_by_id(op->id);
        if (!e) break;
        /* the host (or offline) rolls the damage and resolves the tile; clients apply what the host sent */
        if (!session_is_client()) {
            int rolled[OUTCOME_COUNT], cap = gh->entities.count > 0 ? gh->entities.count : 1;
            attack_roll((AttackAction)op->action, rolled);
            uint32_t *ids = malloc(sizeof(uint32_t) * (size_t)cap);
            int *outcomes = malloc(sizeof(int) * (size_t)cap);
            if (!ids || !outcomes) abort();
            int n = attack_targets(e, op->tile, (AttackDir)op->value, ATTACK_ACTIONS[op->action].value, ids, outcomes, cap);
            da_clear(&op->ids);
            da_clear(&op->values);
            for (int o = 0; o < OUTCOME_COUNT; o++) da_push(&op->values, rolled[o]);
            for (int i = 0; i < n; i++) {
                da_push(&op->ids, ids[i]);
                da_push(&op->values, outcomes[i]);
            }
            free(ids);
            free(outcomes);
        }
        int n = op->ids.count, rolled[OUTCOME_COUNT] = { 0 };
        int *outcomes = malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
        if (!outcomes) abort();
        for (int o = 0; o < OUTCOME_COUNT && o < op->values.count; o++) rolled[o] = (int)op->values.items[o];
        for (int i = 0; i < n; i++)
            outcomes[i] = OUTCOME_COUNT + i < op->values.count ? (int)op->values.items[OUTCOME_COUNT + i] : 0;
        attack_apply(e, (AttackAction)op->action, op->tile, (AttackDir)op->value, rolled, op->ids.items, outcomes, n);
        free(outcomes);
        break;
    }
    case OP_BLOCK_ACTION: {
        Entity *e = entity_by_id(op->id);
        if (e) block_apply(e, (BlockAction)op->action, op->tile, op->value);
        break;
    }
    case OP_DODGE_ACTION: {
        Entity *e = entity_by_id(op->id);
        if (!e) break;
        /* the host (or offline) rolls the Dodge Value's die; clients apply the roll the host sent */
        if (!session_is_client()) op->value = dodge_roll((DodgeAction)op->action);
        bool was_selected = e == selected_entity();
        dodge_apply(e, (DodgeAction)op->action, op->tile, op->value);
        if (was_selected) entities_clear_highlights(); /* the range shown belonged to the old tile */
        break;
    }
    case OP_SET_TEST_MODE:
        if (gh->testMode == (op->value != 0)) break;
        gh->testMode = op->value != 0;
        console_log(gh->testMode ? "Test mode on: actions ignore turns, used actions and stamina"
                                 : "Test mode off");
        break;
    case OP_COUNT:
        break;
    }
}

OpResult ops_submit(Op *op) {
    OpAuthor me = ops_local_author();
    OpResult r = ops_check(op, &me);
    /* A client checks against a table without its own unanswered ops (":c recover 3 spend 3"): the host
     * applies them in order and refuses the spend itself if there really isn't enough stamina. */
    if (r == OPR_NO_STAMINA && session_is_client()) r = OPR_OK;
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
