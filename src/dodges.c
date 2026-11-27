/* Dodges (see dodges.h). */
#include "dodges.h"
#include "attrs.h"

#include <stdio.h>
#include <string.h>

/* Notion, Personal Actions: "Dodge" (Major, 2 stamina): move to an adjacent tile; DV = 1d8 + AGI. */
const DodgeActionDef DODGE_ACTIONS[DODGE_COUNT] = {
    [DODGE_BASIC] = { "dodge", "Dodge", SLOT_MAJOR, 2, 1, 8, ATTR_AGI },
};

int dodge_action_by_id(const char *id) {
    for (int i = 0; id && i < DODGE_COUNT; i++)
        if (strcmp(DODGE_ACTIONS[i].id, id) == 0) return i;
    return -1;
}

static bool valid(DodgeAction a) { return (int)a >= 0 && a < DODGE_COUNT; }

/* ---- tiles ---- */

void dodge_range(const Entity *e, DodgeAction a, TileSet *out) {
    ts_reset(out, e->tile, 2);
    if (!valid(a)) return;
    TileSet fits = { 0 };
    astar_range(e->tile, 1, (Entity *)e, &fits); /* the neighbours it fits on (and its own tile) */
    for (int k = 0; k < 6; k++) {
        Hex n = hex_neighbor_raw(e->tile, k);
        if (ts_has(&fits, n)) ts_add(out, n);
    }
    ts_free(&fits);
}

bool dodge_in_range(const Entity *e, DodgeAction a, Hex tile) {
    if (!e || !valid(a) || hex_distance(e->tile, tile) != 1) return false;
    TileSet range = { 0 };
    dodge_range(e, a, &range);
    bool ok = ts_has(&range, tile);
    ts_free(&range);
    return ok;
}

/* ---- checks ---- */

OpResult dodge_check(const Entity *e, DodgeAction a) {
    if (!e || e->type != ENTITY_CHARACTER) return OPR_NO_ENTITY;
    if (!valid(a)) return OPR_INVALID;
    OpResult r = action_turn_check(e, DODGE_ACTIONS[a].slot);
    if (r != OPR_OK) return r;
    /* test mode: stamina never blocks (moves.h); a character without a stamina stat pays nothing */
    if (!gh->testMode && e->maxStamina > 0 && e->stamina < DODGE_ACTIONS[a].stamina) return OPR_NO_STAMINA;
    return OPR_OK;
}

OpResult dodge_check_to(const Entity *e, DodgeAction a, Hex tile) {
    OpResult r = dodge_check(e, a);
    if (r != OPR_OK) return r;
    return dodge_in_range(e, a, tile) ? OPR_OK : OPR_OUT_OF_RANGE;
}

/* ---- the Dodge Value ---- */

int dodge_roll(DodgeAction a) {
    if (!valid(a)) return 0;
    int dice[16];
    int n = DODGE_ACTIONS[a].dice > 16 ? 16 : DODGE_ACTIONS[a].dice;
    return dice_roll(n, DODGE_ACTIONS[a].sides, 0, dice);
}

bool dodge_roll_ok(DodgeAction a, int roll) {
    return valid(a) && roll >= DODGE_ACTIONS[a].dice && roll <= DODGE_ACTIONS[a].dice * DODGE_ACTIONS[a].sides;
}

int dodge_value(const Entity *e, DodgeAction a, int roll) {
    return roll + (valid(a) && e ? e->attr[DODGE_ACTIONS[a].attr] : 0);
}

/* ---- using it ---- */

static int step_of(Hex from, Hex to) {
    for (int k = 0; k < 6; k++)
        if (hex_eq(hex_neighbor_raw(from, k), to)) return k;
    return 0; /* dodge_check_to made sure it is a neighbour */
}

void dodge_apply(Entity *e, DodgeAction a, Hex tile, int roll) {
    const DodgeActionDef *def = &DODGE_ACTIONS[a];
    action_use_slot(e, def->slot);
    e->turnUsed |= TURN_MOVED; /* a dodge is a move (Sprint) */
    if (gh->init.count == 0) dodges_clear_of(e->id); /* no rounds: the newest dodge replaces the old */
    DodgeRec d = { .dodger = e->id, .step = step_of(e->tile, tile), .value = dodge_value(e, a, roll) };
    d.nfrom = entity_occupied_tiles(e, d.from);
    Hex was = e->tile;
    e->tile = tile;
    da_push(&gh->dodges, d);
    int before;
    bool spent = action_spend_stamina(e, def->stamina, &before);

    char name[CHAR_NAME_MAX + 16], stamina[48] = "";
    entity_display_name(e, name, sizeof(name));
    if (spent) snprintf(stamina, sizeof(stamina), ", stamina %d/%d", e->stamina, e->maxStamina);
    console_log("%s: %s from [%d|%d] to [%d|%d], DV %d (%dd%d: %d, %s %d)%s", name, def->name, was.row, was.col,
                tile.row, tile.col, d.value, def->dice, def->sides, roll, ATTR_NAMES[def->attr], e->attr[def->attr],
                stamina);
    action_end_if_exhausted(e, before, spent);
}

const DodgeRec *dodge_find(const Entity *target, Hex tile) {
    for (int i = gh->dodges.count - 1; i >= 0 && target; i--) { /* the latest first */
        const DodgeRec *d = &gh->dodges.items[i];
        if (d->dodger != target->id) continue;
        for (int k = 0; k < d->nfrom; k++)
            if (hex_eq(d->from[k], tile)) return d;
    }
    return NULL;
}

int dodge_dir(Hex from, Hex tile, int step) {
    Hex to = hex_neighbor_raw(tile, step); /* the step, taken from the attacked tile */
    static const AttackDir dirs[] = { DIR_UP, DIR_LEFT, DIR_RIGHT };
    for (int i = 0; i < 3; i++) {
        Hex out[6];
        int n = attack_dir_tiles(from, tile, dirs[i], out);
        for (int k = 0; k < n; k++)
            if (hex_eq(out[k], to)) return dirs[i];
    }
    return DIR_COUNT; /* toward the attacker */
}

/* ---- lifetime ---- */

void dodges_turn_started(uint32_t id) { dodges_clear_of(id); }

void dodges_clear_of(uint32_t id) {
    for (int i = gh->dodges.count - 1; i >= 0; i--)
        if (gh->dodges.items[i].dodger == id) da_remove_at(&gh->dodges, i);
}

void dodges_clear(void) { da_clear(&gh->dodges); }
