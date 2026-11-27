/* Attacks (see attacks.h). */
#include "attacks.h"
#include "blocks.h"

#include <stdio.h>
#include <string.h>

/* Only a placeholder so the framework can be tried out; real attacks (weapons) replace it. */
const AttackActionDef ATTACK_ACTIONS[ATTACK_COUNT] = {
    [ATTACK_TEST] = { .id = "attack.test", .name = "Test attack", .slot = SLOT_MAJOR, .stamina = 0, .range = 1,
                      .value = 0, .damage = { [OUTCOME_SUCCESS] = { 1, 4, 0 }, [OUTCOME_SCRAPE] = { 0, 0, 1 },
                                              [OUTCOME_MISS] = { 0, 0, 0 } } },
};

const char *const ATTACK_DIR_NAMES[DIR_COUNT] = { "up", "left", "right", "neutral" };
const char *const ATTACK_OUTCOME_NAMES[OUTCOME_COUNT] = { "success", "scrape", "miss" };

#define MAX_RANGE 60 /* keeps the range's tile set small whatever a row says */

int attack_action_by_id(const char *id) {
    for (int i = 0; id && i < ATTACK_COUNT; i++)
        if (strcmp(ATTACK_ACTIONS[i].id, id) == 0) return i;
    return -1;
}

static bool valid(AttackAction a) { return (int)a >= 0 && a < ATTACK_COUNT; }
static bool valid_dir(int d) { return d >= 0 && d < DIR_COUNT; }

static int range_of(AttackAction a) {
    int r = ATTACK_ACTIONS[a].range;
    return r < 0 ? 0 : r > MAX_RANGE ? MAX_RANGE : r;
}

/* ---- tiles and directions ---- */

Hex attack_origin(const Entity *e, Hex tile) {
    Hex occ[MAX_OCCUPIED];
    int n = entity_occupied_tiles(e, occ);
    Hex best = e->tile;
    int best_d = hex_distance(e->tile, tile);
    for (int i = 0; i < n; i++) {
        int d = hex_distance(occ[i], tile);
        if (d < best_d) best = occ[i], best_d = d;
    }
    return best;
}

int attack_dir_tiles(Hex from, Hex target, AttackDir d, Hex out[6]) {
    if (d == DIR_NEUTRAL) {
        out[0] = target;
        return 1;
    }
    int dist = hex_distance(from, target);
    if (dist == 0 || !valid_dir(d)) return 0;
    /* "as seen from the attacker": on the screen, facing from the attacker to the target */
    Vec2 f = hex_center(from), t = hex_center(target);
    double vx = t.x - f.x, vy = t.y - f.y;
    int n = 0;
    for (int k = 0; k < 6; k++) {
        Hex h = hex_neighbor_raw(target, k);
        int hd = hex_distance(from, h);
        if (d == DIR_UP) {
            if (hd > dist) out[n++] = h;
        } else if (hd == dist) {
            Vec2 c = hex_center(h);
            double cross = vx * (c.y - t.y) - vy * (c.x - t.x); /* < 0: on the left (screen y points down) */
            if ((d == DIR_LEFT) == (cross < 0)) out[n++] = h;
        }
    }
    return n;
}

bool attack_in_range(const Entity *e, AttackAction a, Hex tile) {
    if (!e || !valid(a) || entity_occupies(e, tile)) return false;
    return hex_distance(attack_origin(e, tile), tile) <= range_of(a);
}

void attack_range(const Entity *e, AttackAction a, TileSet *out) {
    int r = valid(a) ? range_of(a) : 0;
    /* occupied tiles are one step from e->tile at most, and a step changes row and column by one at most */
    ts_reset(out, e->tile, r + 1);
    for (int row = e->tile.row - r - 1; row <= e->tile.row + r + 1; row++)
        for (int col = e->tile.col - r - 1; col <= e->tile.col + r + 1; col++) {
            Hex h = { row, col };
            if (attack_in_range(e, a, h)) ts_add(out, h);
        }
}

/* ---- checks ---- */

