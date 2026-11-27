/* The attributes of Tales of Honor (STR, AGI, COG, SPI) and the stats they set (the rulebook's stats table):
 * Max HP 12 + 3 STR, Max Stamina 8 + AGI, Speed AGI + 3 (at least 2), Initiative COG. SPI sets nothing yet.
 * No dependencies: app.h, ops.c, console.c and the creation steps include it. */
#ifndef TOH_ATTRS_H
#define TOH_ATTRS_H

#include <stdbool.h>

enum { ATTR_STR, ATTR_AGI, ATTR_COG, ATTR_SPI, ATTR_COUNT };
#define ATTR_MIN (-10)        /* :c, saves, the wire */
#define ATTR_MAX 10
#define ATTR_CREATE_MIN (-2)  /* the creation steps and players' requests (the rulebook's range) */
#define ATTR_CREATE_MAX 3

extern const char *const ATTR_NAMES[ATTR_COUNT]; /* "STR", "AGI", "COG", "SPI" */

typedef struct { int maxHealth, maxStamina, speed, initiative; } AttrStats;

/* Max HP and max stamina never go below 0 (0 = the "no HP" / "no stamina" state). */
AttrStats attr_derive(const int attr[ATTR_COUNT]);
/* "str" .. "spi", any case; -1 if it is none of them. */
int attr_by_name(const char *s);
static inline bool attr_valid(int v) { return v >= ATTR_MIN && v <= ATTR_MAX; }

#endif
