/*
 * Blocks of Tales of Honor (rulebook: Base Action Types -> Block). Design:
 * docs/superpowers/specs/2026-11-27-blocks-design.md.
 *
 * A block covers one, two or all three of the directions up, left and right on a tile (DIR_BIT bits of attacks.h);
 * neutral is always covered. Range 0 blocks the blocker's own tiles (all of them for a big character), a larger range
 * one tile within it (own tiles included). Blocks stay on their tiles, also when the blocker moves away, until the
 * next round's declaration starts (phases.h), the initiative ends or it is deleted; outside initiative (test mode)
 * its next block replaces
 * them. Several blocks on one tile act as one: their directions combine and the highest Block Value applies
 * (block_on_tile, which attacks.c's block hook asks).
 *
 * The failed block: when an attack's block contest on a tile gives success (block_failed), every blocker there that
 * didn't occupy the tile when it blocked can't block that tile again in the next round, unless it occupies it then
 * (a BlockLock). During initiative the lock ends when the round after next starts; outside
 * initiative its next block somewhere else ends it; the initiative ending and deleting the blocker end it too.
 *
 * Blocks follow the turn rules of moves.h and cost stamina. Test mode lifts initiative, the turn, the slot and
 * stamina, but not the range, ownership or the lock (the user's choice).
 *
 * Blocks are table state, not saved and not sent to players who join later: the host decides attack outcomes and
 * sends them (attacks.h), so a late joiner's missing blocks never change anyone's HP.
 *
 * Add a block: a BlockAction value (before BLOCK_COUNT) and its row in BLOCK_ACTIONS; the radial menu entry with the
 * same id goes into radial_config.c (a "Block" submenu of its slot) and its rule into radial_game.c.
 */
#ifndef TOH_BLOCKS_H
#define TOH_BLOCKS_H

#include "attacks.h"

typedef enum { BLOCK_TEST, BLOCK_TEST_RANGE, BLOCK_COUNT } BlockAction;

/* The directions a block can choose (neutral is always covered). */
#define BLOCK_DIRS (DIR_BIT(DIR_UP) | DIR_BIT(DIR_LEFT) | DIR_BIT(DIR_RIGHT))

typedef struct {
    const char *id; /* also the radial menu entry's id */
    const char *name;
    ActionSlot slot;
    int stamina;
    int range; /* 0 = the blocker's own tiles, else one tile within this many (hex distance) */
    int value; /* the Block Value (BV) */
} BlockActionDef;

extern const BlockActionDef BLOCK_ACTIONS[BLOCK_COUNT];

int block_action_by_id(const char *id); /* -1 if none */

/* ---- tiles ---- */

/* May e's block a cover tile (by range; not the lock)? Range 0: one of e's tiles. */
bool block_in_range(const Entity *e, BlockAction a, Hex tile);
void block_range(const Entity *e, BlockAction a, TileSet *out);
/* Is tile locked for e (a failed block there) while e doesn't occupy it? */
bool block_locked(const Entity *e, Hex tile);

/* ---- checks ---- */

/* May e use the action now (before choosing where)? OPR_OK or the reason. */
OpResult block_check(const Entity *e, BlockAction a);
/* ...on tile with directions dirs (a non-empty subset of BLOCK_DIRS): plus range and the lock. */
OpResult block_check_at(const Entity *e, BlockAction a, Hex tile, int dirs);

/* ---- using it ---- */

/* Uses the slot, spends the stamina and puts the block on tile (range 0: on all of e's tiles); logs it. Exhausted:
 * the turn ends. Requires block_check_at == OPR_OK. */
void block_apply(Entity *e, BlockAction a, Hex tile, int dirs);
/* The combined block on tile: every block's directions and the highest Block Value. False if none. */
bool block_on_tile(Hex tile, int *dirs, int *value);
/* An attack's block contest on tile gave success: locks the tile for the blockers that didn't occupy it. */
void block_failed(Hex tile);
/* Writes "up, left" style names of dirs into buf. */
void block_dirs_text(int dirs, char *buf, size_t cap);

/* ---- lifetime ---- */

/* A round starts (phases.h, for every character): its blocks go; its armed locks end, newer ones arm */
void blocks_turn_started(uint32_t id);
void blocks_clear_of(uint32_t id);     /* it was deleted */
void blocks_clear(void);               /* the initiative ends, a table is loaded */

#endif
