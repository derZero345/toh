/* Dice in "NdM" notation (calc/Dice), plus an optional +K / -K modifier. */
#include "app.h"

#include <ctype.h>

static bool parse_int(const char **s, int *out) {
    const char *p = *s;
    if (!isdigit((unsigned char)*p)) return false;
    long v = 0;
    while (isdigit((unsigned char)*p)) {
        v = v * 10 + (*p - '0');
        if (v > 100000) return false;
        p++;
    }
    *out = (int)v;
    *s = p;
    return true;
}

bool dice_parse(const char *spec, int *num, int *sides, int *modifier) {
    const char *p = spec;
    *modifier = 0;
    if (!parse_int(&p, num)) return false;
    if (*p != 'd' && *p != 'D') return false;
    p++;
    if (!parse_int(&p, sides)) return false;
    if (*p == '+' || *p == '-') {
        int sign = (*p == '-') ? -1 : 1;
        p++;
        if (!parse_int(&p, modifier)) return false;
        *modifier *= sign;
    }
    return *p == '\0' && *sides > 0 && *num <= 1000;
}

int dice_roll(int num, int sides, int modifier, int *rolls) {
    int sum = 0;
    for (int i = 0; i < num; i++) {
        rolls[i] = (int)(rand_unit() * sides) + 1;
        sum += rolls[i];
    }
    return sum + modifier;
}
