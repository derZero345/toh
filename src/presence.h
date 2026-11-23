#ifndef TOH_PRESENCE_H
#define TOH_PRESENCE_H

/*
 * Presence (spec section 4): what each person at the table is pointing at. presence_collect turns the
 * local selection, movement preview and tools into a NetPresence; presence_draw shows other people's
 * presence in their colors. Positions travel as tiles or as world coordinates (NET_WORLD_SCALE per hex
 * size, relative to the grid origin), so every screen draws them with its own zoom and pan.
 */

#include "netmsg.h"

typedef struct {
    const NetPresence *p;
    uint32_t color; /* 0xRRGGBB */
    const char *name;
} RemotePresence;

/* Fills out from the local state; free with netpresence_free. */
void presence_collect(NetPresence *out);

/* Draws everyone in list (in that order; later people's outlines are inset when tiles are shared). */
void presence_draw(const RemotePresence *list, int n);

/* World coordinate conversions (exposed for tests). */
int32_t presence_world_x(double px);
int32_t presence_world_y(double py);
double  presence_px_x(int32_t wx);
double  presence_px_y(int32_t wy);

#endif
