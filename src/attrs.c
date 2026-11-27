/* Attribute rules (see attrs.h). */
#include "attrs.h"

#include <strings.h>

const char *const ATTR_NAMES[ATTR_COUNT] = { "STR", "AGI", "COG", "SPI" };

AttrStats attr_derive(const int attr[ATTR_COUNT]) {
    AttrStats d;
    d.maxHealth = 12 + 3 * attr[ATTR_STR];
    d.maxStamina = 8 + attr[ATTR_AGI];
    d.speed = attr[ATTR_AGI] + 3;
    d.initiative = attr[ATTR_COG];
    if (d.maxHealth < 0) d.maxHealth = 0;
    if (d.maxStamina < 0) d.maxStamina = 0;
    if (d.speed < 2) d.speed = 2;
    return d;
}

int attr_by_name(const char *s) {
    for (int i = 0; i < ATTR_COUNT; i++)
        if (strcasecmp(s, ATTR_NAMES[i]) == 0) return i;
    return -1;
}
