/* The HUD's connection to the game (hud_game.c): the only HUD code that reads or changes game state. */
#ifndef TOH_HUD_GAME_H
#define TOH_HUD_GAME_H

#include "app.h"
#include "hud.h"

void hud_game_init(const char *resource_dir); /* icons and the event handler; after console_init / toolbox_init */
void hud_game_shutdown(void);
/* Publishes this frame's values (call once per drawn frame, before hud_draw). dt: seconds since the previous
 * frame while bars move, else 0. True while a bar is still moving. */
bool hud_game_update(float dt);
void hud_game_refresh_screen(void); /* sets the HUD's screen now (scripts ask for part rectangles before a frame) */
/* May the viewer see this character's HP? The GM (or playing alone): any character; a player: their own and
 * those in a party with one of their own (party.h). */
bool hud_can_see(const Entity *e);
/* The character the HUD shows, or NULL. *others: how many more visible characters are selected;
 * *from_selection: it is selected (else it is the player's own oldest character). */
Entity *hud_pick_character(int *others, bool *from_selection);
/* Input from input.c; each true = the map must ignore it. */
bool hud_game_mouse_down(Vec2 p);
bool hud_game_mouse_move(Vec2 p);
bool hud_game_mouse_up(Vec2 p);
bool hud_game_contains(Vec2 p);
bool hud_game_key(SDL_Keycode key); /* 1..9, 0 and the keypad, while the console is closed */

#endif