OpResult attack_check(const Entity *e, AttackAction a) {
    if (!e || e->type != ENTITY_CHARACTER) return OPR_NO_ENTITY;
    if (!valid(a)) return OPR_INVALID;
    OpResult r = action_turn_check(e, ATTACK_ACTIONS[a].slot);
    if (r != OPR_OK) return r;
    /* test mode: stamina never blocks (moves.h); a character without a stamina stat pays nothing */
    if (!gh->testMode && e->maxStamina > 0 && e->stamina < ATTACK_ACTIONS[a].stamina) return OPR_NO_STAMINA;
    return OPR_OK;
}

OpResult attack_check_at(const Entity *e, AttackAction a, Hex tile, int d) {
    OpResult r = attack_check(e, a);
    if (r != OPR_OK) return r;
    if (!valid_dir(d)) return OPR_INVALID;
    return attack_in_range(e, a, tile) ? OPR_OK : OPR_OUT_OF_RANGE;
}

/* ---- resolution ---- */

AttackOutcome attack_block_contest(int dirs, int bv, AttackDir d, int av) {
    bool covered = d == DIR_NEUTRAL || (dirs & DIR_BIT(d));
    if (!covered) return OUTCOME_SUCCESS;
    return bv >= av ? OUTCOME_MISS : OUTCOME_SCRAPE;
}

AttackOutcome attack_dodge_contest(AttackDir dodge, int dv, int et, AttackDir d, int av) {
    bool won = dv >= av + et, same = dodge == d;
    if (won && !same) return OUTCOME_MISS;
    if (!won && same) return OUTCOME_SUCCESS;
    return OUTCOME_SCRAPE;
}

/* The block on tile, if there is one: its directions (DIR_BIT) and Block Value (blocks.h: every block there
 * combined, the highest BV). */
static bool find_block(Hex tile, int *dirs, int *bv) { return block_on_tile(tile, dirs, bv); }

/* target's dodge, if it dodges: its direction, Dodge Value and Evasion Threshold. Dodges don't exist yet. */
static bool find_dodge(const Entity *target, AttackDir *dir, int *dv, int *et) {
    (void)target, (void)dir, (void)dv, (void)et;
    return false;
}

AttackOutcome attack_resolve(const Entity *target, Hex tile, AttackDir d, int av) {
    int dirs, bv, dv, et;
    AttackDir dodge;
    /* how a block and a dodge against the same attack combine is decided when both exist; for now the block wins */
    if (find_block(tile, &dirs, &bv)) return attack_block_contest(dirs, bv, d, av);
    if (find_dodge(target, &dodge, &dv, &et)) return attack_dodge_contest(dodge, dv, et, d, av);
    return OUTCOME_SUCCESS; /* not interfered with */
}

/* ---- damage ---- */

static int clamp_damage(long long v) { return v < 0 ? 0 : v > ATTACK_MAX_DAMAGE ? ATTACK_MAX_DAMAGE : (int)v; }
static bool has_dice(Damage dmg) { return dmg.num > 0 && dmg.sides > 0; }

int damage_min(Damage dmg) { return clamp_damage((has_dice(dmg) ? dmg.num : 0) + (long long)dmg.modifier); }
int damage_max(Damage dmg) {
    return clamp_damage((has_dice(dmg) ? (long long)dmg.num * dmg.sides : 0) + dmg.modifier);
}

static int damage_roll(Damage dmg) {
    if (!has_dice(dmg)) return clamp_damage(dmg.modifier);
    int dice[ATTACK_MAX_DICE];
    int num = dmg.num > ATTACK_MAX_DICE ? ATTACK_MAX_DICE : dmg.num;
    return clamp_damage(dice_roll(num, dmg.sides, dmg.modifier, dice));
}

void attack_roll(AttackAction a, int rolled[OUTCOME_COUNT]) {
    for (int o = 0; o < OUTCOME_COUNT; o++) rolled[o] = valid(a) ? damage_roll(ATTACK_ACTIONS[a].damage[o]) : 0;
}

