/* Parties: a name on characters (Entity.party, "" = none). A player sees the characters that share a party with
 * one of their own (hud_can_see); :party lists the parties. Only the GM changes them (OP_SET_PARTY).
 * Design: docs/superpowers/specs/2026-11-26-parties-design.md */
#ifndef TOH_PARTY_H
#define TOH_PARTY_H

#include "app.h"

/* Are a and b characters in the same party (names compared ignoring ASCII case)? False without a party. */
bool party_shared(const Entity *a, const Entity *b);
/* Does the player own a character in e's party? */
bool party_visible(const Entity *e, const char *player);
/* Logs one line per party, in creation order: "Heroes: Mao (anna), Character #7 (NPC)". The party's name is
 * spelled as on its oldest member. viewer NULL: every party (the GM); else only the parties one of the viewer's
 * characters is in. With nothing to list: "No parties" / "None of your characters is in a party". */
void party_log(const char *viewer);

#endif
