#include "util.h"

#include <math.h>
#include <stdio.h>

char *str_dup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (!d) abort();
    memcpy(d, s, n);
    return d;
}

void fmt_java_double(char *out, size_t n, double v) {
    if (v == floor(v) && fabs(v) < 1e7) {
        snprintf(out, n, "%.1f", v);
    } else {
        snprintf(out, n, "%.15g", v);
    }
}

double rand_unit(void) {
    return (double)rand() / ((double)RAND_MAX + 1.0);
}