bool attack_rolls_ok(AttackAction a, const int rolled[OUTCOME_COUNT]) {
    if (!valid(a)) return false;
    for (int o = 0; o < OUTCOME_COUNT; o++) {
        Damage dmg = ATTACK_ACTIONS[a].damage[o];
        if (rolled[o] < damage_min(dmg) || rolled[o] > damage_max(dmg)) return false;
    }
    return true;
}

/* ---- using it ---- */

int attack_targets(Hex tile, AttackDir d, int av, uint32_t *ids, int *outcomes, int cap) {
    int n = 0;
    for (int i = 0; i < gh->entities.count && n < cap; i++) {
        const Entity *t = gh->entities.items[i];
        if (t->type != ENTITY_CHARACTER || !entity_occupies(t, tile)) continue;
        ids[n] = t->id;
        outcomes[n] = attack_resolve(t, tile, d, av);
        n++;
    }
    return n;
}

void attack_apply(Entity *e, AttackAction a, Hex tile, AttackDir d, const int rolled[OUTCOME_COUNT],
                  const uint32_t *ids, const int *outcomes, int n) {
    const AttackActionDef *def = &ATTACK_ACTIONS[a];
    action_use_slot(e, def->slot);
    int before;
    bool spent = action_spend_stamina(e, def->stamina, &before);

    if (gh->init.count == 0) attack_marks_clear_of(e->id); /* no rounds: the newest attack replaces the old */
    AttackMark m = { .tile = tile, .from = attack_origin(e, tile), .dir = d, .attacker = e->id };
    da_push(&gh->attackMarks, m);

    char name[CHAR_NAME_MAX + 16];
    entity_display_name(e, name, sizeof(name));
    char stamina[48] = "";
    if (spent) snprintf(stamina, sizeof(stamina), ", stamina %d/%d", e->stamina, e->maxStamina);
    int dirs, bv;
    char blocked[64] = "";
    bool block = block_on_tile(tile, &dirs, &bv);
    if (block) {
        AttackOutcome bx = attack_block_contest(dirs, bv, d, def->value);
        snprintf(blocked, sizeof(blocked), ", AV %d against a block of BV %d%s", def->value, bv,
                 bx == OUTCOME_SUCCESS ? " in other directions" : "");
        if (bx == OUTCOME_SUCCESS) block_failed(tile); /* blockers off the tile can't block it again next turn */
    }
    console_log("%s: %s on [%d|%d] %s%s%s%s", name, def->name, tile.row, tile.col, ATTACK_DIR_NAMES[d], blocked,
                stamina, n ? "" : ", nobody there");

    /* attacks apply to the tile: every character on it, with the outcome the host decided */
    for (int i = 0; i < n; i++) {
        Entity *t = entity_by_id(ids[i]);
        if (!t || t->type != ENTITY_CHARACTER || outcomes[i] < 0 || outcomes[i] >= OUTCOME_COUNT) continue;
        AttackOutcome o = (AttackOutcome)outcomes[i];
        char tname[CHAR_NAME_MAX + 16];
        entity_display_name(t, tname, sizeof(tname));
        if (t->maxHealth <= 0) { /* no HP stat: nothing to lose */
            console_log("  %s: %s, %d damage (no HP)", tname, ATTACK_OUTCOME_NAMES[o], rolled[o]);
            continue;
        }
        t->health = hp_clamp((long long)t->health - rolled[o], t->maxHealth);
        console_log("  %s: %s, %d damage, HP %d/%d (%s)", tname, ATTACK_OUTCOME_NAMES[o], rolled[o], t->health,
                    t->maxHealth, hp_stage_name(hp_stage(t->health, t->maxHealth)));
    }
    action_end_if_exhausted(e, before, spent);
}

/* ---- tile marks ---- */

void attack_marks_clear_of(uint32_t attacker) {
    for (int i = gh->attackMarks.count - 1; i >= 0; i--)
        if (gh->attackMarks.items[i].attacker == attacker) da_remove_at(&gh->attackMarks, i);
}

void attack_marks_clear(void) { da_clear(&gh->attackMarks); }
