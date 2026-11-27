/* The phases of a combat round (see phases.h). */
#include "phases.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool phases_declaring(void) { return gh->init.count > 0; }

/* ---- where a character is during the round ---- */

static bool covers(const Entity *e, Hex center, Hex tile) {
    Hex tiles[MAX_OCCUPIED];
    int n = character_occupied_tiles(center, e->size, tiles);
    for (int i = 0; i < n; i++)
        if (hex_eq(tiles[i], tile)) return true;
    return false;
}

int phases_presence(const Entity *e, Hex tile) {
    if (!e || e->type != ENTITY_CHARACTER) return PRESENCE_NONE;
    if (entity_occupies(e, tile)) return PRESENCE_STOP; /* where it stays, or where its moves end */
    bool passed = false;
    for (int i = 0; i < gh->declared.count; i++) {
        const Declared *d = &gh->declared.items[i];
        if (d->actor != e->id || (d->type != OP_MOVE_ACTION && d->type != OP_DODGE_ACTION)) continue;
        if (covers(e, d->start, tile) || covers(e, d->tile, tile)) return PRESENCE_STOP;
        for (int k = 1; k + 1 < d->path.count && !passed; k++) passed = covers(e, d->path.items[k], tile);
    }
    return passed ? PRESENCE_PASSED : PRESENCE_NONE;
}

bool phases_shown_on(const Entity *e, Hex h) {
    if (!e) return false;
    return e->hasShown ? covers(e, e->shown, h) : entity_occupies(e, h);
}

bool phases_has_declared(uint32_t id) {
    for (int i = 0; i < gh->declared.count; i++)
        if (gh->declared.items[i].actor == id && !gh->declared.items[i].resolved) return true;
    return false;
}

/* ---- declaring ---- */

static Declared base(const Entity *e, int type, int action, Hex tile, int value) {
    return (Declared){ .actor = e->id, .type = type, .action = action, .value = value, .tile = tile,
                       .start = e->tile, .origin = e->tile, .turnUsed = e->turnUsed, .stamina = e->stamina };
}

/* the token stays where it stood until its moves resolve */
static void keep_shown(Entity *e) {
    if (e->hasShown) return;
    e->shown = e->tile;
    e->hasShown = true;
}

static void stamina_note(const Entity *e, bool spent, char *buf, size_t n) {
    buf[0] = '\0';
    if (spent) snprintf(buf, n, ", stamina %d/%d", e->stamina, e->maxStamina);
}

void phases_declare_move(Entity *e, MoveAction a, Hex tile) {
    const MoveActionDef *def = &MOVE_ACTIONS[a];
    Declared d = base(e, OP_MOVE_ACTION, a, tile, 0);
    action_use_slot(e, def->slot);
    e->turnUsed |= TURN_MOVED;
    if (!astar_path_within(e->tile, tile, move_distance(e, a), e, &d.path) || d.path.count < 2) {
        da_clear(&d.path);
        da_push(&d.path, e->tile);
        da_push(&d.path, tile);
    }
    keep_shown(e);
    e->tile = tile;
    int before;
    bool spent = action_spend_stamina(e, def->stamina, &before);
    da_push(&gh->declared, d);
    char name[CHAR_NAME_MAX + 16], st[48];
    entity_display_name(e, name, sizeof(name));
    stamina_note(e, spent, st, sizeof(st));
    console_log("%s declares %s to [%d|%d]%s", name, def->name, tile.row, tile.col, st);
    action_end_if_exhausted(e, before, spent);
}

void phases_declare_recover(Entity *e, RecoverAction a) {
    Declared d = base(e, OP_RECOVER_ACTION, a, e->tile, 0);
    action_use_slot(e, RECOVER_ACTIONS[a].slot);
    da_push(&gh->declared, d);
    char name[CHAR_NAME_MAX + 16];
    entity_display_name(e, name, sizeof(name));
    console_log("%s declares %s", name, RECOVER_ACTIONS[a].name);
}

void phases_declare_attack(Entity *e, AttackAction a, Hex tile, AttackDir dir) {
    Declared d = base(e, OP_ATTACK_ACTION, a, tile, dir);
    d.origin = attack_origin(e, tile); /* where it attacks from at this point of its turn */
    int before;
    bool spent;
    attack_declare(e, a, tile, dir, true, &before, &spent);
    da_push(&gh->declared, d);
    char name[CHAR_NAME_MAX + 16], st[48];
    entity_display_name(e, name, sizeof(name));
    stamina_note(e, spent, st, sizeof(st));
    console_log("%s declares %s on [%d|%d] %s%s", name, ATTACK_ACTIONS[a].name, tile.row, tile.col,
                ATTACK_DIR_NAMES[dir], st);
    action_end_if_exhausted(e, before, spent);
}

