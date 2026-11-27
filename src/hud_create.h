/* The creation steps in the HUD: the rows of each step and of the "no character" bar, the values they show, and
 * their events (charcreate.h does the work). hud_game.c decides when to show them. */
#ifndef TOH_HUD_CREATE_H
#define TOH_HUD_CREATE_H

#include "hud.h"

void hud_create_rows(const HudRow **rows, int *n);                   /* the current step's rows */
void hud_create_empty_rows(int waiting, const HudRow **rows, int *n); /* Create + waiting lines (0..3) */
void hud_create_publish(void);       /* the current step's values and tab */
void hud_create_publish_empty(void); /* the Create button, the waiting lines and the tab */
bool hud_create_event(const HudEvent *ev); /* true = it was one of ours ("cc.*") */

#endif
