/* The radial menu's connection to the game (radial_game.c): the only radial menu code that reads or changes game
 * state. input.c tells it about clicks; it decides which menu a hex gets, provides the rules the menus' conditions
 * name, and turns the menu's actions into game code. */
#ifndef TOH_RADIAL_GAME_H
#define TOH_RADIAL_GAME_H

#include "app.h"
#include "radial.h"

void radial_game_init(void); /* rules and the event handler */
void radial_game_shutdown(void);
/* The rules the menus may name (tests check the config against them). */
const RadialRule *radial_game_rules(int *n);
/* The menu id for a hex: "actions" with a character the local user controls on it (radial_game_controls),
 * "character" with another character, else "tile". */
const char *radial_game_menu_for(Hex h);
/* Does the local user control e: the GM (and offline) every character, a player the ones they own. */
bool radial_game_controls(const Entity *e);
/* The character the open "actions" menu is for, 0 if none is open. */
uint32_t radial_game_actor(void);

/* A right click (press and release without dragging) at p: opens the menu for the hex there. */
bool radial_game_open(Vec2 p);
/* A right press at p while the menu may be open: closes it. True if that used the press up (it was on the menu or
 * on the menu's own hex); false lets it go on as a normal right press, so a right click elsewhere reopens it. */
bool radial_game_right_down(Vec2 p);
void radial_game_close(void); /* Esc */
/* Left button and motion from input.c; each true = the map must ignore it. */
bool radial_game_mouse_down(Vec2 p);
bool radial_game_mouse_move(Vec2 p);
bool radial_game_mouse_up(Vec2 p);
bool radial_game_contains(Vec2 p);
void radial_game_refresh_screen(void); /* scripts ask for part points before a frame */
/* Once per drawn frame: follows the hex when the view moves and animates. True while it still moves. */
bool radial_game_update(float dt);

#endif