void phases_declare_block(Entity *e, BlockAction a, Hex tile, int dirs) {
    Declared d = base(e, OP_BLOCK_ACTION, a, tile, dirs);
    int had = gh->blocks.count;
    /* it takes effect now, for the whole round; block_apply logs it (and may end the turn: it is pushed first) */
    da_push(&gh->declared, d);
    int idx = gh->declared.count - 1;
    block_apply(e, a, tile, dirs);
    gh->declared.items[idx].nblocks = gh->blocks.count - had;
}

void phases_declare_dodge(Entity *e, DodgeAction a, Hex tile, int roll) {
    Declared d = base(e, OP_DODGE_ACTION, a, tile, roll);
    da_push(&d.path, e->tile);
    da_push(&d.path, tile);
    keep_shown(e);
    da_push(&gh->declared, d);
    dodge_apply(e, a, tile, roll); /* the dodge and its roll count for the whole round; logs it */
}

/* ---- taking back ---- */

static int last_of(uint32_t id) {
    for (int i = gh->declared.count - 1; i >= 0; i--)
        if (gh->declared.items[i].actor == id && !gh->declared.items[i].resolved) return i;
    return -1;
}

OpResult phases_take_back_check(const Entity *e) {
    if (!e || e->type != ENTITY_CHARACTER) return OPR_NO_ENTITY;
    if (!phases_declaring()) return OPR_NOT_IN_COMBAT;
    if (gh->phase != PHASE_DECLARE) return OPR_RESOLVING;
    if (!gh->testMode && gh->currentTurn != e) return OPR_NOT_YOUR_TURN;
    return last_of(e->id) >= 0 ? OPR_OK : OPR_NOTHING_DECLARED;
}

static void remove_last_mark(uint32_t id) {
    for (int i = gh->attackMarks.count - 1; i >= 0; i--)
        if (gh->attackMarks.items[i].attacker == id) {
            da_remove_at(&gh->attackMarks, i);
            return;
        }
}

static void remove_last_blocks(uint32_t id, int n) {
    for (int i = gh->blocks.count - 1; i >= 0 && n > 0; i--)
        if (gh->blocks.items[i].blocker == id) {
            da_remove_at(&gh->blocks, i);
            n--;
        }
}

static void remove_last_dodge(uint32_t id) {
    for (int i = gh->dodges.count - 1; i >= 0; i--)
        if (gh->dodges.items[i].dodger == id) {
            da_remove_at(&gh->dodges, i);
            return;
        }
}

static bool moves_left(uint32_t id) {
    for (int i = 0; i < gh->declared.count; i++) {
        const Declared *d = &gh->declared.items[i];
        if (d->actor == id && !d->resolved && (d->type == OP_MOVE_ACTION || d->type == OP_DODGE_ACTION)) return true;
    }
    return false;
}

static const char *action_name(int type, int action) {
    switch (type) {
    case OP_MOVE_ACTION:    return MOVE_ACTIONS[action].name;
    case OP_RECOVER_ACTION: return RECOVER_ACTIONS[action].name;
    case OP_ATTACK_ACTION:  return ATTACK_ACTIONS[action].name;
    case OP_BLOCK_ACTION:   return BLOCK_ACTIONS[action].name;
    case OP_DODGE_ACTION:   return DODGE_ACTIONS[action].name;
    default:                return "?";
    }
}

bool phases_take_back_state(const Entity *e, Hex *tile, int *stamina, int *turnUsed) {
    int i = e ? last_of(e->id) : -1;
    if (i < 0) return false;
    *tile = gh->declared.items[i].start;
    *stamina = gh->declared.items[i].stamina;
    *turnUsed = gh->declared.items[i].turnUsed;
    return true;
}

