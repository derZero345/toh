/* Choosing where an attack goes (attacks.h): after the action menu (radial_game.c) picks an attack, the tiles in its
 * range are shown; a left click on one of them opens the direction menu on that tile (radial_game_open_directions),
 * and choosing a direction there attacks (OP_ATTACK_ACTION). Closing the direction menu goes back to choosing a tile.
 * Esc, a right click or a click outside the range cancels. State: gh->attackMode. */
#ifndef TOH_ATTACK_UI_H
#define TOH_ATTACK_UI_H

#include "app.h"

/* Starts choosing a tile for character id; false (and a log line saying why) if it can't use the attack now. */
bool attack_ui_start(uint32_t id, int action);
void attack_ui_cancel(void);
bool attack_ui_active(void);
/* A left press on hex h while choosing a tile: opens the direction menu if h is in range, else cancels. True = the
 * press is used. */
bool attack_ui_click(Hex h);
/* The direction menu chose d (AttackDir): attacks the chosen tile and ends the mode. */
void attack_ui_choose(int d);
/* The direction menu closed without a choice: back to choosing a tile. */
void attack_ui_back(void);
/* Once per frame: ends the mode when the attacker is gone, moved or can no longer use the attack. */
void attack_ui_update(void);

#endif
