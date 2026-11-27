/* Creating a character through the HUD's steps (spec: docs/superpowers/specs/2026-11-27-character-creation-design.md).
 * The draft and its steps; no drawing. hud_create.c shows it in the HUD, input.c / graphics.c place it on the map.
 * The GM's character is spawned (OP_SPAWN, no owner); a player's is asked for (session_request_character). */
#ifndef TOH_CHARCREATE_H
#define TOH_CHARCREATE_H

#include "app.h"
#include "image.h"

typedef enum { CC_NAME, CC_SIZE, CC_ATTRS, CC_PICTURE, CC_SUMMARY, CC_STEPS } CcStep;

typedef struct {
    bool active;  /* creation is running (the steps or placing) */
    bool placing; /* after the summary: the picture follows the mouse, a click places it */
    CcStep step;
    char name[CHAR_NAME_MAX];
    int size;
    int attr[ATTR_COUNT];
    bool hasTile; /* started from the tile menu: created on tile, no placing */
    Hex tile;
    Texture *picture; /* one reference, or NULL */
    Image small;      /* players: the picture for the request (px NULL = none) */
    unsigned gen;     /* counts cc_start calls: a restart on the same step is a new draft (hud_game.c refocuses) */
} CharCreate;

const CharCreate *cc_state(void);
/* tile: from the tile menu, else NULL. Drops a running draft and ends choosing a move's tile (move_ui.h), as
 * placing does. */
void cc_start(const Hex *tile);
void cc_cancel(void);           /* logs "Character creation cancelled" */
void cc_shutdown(void);         /* drops a draft without a log line (before the renderer goes) */
bool cc_can_next(const char **why);
void cc_next(void);             /* the next step; on the summary: place (or create on the chosen tile) */
void cc_back(void);             /* the step before; while placing: back to the summary */
void cc_set_name(const char *name);
void cc_set_size(int size);     /* SIZE_MIN..SIZE_MAX_CLASS, else ignored */
void cc_step_attr(int attr, int delta); /* within ATTR_CREATE_MIN..ATTR_CREATE_MAX */
void cc_choose_picture(void);   /* opens the PNG dialog (ACTION_CREATE_PICTURE) */
/* The chosen picture: t is retained; small (players' request copy, may be empty) is taken over. */
void cc_set_picture(Texture *t, Image small);
/* While placing, a click on tile h: places the character there if it fits, else logs "No room on that tile".
 * True = the click was used (always, while placing). */
bool cc_place(Hex h);
bool cc_placing(void);

#endif
