/*
 * Move actions of Tales of Honor (rulebook: Actions -> Move, and the Personal Actions with type Move).
 *
 * During initiative each character has a Major and a Minor action per turn; a Major action can be used as a Minor
 * one. A move action uses its slot, costs stamina and moves the character along adjacent tiles up to its distance.
 * Reaching 0 stamina makes the character exhausted and ends its turn. Outside initiative there are no turns, so
 * move actions can't be used (characters move freely as before).
 *
 * Only the actions the map can support are here: no levels, terrain or conditions yet, so Jump, Long Jump, Climb,
 * Swimming, Glide, Hang and Turn Around are left out, and prerequisites about them (Falling, Prone, under attack)
 * are not checked.
 *
 * Test mode (:tm, gh->testMode, the whole table) lifts the requirements so features can be tried out: initiative,
 * the character's turn, a free slot (action_turn_check, also for recover actions), Sprint's earlier move and enough
 * stamina never block. The effects stay: the slot
 * is marked used, stamina is spent down to 0 at most, and the turn ends only when an action uses up the last
 * stamina. Ownership, the range and speed 0 still hold (attacks.h: the same for attacks). Keep this list in the guides
 * when a requirement is added.
 *
 * Add an action: a MoveAction value (before MOVE_COUNT) and its row in MOVE_ACTIONS; the radial menu entry with the
 * same id goes into radial_config.c. Nothing else changes.
 */
#ifndef TOH_MOVES_H
#define TOH_MOVES_H

#include "app.h"
#include "ops.h"

typedef enum { MOVE_STEP, MOVE_WALK, MOVE_RUN, MOVE_SPRINT, MOVE_CRAWL, MOVE_COUNT } MoveAction;
typedef enum { SLOT_MAJOR, SLOT_MINOR } ActionSlot;
typedef enum { DIST_ONE, DIST_HALF_SPEED, DIST_SPEED } MoveDistance;

/* Entity.turnUsed bits */
enum { TURN_MAJOR = 1, TURN_MINOR = 2, TURN_MOVED = 4, TURN_ALL = 7 };

typedef struct {
    const char *id;   /* also the radial menu entry's id */
    const char *name;
    ActionSlot slot;
    int stamina;
    MoveDistance distance;
    bool afterMove; /* needs another move action earlier this turn (Sprint) */
} MoveActionDef;

extern const MoveActionDef MOVE_ACTIONS[MOVE_COUNT];

/* The turn rules every action shares (move actions here, recover actions in recover.h). */
/* Which slot an action of slot s would use now: its own; a Minor one uses the Major slot when the Minor one is
 * used. -1 if none is free. */
int action_slot(const Entity *e, ActionSlot s);
/* Initiative runs, it is e's turn and a slot is free; test mode lifts all three. OPR_OK or the reason. */
OpResult action_turn_check(const Entity *e, ActionSlot s);
/* Marks the slot action_slot names (its own if none is free: test mode) as used. */
void action_use_slot(Entity *e, ActionSlot s);
/* Spends cost stamina; a character without a stamina stat (max 0) pays nothing, and test mode spends what there is
 * (never below 0). *before = the stamina before. True if stamina was spent. */
bool action_spend_stamina(Entity *e, int cost, int *before);
/* After an action: if it used up the last stamina (spent, before > 0, now <= 0) on e's turn, logs that e is exhausted
 * and ends the turn. */
void action_end_if_exhausted(Entity *e, int before, bool spent);

int move_action_by_id(const char *id); /* -1 if none */
/* Tiles the action may move e: 1, half its speed rounded up, or its speed. */
int move_distance(const Entity *e, MoveAction a);
int move_slot(const Entity *e, MoveAction a); /* action_slot for the action's slot */
/* May e use the action now (before choosing where)? OPR_OK or the reason. */
OpResult move_check(const Entity *e, MoveAction a);
/* ...and move to tile: move_check plus the tile in the action's range (move_range). */
OpResult move_check_to(const Entity *e, MoveAction a, Hex tile);
/* The tiles e can reach with the action (adjacent steps, tiles it fits on). */
void move_range(const Entity *e, MoveAction a, TileSet *out);
/* Uses the slot, spends the stamina and moves e (path: the way it took, may be NULL). Exhausted: the turn ends.
 * Requires move_check_to == OPR_OK. log: write what happened to the console. */
void move_apply(Entity *e, MoveAction a, Hex tile, HexList *path, bool log);
const char *move_result_text(OpResult r); /* like ops_result_text, worded for the menu */

#endif
