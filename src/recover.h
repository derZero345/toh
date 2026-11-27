/*
 * Recover actions of Tales of Honor (rulebook: the Personal Actions that only recover stamina).
 *
 *   Recover       Minor, 0 stamina: recover stamina equal to the Stamina Recovery Die (1d4)
 *   Full Recover  Major, 0 stamina: stamina back to its maximum
 *
 * They follow the turn rules of moves.h (initiative, the character's turn, a free slot; a Minor action can use the
 * Major slot), which test mode lifts. A character needs a stamina stat (max stamina above 0), and stamina below its
 * maximum: otherwise there is nothing to recover. Test mode keeps those two.
 *
 * The host rolls the die (ops_apply when not a client) and sends the result in the op, so everyone gets the same
 * stamina and sees the roll in the log.
 *
 * Add an action: a RecoverAction value (before RECOVER_COUNT) and its row in RECOVER_ACTIONS; the radial menu entry
 * with the same id goes into radial_config.c and its rule into radial_game.c.
 */
#ifndef TOH_RECOVER_H
#define TOH_RECOVER_H

#include "moves.h"

typedef enum { RECOVER_SMALL, RECOVER_FULL, RECOVER_COUNT } RecoverAction;

#define RECOVER_DIE 4 /* the Stamina Recovery Die: 1d4 */

typedef struct {
    const char *id; /* also the radial menu entry's id */
    const char *name;
    ActionSlot slot;
    bool full; /* to the maximum; otherwise one roll of the recovery die */
} RecoverActionDef;

extern const RecoverActionDef RECOVER_ACTIONS[RECOVER_COUNT];

int recover_action_by_id(const char *id); /* -1 if none */
/* May e use the action now? OPR_OK or the reason. */
OpResult recover_check(const Entity *e, RecoverAction a);
/* The host's roll for the action: 1..RECOVER_DIE, or 0 for Full Recover. */
int recover_roll(RecoverAction a);
/* Uses the slot and raises e's stamina by rolled (Full Recover: to the maximum); logs what happened.
 * Requires recover_check == OPR_OK. */
void recover_apply(Entity *e, RecoverAction a, int rolled);
/* Only the effect (the resolution phase, phases.h): raises e's stamina by rolled (Full Recover: to the maximum) and
 * logs it. */
void recover_gain(Entity *e, RecoverAction a, int rolled);

#endif
