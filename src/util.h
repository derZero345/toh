#ifndef PF_UTIL_H
#define PF_UTIL_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define SQRT3 1.7320508075688772

/* ---- basic value types ------------------------------------------------ */

typedef struct { double x, y; } Vec2;

typedef struct { uint8_t r, g, b, a; } Color;

static inline Color rgb_hex(uint32_t hex) {
    return (Color){ (uint8_t)(hex >> 16), (uint8_t)(hex >> 8), (uint8_t)hex, 255 };
}
static inline Color color_alpha(Color c, uint8_t a) { c.a = a; return c; }

#define COLOR_RED     ((Color){255,   0,   0, 255})
#define COLOR_GREEN   ((Color){  0, 255,   0, 255})
#define COLOR_BLUE    ((Color){  0,   0, 255, 255})
#define COLOR_MAGENTA ((Color){255,   0, 255, 255})
#define COLOR_BLACK   ((Color){  0,   0,   0, 255})
#define COLOR_WHITE   ((Color){255, 255, 255, 255})

typedef struct { int x, y, w, h; } Rect;

static inline bool rect_contains(Rect r, Vec2 p) {
    return p.x >= r.x && p.y >= r.y && p.x < r.x + r.w && p.y < r.y + r.h;
}

static inline double vec_dist(Vec2 a, Vec2 b) {
    double dx = a.x - b.x, dy = a.y - b.y;
    return sqrt(dx * dx + dy * dy);
}

/* Floor division by two (Java's `v >> 1` on ints). */
static inline int floor_div2(int v) { return (v >= 0) ? v / 2 : -((-v + 1) / 2); }

/* ---- growable arrays -------------------------------------------------- */

#define DA(T) struct { T *items; int count, cap; }

#define da_reserve(a, n) do {                                                  \
        if ((n) > (a)->cap) {                                                  \
            int nc_ = (a)->cap ? (a)->cap * 2 : 16;                            \
            while (nc_ < (n)) nc_ *= 2;                                        \
            (a)->items = realloc((a)->items, (size_t)nc_ * sizeof(*(a)->items)); \
            if (!(a)->items) abort();                                          \
            (a)->cap = nc_;                                                    \
        }                                                                      \
    } while (0)

#define da_push(a, v) do { da_reserve((a), (a)->count + 1); (a)->items[(a)->count++] = (v); } while (0)

#define da_remove_at(a, i) do {                                                \
        int i_ = (i);                                                          \
        memmove(&(a)->items[i_], &(a)->items[i_ + 1],                          \
                (size_t)((a)->count - i_ - 1) * sizeof(*(a)->items));          \
        (a)->count--;                                                          \
    } while (0)

#define da_clear(a) ((a)->count = 0)
#define da_free(a)  do { free((a)->items); (a)->items = NULL; (a)->count = (a)->cap = 0; } while (0)

/* Remove the first element equal (by ==) to v. */
#define da_remove_value(a, v) do {                                             \
        for (int j_ = 0; j_ < (a)->count; j_++)                                \
            if ((a)->items[j_] == (v)) { da_remove_at((a), j_); break; }       \
    } while (0)

/* ---- misc -------------------------------------------------------------- */

char *str_dup(const char *s);
/* Formats a double the way Java's Double.toString does for ordinary values. */
void  fmt_java_double(char *out, size_t n, double v);
double rand_unit(void); /* [0,1) */

#endif