void phases_take_back(Entity *e) {
    int i = last_of(e->id);
    if (i < 0) return;
    Declared d = gh->declared.items[i];
    da_remove_at(&gh->declared, i);
    e->turnUsed = d.turnUsed;
    e->stamina = d.stamina;
    switch (d.type) {
    case OP_MOVE_ACTION:
        e->tile = d.start;
        break;
    case OP_DODGE_ACTION:
        e->tile = d.start;
        remove_last_dodge(e->id);
        break;
    case OP_ATTACK_ACTION:
        remove_last_mark(e->id);
        break;
    case OP_BLOCK_ACTION:
        remove_last_blocks(e->id, d.nblocks);
        break;
    default:
        break;
    }
    if (!moves_left(e->id)) e->hasShown = false;
    da_free(&d.path);
    char name[CHAR_NAME_MAX + 16];
    entity_display_name(e, name, sizeof(name));
    console_log("%s takes back %s", name, action_name(d.type, d.action));
}

/* ---- resolving ---- */

void phases_results(uint32_t actor, DoubleList *out) {
    da_clear(out);
    int cap = gh->entities.count > 0 ? gh->entities.count : 1;
    uint32_t *ids = malloc(sizeof(uint32_t) * (size_t)cap);
    int *outcomes = malloc(sizeof(int) * (size_t)cap);
    if (!ids || !outcomes) abort();
    for (int i = 0; i < gh->declared.count; i++) {
        const Declared *d = &gh->declared.items[i];
        if (d->actor != actor || d->resolved) continue;
        switch (d->type) {
        case OP_MOVE_ACTION:
        case OP_DODGE_ACTION:
            da_push(out, d->type);
            da_push(out, d->action);
            da_push(out, d->tile.row);
            da_push(out, d->tile.col);
            break;
        case OP_RECOVER_ACTION:
            da_push(out, d->type);
            da_push(out, d->action);
            da_push(out, recover_roll((RecoverAction)d->action));
            break;
        case OP_ATTACK_ACTION: {
            int rolled[OUTCOME_COUNT];
            attack_roll((AttackAction)d->action, rolled);
            int n = attack_targets(d->origin, d->tile, (AttackDir)d->value, ATTACK_ACTIONS[d->action].value, ids,
                                   outcomes, cap);
            double head[] = { d->type, d->action, d->tile.row, d->tile.col, d->value, d->origin.row, d->origin.col,
                              rolled[0], rolled[1], rolled[2], n };
            for (size_t k = 0; k < sizeof(head) / sizeof(head[0]); k++) da_push(out, head[k]);
            for (int k = 0; k < n; k++) {
                da_push(out, ids[k]);
                da_push(out, outcomes[k]);
            }
            break;
        }
        default: /* blocks: nothing to resolve */
            break;
        }
    }
    free(ids);
    free(outcomes);
}

static bool whole(double v, double lo, double hi) { return isfinite(v) && v == floor(v) && v >= lo && v <= hi; }

/* The length of the record at v[0..left), or 0 if it is malformed. */
static int record_len(const double *v, int left) {
    if (left < 2 || !isfinite(v[0])) return 0;
    switch ((int)v[0]) {
    case OP_MOVE_ACTION:
        return left >= 4 && v[0] == OP_MOVE_ACTION && whole(v[1], 0, MOVE_COUNT - 1) &&
                       whole(v[2], -1e6, 1e6) && whole(v[3], -1e6, 1e6) ? 4 : 0;
    case OP_DODGE_ACTION:
        return left >= 4 && v[0] == OP_DODGE_ACTION && whole(v[1], 0, DODGE_COUNT - 1) &&
                       whole(v[2], -1e6, 1e6) && whole(v[3], -1e6, 1e6) ? 4 : 0;
    case OP_RECOVER_ACTION:
        return left >= 3 && v[0] == OP_RECOVER_ACTION && whole(v[1], 0, RECOVER_COUNT - 1) &&
                       whole(v[2], 0, RECOVER_DIE) ? 3 : 0;
    case OP_ATTACK_ACTION: {
        if (left < 11 || v[0] != OP_ATTACK_ACTION || !whole(v[1], 0, ATTACK_COUNT - 1)) return 0;
        for (int k = 2; k <= 6; k++)
            if (!whole(v[k], -1e6, 1e6)) return 0;
        if (!whole(v[4], 0, DIR_COUNT - 1)) return 0;
        int rolled[OUTCOME_COUNT];
        for (int o = 0; o < OUTCOME_COUNT; o++) {
            if (!whole(v[7 + o], 0, ATTACK_MAX_DAMAGE)) return 0;
            rolled[o] = (int)v[7 + o];
        }
        if (!attack_rolls_ok((AttackAction)(int)v[1], rolled)) return 0;
        if (!whole(v[10], 0, (double)((left - 11) / 2))) return 0;
        int n = (int)v[10];
        for (int k = 0; k < n; k++)
            if (!whole(v[11 + 2 * k], 1, 4294967295.0) || !whole(v[12 + 2 * k], 0, OUTCOME_COUNT - 1)) return 0;
        return 11 + 2 * n;
    }
    default:
        return 0;
    }
}

