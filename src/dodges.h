/*
 * Dodges of Tales of Honor (rulebook: Base Action Types -> Dodge; Notion: Personal Actions with type Dodge). Design:
 * docs/superpowers/specs/2026-11-27-dodges-design.md.
 *
 * A dodge moves the character to an adjacent tile it fits on, and rolls its Dodge Value (the host rolls, the op
 * carries the die). "You always dodge on the original tile": until its next turn starts, an attack on a tile it left
 * meets the dodge contest (attacks.h) there. The dodge's direction is its first step as seen from that attacker
 * (dodge_dir): away from it = up, the same-distance neighbour on its left / right = left / right; a step toward the
 * attacker matches no attack direction. Attacks on the tile it moved to hit it normally. With a block on the attacked
 * tile as well, the better result for the defender counts (attacks.c).
 *
 * The contest needs the dodger's Evasion Threshold: Entity.evasion (EVASION_DEFAULT for new characters, :c et).
 *
 * Dodges stay until the dodger's next turn starts, the initiative ends or it is deleted; outside initiative (test
 * mode) its next dodge replaces them. Not saved. A dodge counts as a move (TURN_MOVED: Sprint).
 *
 * Dodges follow the turn rules of moves.h and cost stamina. Test mode lifts initiative, the turn, the slot and
 * stamina; not ownership, adjacency or the room on the tile.
 *
 * Only Dodge is here: the ability dodges (Spot Dodge, Leap) wait for abilities, and Leap for levels, falling and
 * prone. Add a dodge: a DodgeAction value (before DODGE_COUNT) and its row in DODGE_ACTIONS; the radial menu entry
 * with the same id goes into radial_config.c and its rule into radial_game.c.
 */
#ifndef TOH_DODGES_H
#define TOH_DODGES_H

#include "attacks.h"

typedef enum { DODGE_BASIC, DODGE_COUNT } DodgeAction;

typedef struct {
    const char *id; /* also the radial menu entry's id */
    const char *name;
    ActionSlot slot;
    int stamina;
    int dice, sides; /* the Dodge Value: dice d sides + the attribute */
    int attr;        /* AttrIndex (attrs.h) */
} DodgeActionDef;

extern const DodgeActionDef DODGE_ACTIONS[DODGE_COUNT];

int dodge_action_by_id(const char *id); /* -1 if none */

/* ---- tiles ---- */

/* The tiles e can dodge to: adjacent to its tile, where it fits. */
void dodge_range(const Entity *e, DodgeAction a, TileSet *out);
bool dodge_in_range(const Entity *e, DodgeAction a, Hex tile);

/* ---- checks ---- */

/* May e use the action now (before choosing where)? OPR_OK or the reason. */
OpResult dodge_check(const Entity *e, DodgeAction a);
/* ...to tile: plus the tile in range. */
OpResult dodge_check_to(const Entity *e, DodgeAction a, Hex tile);

/* ---- the Dodge Value ---- */

int dodge_roll(DodgeAction a);                /* the host's roll of the dice */
bool dodge_roll_ok(DodgeAction a, int roll);  /* could the dice give roll? */
int dodge_value(const Entity *e, DodgeAction a, int roll); /* roll + e's attribute */

/* ---- using it ---- */

/* Uses the slot, spends the stamina, moves e to tile and records the dodge with DV dodge_value(roll); logs it.
 * Exhausted: the turn ends. Requires dodge_check_to == OPR_OK. */
void dodge_apply(Entity *e, DodgeAction a, Hex tile, int roll);
/* target's latest dodge from tile, or NULL. */
const DodgeRec *dodge_find(const Entity *target, Hex tile);
/* The direction (AttackDir) of a dodge with first step `step` from tile, for an attack from `from`; DIR_COUNT when
 * the step goes toward the attacker (no attack direction). */
int dodge_dir(Hex from, Hex tile, int step);

/* ---- lifetime ---- */

void dodges_turn_started(uint32_t id); /* its dodges end */
void dodges_clear_of(uint32_t id);     /* it was deleted, or it dodges again outside initiative */
void dodges_clear(void);               /* the initiative ends, a table is loaded */

#endif
