/* Choosing where an attack (attacks.h) or a block (blocks.h) goes. After the action menu (radial_game.c) picks one,
 * the tiles in its range are shown; a left click on one of them opens a ring on that tile: the direction menu for an
 * attack (choosing a direction attacks, OP_ATTACK_ACTION), the toggle ring for a block (Up / Right / Left switch on
 * and off, the centre confirms, OP_BLOCK_ACTION). A block of range 0 covers the blocker's own tiles: its ring opens on
 * its tile at once. Closing the ring goes back to choosing a tile (range 0: cancels). Esc, a right click or a click
 * outside the range cancels. State: gh->targetMode. */
#ifndef TOH_TARGET_UI_H
#define TOH_TARGET_UI_H

#include "app.h"

typedef enum { TARGET_ATTACK, TARGET_BLOCK } TargetKind;

/* Starts choosing for character id's attack or block `action`; false (and a log line saying why) if it can't use it
 * now. */
bool target_ui_start(TargetKind kind, uint32_t id, int action);
void target_ui_cancel(void); /* also closes its ring */
bool target_ui_active(void);
/* A left press on hex h while choosing a tile: opens the ring if h is in range, else cancels. True = the press is
 * used. */
bool target_ui_click(Hex h);
/* The direction menu chose d (AttackDir): attacks the chosen tile and ends the mode. */
void target_ui_attack(int d);
/* The toggle ring confirmed the directions dirs (DIR_BIT bits): blocks the chosen tile and ends the mode. */
void target_ui_block(int dirs);
/* The ring closed without a choice: back to choosing a tile (a block of range 0 cancels). */
void target_ui_back(void);
/* Once per frame: ends the mode when the character is gone, moved or can no longer use the action. */
void target_ui_update(void);
/* The slot the chosen action would use now (SLOT_MAJOR / SLOT_MINOR), for its colour. */
int target_ui_slot(void);

#endif