bool phases_results_ok(const double *v, int n) {
    if (n < 0 || n > PHASES_MAX_RESULTS) return false;
    for (int i = 0; i < n;) {
        int len = record_len(v + i, n - i);
        if (len == 0) return false;
        i += len;
    }
    return true;
}

void phases_apply_results(uint32_t actor, const double *v, int n) {
    Entity *e = entity_by_id(actor);
    char name[CHAR_NAME_MAX + 16] = "";
    if (e) entity_display_name(e, name, sizeof(name));
    for (int i = 0; i < n;) {
        int len = record_len(v + i, n - i);
        if (len == 0) break;
        const double *r = v + i;
        i += len;
        if (!e) continue;
        int type = (int)r[0], action = (int)r[1];
        switch (type) {
        case OP_MOVE_ACTION:
        case OP_DODGE_ACTION:
            console_log("%s: %s to [%d|%d]", name, action_name(type, action), (int)r[2], (int)r[3]);
            break;
        case OP_RECOVER_ACTION:
            recover_gain(e, (RecoverAction)action, (int)r[2]);
            break;
        case OP_ATTACK_ACTION: {
            int rolled[OUTCOME_COUNT] = { (int)r[7], (int)r[8], (int)r[9] }, cnt = (int)r[10];
            uint32_t *ids = malloc(sizeof(uint32_t) * (size_t)(cnt ? cnt : 1));
            int *outs = malloc(sizeof(int) * (size_t)(cnt ? cnt : 1));
            if (!ids || !outs) abort();
            for (int k = 0; k < cnt; k++) {
                ids[k] = (uint32_t)r[11 + 2 * k];
                outs[k] = (int)r[12 + 2 * k];
            }
            Hex tile = { (int)r[2], (int)r[3] }, from = { (int)r[5], (int)r[6] };
            attack_hit(e, (AttackAction)action, from, tile, (AttackDir)(int)r[4], rolled, ids, outs, cnt, NULL);
            free(ids);
            free(outs);
            break;
        }
        default:
            break;
        }
    }
    /* what this player knew of the declarations is resolved; the token goes where the character stands */
    for (int k = 0; k < gh->declared.count; k++)
        if (gh->declared.items[k].actor == actor) gh->declared.items[k].resolved = true;
    for (int k = 0; k < gh->attackMarks.count; k++)
        if (gh->attackMarks.items[k].attacker == actor) gh->attackMarks.items[k].pending = false;
    if (e) e->hasShown = false;
}

/* ---- the round ---- */

static void clear_declared(void) {
    for (int i = 0; i < gh->declared.count; i++) da_free(&gh->declared.items[i].path);
    da_clear(&gh->declared);
    for (int i = 0; i < gh->entities.count; i++) gh->entities.items[i]->hasShown = false;
}

void phases_round_start(void) {
    clear_declared();
    attack_marks_clear();
    for (int i = 0; i < gh->entities.count; i++) blocks_turn_started(gh->entities.items[i]->id); /* and the locks */
    dodges_clear();
}

void phases_clear(void) {
    clear_declared();
    gh->phase = PHASE_DECLARE;
    gh->resolveDone = false;
}

void phases_clear_of(uint32_t id) {
    for (int i = gh->declared.count - 1; i >= 0; i--)
        if (gh->declared.items[i].actor == id) {
            da_free(&gh->declared.items[i].path);
            da_remove_at(&gh->declared, i);
        }
}

bool phases_label(char *buf, size_t n) {
    if (gh->init.count == 0 || !gh->currentTurn) return false;
    if (gh->phase == PHASE_RESOLVE && gh->resolveDone) {
        snprintf(buf, n, "RESOLUTION · done");
        return true;
    }
    char name[CHAR_NAME_MAX + 16];
    entity_display_name(gh->currentTurn, name, sizeof(name));
    snprintf(buf, n, "%s · %s", gh->phase == PHASE_RESOLVE ? "RESOLUTION" : "DECLARATION", name);
    return true;
}
