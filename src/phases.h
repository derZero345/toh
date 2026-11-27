/*
 * The phases of a combat round (rulebook: Combat -> Combat Turns). Design: docs/superpowers/specs/2026-11-27-phases-design.md.
 *
 * While initiative runs, a round has two phases (gh->phase):
 *   declaration  in initiative order each character declares its actions (the action ops: move, recover, attack,
 *                block, dodge); prerequisites are checked and stamina and the slot are spent now. A declared move
 *                or dodge sets the character's tile to where it ends (the planned tile), so its later declarations
 *                and everyone's room checks start there; the token stays drawn where it stood (Entity.shown) until
 *                the move resolves. Blocks and dodges (and their rolls) take effect at once, for the whole round.
 *                During its declaring turn (test mode: any time of this phase) a character can take back its last
 *                declaration (OP_TAKE_BACK).
 *   resolution   :i next after the last declarer starts it at the top; each :i next resolves the current
 *                character's declarations in the order made (moves and dodges move the token, attacks hit, Recover
 *                rolls), the host deciding: OP_INIT_NEXT carries every result (phases_results), so players who
 *                joined later stay right. After the last one, :i next starts the next round's declaration, which
 *                clears the round's marks, blocks, dodges and declarations.
 * Nothing can be declared during resolution, even in test mode (OPR_RESOLVING). Outside initiative actions happen
 * at once, as before.
 *
 * Round-wide: an attack hits every character that is on its tile at some point of the round (phases_presence): a
 * tile where one of its moves starts or ends, or where it stays, gives the normal result; a tile passed in the middle
 * of a move gives at least a scrape; a tile it dodged from, the dodge contest (dodges.h).
 */
#ifndef TOH_PHASES_H
#define TOH_PHASES_H

#include "attacks.h"
#include "blocks.h"
#include "dodges.h"
#include "recover.h"

typedef enum { PHASE_DECLARE, PHASE_RESOLVE } Phase;
typedef enum { PRESENCE_NONE, PRESENCE_STOP, PRESENCE_PASSED } Presence;

#define PHASES_MAX_RESULTS 400000 /* values in one resolution (= NETMSG_MAX_RESULTS) */

/* Initiative runs: the action ops declare instead of acting at once. */
bool phases_declaring(void);
/* Is e on tile at some point of this round (Presence)? Outside initiative: on its tile now or not. */
int phases_presence(const Entity *e, Hex tile);
bool phases_has_declared(uint32_t id); /* anything not resolved yet */
/* Is e's token drawn on h: its shown tile while its declared moves wait, else where it is? */
bool phases_shown_on(const Entity *e, Hex h);

/* ---- declaring (ops_apply while initiative runs; the checks are the actions' own) ---- */

void phases_declare_move(Entity *e, MoveAction a, Hex tile);
void phases_declare_recover(Entity *e, RecoverAction a);
void phases_declare_attack(Entity *e, AttackAction a, Hex tile, AttackDir d);
void phases_declare_block(Entity *e, BlockAction a, Hex tile, int dirs);
void phases_declare_dodge(Entity *e, DodgeAction a, Hex tile, int roll);

/* ---- taking back ---- */

/* May e take back its last declaration now? The declaration phase, its turn (test mode: anyone), something
 * declared. */
OpResult phases_take_back_check(const Entity *e);
void phases_take_back(Entity *e); /* restores slot, stamina, tile, block, dodge, mark; logs it */
/* What taking back e's last declaration goes back to; false if there is none. */
bool phases_take_back_state(const Entity *e, Hex *tile, int *stamina, int *turnUsed);

/* ---- resolving ---- */

/* The host: resolves actor's declarations (rolls, characters hit, outcomes) into out, one record each:
 *   move, dodge:  type, action, row, col
 *   recover:      type, action, roll
 *   attack:       type, action, row, col, dir, origin row, origin col, 3 damage rolls, n, then n x (id, outcome)
 * Blocks need nothing. */
void phases_results(uint32_t actor, DoubleList *out);
bool phases_results_ok(const double *v, int n); /* well formed, values in range */
/* Everyone: applies the results to actor (logs them), then its declarations are resolved and its token is drawn
 * where it stands. */
void phases_apply_results(uint32_t actor, const double *v, int n);

/* ---- the round ---- */

void phases_round_start(void);    /* the next round's declaration: marks, blocks, dodges, declarations go */
void phases_clear(void);          /* the initiative ends, a table is loaded */
void phases_clear_of(uint32_t id); /* it was deleted */
/* "DECLARATION · Mao", "RESOLUTION · Mao", "RESOLUTION · done"; false outside a running round. */
bool phases_label(char *buf, size_t n);

#endif
