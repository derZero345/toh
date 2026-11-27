/*
 * Attacks of Tales of Honor (rulebook: Base Action Types -> Attacks, Block, Dodge). Design:
 * docs/superpowers/specs/2026-11-27-attacks-design.md.
 *
 * An attack targets a tile, not a character: a tile within the attack's range, in one of four directions (up, left,
 * right, neutral). It succeeds, scrapes or misses for every character on the tile, who takes the damage of that
 * outcome. It resolves at once, when it is used. Only the framework is built: the table holds one placeholder row,
 * blocks and dodges don't exist yet (attack_resolve's hooks find none, so every attack succeeds), and the contests
 * they will use are ready below.
 *
 * Directions, as seen from the attacker (attack_dir_tiles):
 *   neutral      the target tile itself
 *   up           the target's neighbours farther from the attacker
 *   left, right  the target's neighbour at the same distance from the attacker, on the attacker's left / right (for
 *                an adjacent target: the neighbour that also borders the attacker)
 *
 * Attacks follow the turn rules of moves.h (initiative, the character's turn, a free slot; a Minor attack can use the
 * Major slot) and cost stamina; using up the last stamina ends the turn. Test mode lifts initiative, the turn, the
 * slot and stamina; the range, ownership and the direction still hold.
 *
 * Every attack leaves a mark on its tile (gh->attackMarks, drawn by graphics.c) until the attacker's next turn starts,
 * the initiative ends or the attacker is deleted; outside initiative (test mode) the attacker's next attack replaces
 * its marks. Marks are not saved.
 *
 * The host rolls the damage of every outcome (ops_apply when not a client) and sends the rolls in the op, so everyone
 * applies the same damage; the outcome itself follows from the shared table state.
 *
 * Add an attack: an AttackAction value (before ATTACK_COUNT) and its row in ATTACK_ACTIONS; the radial menu entry
 * with the same id goes into radial_config.c (an "Attack" submenu of its slot) and its rule into radial_game.c.
 * Blocks and dodges: fill in find_block / find_dodge in attacks.c; attack_resolve then uses the contests.
 */
#ifndef TOH_ATTACKS_H
#define TOH_ATTACKS_H

#include "moves.h"

typedef enum { ATTACK_TEST, ATTACK_COUNT } AttackAction;
typedef enum { DIR_UP, DIR_LEFT, DIR_RIGHT, DIR_NEUTRAL, DIR_COUNT } AttackDir;
typedef enum { OUTCOME_SUCCESS, OUTCOME_SCRAPE, OUTCOME_MISS, OUTCOME_COUNT } AttackOutcome;

#define ATTACK_MAX_DICE 20     /* dice in one damage roll */
#define ATTACK_MAX_DAMAGE 1000 /* one roll's damage at most (the wire checks it too) */
/* Block directions: a bit per AttackDir. A block always covers neutral as well. */
#define DIR_BIT(d) (1 << (d))

/* num d sides + modifier; sides 0 = only the modifier. Never below 0. */
typedef struct { int num, sides, modifier; } Damage;

typedef struct {
    const char *id; /* also the radial menu entry's id */
    const char *name;
    ActionSlot slot;
    int stamina;
    int range; /* in tiles, straight-line (hex distance) */
    int value; /* the Attack Value (AV) */
    Damage damage[OUTCOME_COUNT];
} AttackActionDef;

extern const AttackActionDef ATTACK_ACTIONS[ATTACK_COUNT];
extern const char *const ATTACK_DIR_NAMES[DIR_COUNT];         /* "up", "left", "right", "neutral" */
extern const char *const ATTACK_OUTCOME_NAMES[OUTCOME_COUNT]; /* "success", "scrape", "miss" */

int attack_action_by_id(const char *id); /* -1 if none */

/* ---- tiles and directions ---- */

/* The attacker's tile nearest to tile (multi-tile characters attack from the tile closest to the target). */
Hex attack_origin(const Entity *e, Hex tile);
/* The tiles direction d of an attack from `from` on `target` points at (see above); returns how many (at most 6). */
int attack_dir_tiles(Hex from, Hex target, AttackDir d, Hex out[6]);
/* The tiles e can attack with the action: within its range of e's tiles, not e's own tiles. */
void attack_range(const Entity *e, AttackAction a, TileSet *out);
bool attack_in_range(const Entity *e, AttackAction a, Hex tile);

/* ---- checks ---- */

/* May e use the action now (before choosing where)? OPR_OK or the reason. */
OpResult attack_check(const Entity *e, AttackAction a);
/* ...on tile in direction d: attack_check plus the tile in range and a valid direction. */
OpResult attack_check_at(const Entity *e, AttackAction a, Hex tile, int d);

/* ---- resolution ---- */

/* The block contest (BX): a block on the tile in the directions `dirs` (DIR_BIT; neutral is always covered) with
 * Block Value bv against an attack in direction d with Attack Value av. Right direction: miss if bv >= av, else
 * scrape. Wrong direction: success. */
AttackOutcome attack_block_contest(int dirs, int bv, AttackDir d, int av);
/* The dodge contest (DX): a dodge in direction dodge with Dodge Value dv by a character with Evasion Threshold et
 * against an attack in direction d with Attack Value av; won if dv >= av + et. Won and another direction: miss; lost
 * and another direction or won and the same direction: scrape; lost and the same direction: success. */
AttackOutcome attack_dodge_contest(AttackDir dodge, int dv, int et, AttackDir d, int av);
/* The outcome of an attack (direction d, Attack Value av) on tile for the character target there. Asks for a block on
 * the tile and a dodge by target; there are none yet, so it is a success. */
AttackOutcome attack_resolve(const Entity *target, Hex tile, AttackDir d, int av);

/* ---- damage ---- */

int damage_min(Damage dmg);
int damage_max(Damage dmg);
/* The host's rolls for the action: one damage roll per outcome. */
void attack_roll(AttackAction a, int rolled[OUTCOME_COUNT]);
/* Could the rolls come from the action's damage dice? */
bool attack_rolls_ok(AttackAction a, const int rolled[OUTCOME_COUNT]);

/* ---- using it ---- */

/* Uses the slot, spends the stamina, marks the tile and resolves the attack for every character on it, who loses
 * rolled[its outcome] HP; logs what happened. Exhausted: the turn ends. Requires attack_check_at == OPR_OK. */
void attack_apply(Entity *e, AttackAction a, Hex tile, AttackDir d, const int rolled[OUTCOME_COUNT]);

/* ---- tile marks ---- */

void attack_marks_clear_of(uint32_t attacker); /* its turn starts, it was deleted, or it attacks outside initiative */
void attack_marks_clear(void);                 /* the initiative ends, a table is loaded */

#endif
