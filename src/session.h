#ifndef TOH_SESSION_H
#define TOH_SESSION_H

/*
 * Multiplayer sessions: the GM hosts (:host), players join (:join) with a join code.
 * The host's table is authoritative: clients send their ops, the host checks and applies
 * them and broadcasts every applied op to everyone (OP_APPLIED), so all tables stay identical.
 * All of this runs on the main thread; net.c does the networking in the background.
 * Design: docs/superpowers/specs/2026-11-21-multiplayer-design.md, sections 1-3 and 5.
 */

#include "image.h"
#include "ops.h"

/* The GM is player 0 and always gold; players get the first free palette color. */
#define SESSION_GM_COLOR 0xE0B000u

void session_init(void);     /* after gfx_init: registers the wake-up event */
void session_shutdown(void); /* on quit: ends the session, removes the port mapping */
void session_poll(void);     /* on gh->netEvent: handles everything the network thread delivered */
void session_set_port(uint16_t port); /* --port */

bool        session_is_host(void);
bool        session_is_client(void); /* joining or joined */
const char *session_my_name(void);
/* @waitnet: host: at least n players joined; client: joined. */
bool        session_ready(int n);
/* Ring color of a character owned by owner: true when a ring is drawn (in a session, owner not empty).
 * mine: we are that owner. Owners who are not at the table are grey. */
bool        session_owner_color(const char *owner, uint32_t *color, bool *mine);

/* Called by ops_submit. */
void session_send_op(const Op *op);      /* client: send to the host */
void session_broadcast_op(const Op *op); /* host: after applying the GM's own op */

void session_table_replaced(void); /* host: after :load, every player gets the new table */
bool session_roll(const char *spec); /* :r in a session: rolled by the host. false = offline */

/* Commands */
void session_cmd_host(const char *name, bool direct); /* direct: no relay, players connect to us */
void session_cmd_unhost(void);
void session_cmd_join(const char *code, const char *name);
void session_cmd_leave(void);
void session_cmd_players(void);
void session_cmd_kick(const char *name);
void session_cmd_ban(const char *name);   /* NULL: list the bans */
void session_cmd_unban(const char *name);
void session_cmd_claim(void);

/* The console's yes/no prompt became free: asks the next queued request. */
void session_prompt_free(void);
/* Client: asks the GM for a new character of ours with this picture (at most NET_CREATE_MAX_DIM per side). */
void session_request_character(const Image *pic, Hex tile, int size, int maxHealth, int speed, int initiative,
                               const char *name);
/* Host: while a new character is being asked about, draws its picture faintly on its tile (only for the GM). */
void session_draw_request_preview(void);

/* Presence (spec section 4). After a frame was drawn the local presence may have changed; session_tick
 * sends it (at most every SESSION_PRESENCE_MS) and, on the host, forwards the players'. It returns the
 * milliseconds until it needs to run again, or -1. session_draw_presence draws the other people's. */
#define SESSION_PRESENCE_MS 50
void session_presence_changed(void);
int  session_tick(void);
void session_draw_presence(void);

#endif
