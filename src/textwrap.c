#include "textwrap.h"

#include <string.h>

/* Bytes of the UTF-8 character at s[i] (a lead byte and its continuation bytes). */
static int char_len(const char *s, int i, int n) {
    int l = 1;
    while (i + l < n && ((unsigned char)s[i + l] & 0xC0) == 0x80) l++;
    return l;
}

static float char_width(FontStyle style, float size, const char *s, int len) {
    char c[8];
    if (len > 7) len = 7;
    memcpy(c, s, (size_t)len);
    c[len] = '\0';
    return dr_text_width(style, size, c);
}

int text_wrap(FontStyle style, float size, const char *s, float width, TextSpan *out, int max) {
    int n = (int)strlen(s), lines = 0, pos = 0;
    do {
        int start = pos, i = pos, brk = -1; /* brk: just after the last space on this line */
        float w = 0;
        while (i < n && s[i] != '\n') {
            int cl = char_len(s, i, n);
            float cw = char_width(style, size, s + i, cl);
            if (w + cw > width && i > start) break; /* at least one character per line */
            w += cw;
            i += cl;
            if (s[i - 1] == ' ') brk = i;
        }
        int end = i;
        bool wrapped = i < n && s[i] != '\n';
        if (wrapped && brk > start) end = brk;
        int len = end - start;
        while (wrapped && len > 0 && s[start + len - 1] == ' ') len--; /* the spaces at the wrap */
        if (lines < max) out[lines] = (TextSpan){ start, len };
        lines++;
        pos = end;
        if (pos < n && s[pos] == '\n') pos++;
        else
            while (pos < n && s[pos] == ' ') pos++;
    } while (pos < n);
    return lines;
}
