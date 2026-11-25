#include "util.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <strings.h>

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

Answer confirm_answer(const char *question, const char *line) {
    /* only what was typed after the question: it starts with a player's name, which may start with n */
    size_t q = strlen(question);
    const char *a = strncmp(line, question, q) == 0 ? line + q : line;
    while (isspace((unsigned char)*a)) a++;
    size_t n = strlen(a);
    while (n > 0 && isspace((unsigned char)a[n - 1])) n--;
    if (n == 0) return ANSWER_YES; /* just Enter */
    static const char *const words[] = { "y", "yes", "n", "no" };
    for (int i = 0; i < 4; i++)
        if (n == strlen(words[i]) && strncasecmp(a, words[i], n) == 0) return i < 2 ? ANSWER_YES : ANSWER_NO;
    return ANSWER_UNCLEAR;
}

int split_args(const char *input, char *buf, size_t cap, char **argv, int max) {
    size_t o = 0;
    int argc = 0;
    const char *p = input;
    while (*p && argc < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || o + 1 >= cap) break;
        argv[argc++] = buf + o;
        bool quoted = false;
        while (*p && (quoted || (*p != ' ' && *p != '\t'))) {
            if (*p == '"') quoted = !quoted;
            else if (o + 1 < cap) buf[o++] = *p;
            p++;
        }
        buf[o++] = '\0';
    }
    return argc;
}
