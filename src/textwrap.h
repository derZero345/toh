/* Word wrapping for text drawn with draw.h (the console log). */
#ifndef TOH_TEXTWRAP_H
#define TOH_TEXTWRAP_H

#include "draw.h"

typedef struct {
    int start, len; /* bytes of the source string */
} TextSpan;

/* Splits s into lines no wider than width, breaking after spaces; a word wider than a line is split between
 * characters (never inside a UTF-8 sequence), and '\n' always breaks. The spaces at a wrap are dropped. Returns
 * the number of lines (at least 1, also for ""); fills out[] with up to max of them. */
int text_wrap(FontStyle style, float size, const char *s, float width, TextSpan *out, int max);

#endif
