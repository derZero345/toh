/* Choosing where a move action goes (moves.h): after the action menu (radial_game.c) picks Walk, Run, ..., the
 * action's range is shown, the way to the tile under the pointer is previewed and a left click on a tile in the
 * range moves the character there (OP_MOVE_ACTION). Esc, a right click or a click outside the range cancels.
 * State: gh->moveMode. */
#ifndef TOH_MOVE_UI_H
#define TOH_MOVE_UI_H

#include "app.h"

/* Starts choosing a tile for character id; false (and a log line saying why) if it can't use the action now. */
bool move_ui_start(uint32_t id, int action);
void move_ui_cancel(void);
bool move_ui_active(void);
/* A left press on hex h while choosing: moves there if it is in range, else cancels. True = the press is used. */
bool move_ui_click(Hex h);
/* Once per frame: previews the way to the tile under the pointer; ends the mode when the character is gone, moved
 * or can no longer use the action (its turn ended, the action was used elsewhere). */
void move_ui_update(void);

#endif
