/* User settings in a plain text file (see settings.h). */
#include "settings.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HEADER "# Tales of Honor settings. The game rewrites this file when a setting changes;\n" \
               "# edit it while the game is closed. One \"key = value\" per line.\n"

/* Every line of the file in order; key is NULL for lines that are no setting (comments, blank, unreadable). */
typedef struct {
    char *text;  /* the line as read (kept for non-settings) */
    char *key;   /* trimmed */
    char *value; /* trimmed */
} Line;

static Line *lines;
static int nlines, caplines;
static char *path;

static char *dup_range(const char *s, size_t n) {
    char *d = malloc(n + 1);
    if (!d) abort();
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

static char *dup_trimmed(const char *s, size_t n) {
    while (n > 0 && isspace((unsigned char)*s)) s++, n--;
    while (n > 0 && isspace((unsigned char)s[n - 1])) n--;
    return dup_range(s, n);
}

static Line *push_line(void) {
    if (nlines == caplines) {
        caplines = caplines ? caplines * 2 : 16;
        lines = realloc(lines, (size_t)caplines * sizeof(Line));
        if (!lines) abort();
    }
    Line *l = &lines[nlines++];
    memset(l, 0, sizeof(*l));
    return l;
}

/* A setting line is "key = value" with a key that isn't empty and no '#' before the '='. */
static void parse_line(Line *l) {
    const char *s = l->text, *eq = strchr(s, '=');
    while (isspace((unsigned char)*s)) s++;
    if (!eq || *s == '#' || *s == '=' || memchr(s, '#', (size_t)(eq - s))) return;
    l->key = dup_trimmed(s, (size_t)(eq - s));
    l->value = dup_trimmed(eq + 1, strlen(eq + 1));
}

void settings_free(void) {
    for (int i = 0; i < nlines; i++) {
        free(lines[i].text);
        free(lines[i].key);
        free(lines[i].value);
    }
    free(lines);
    lines = NULL;
    nlines = caplines = 0;
    free(path);
    path = NULL;
}

void settings_init(const char *file) {
    settings_free();
    if (!file) return;
    path = dup_range(file, strlen(file));
    FILE *f = fopen(file, "r");
    if (!f) return;
    char buf[1024];
    while (fgets(buf, sizeof(buf), f)) {
        size_t n = strlen(buf);
        bool whole = n > 0 && buf[n - 1] == '\n';
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) n--;
        Line *l = push_line();
        l->text = dup_range(buf, n);
        if (!whole && !feof(f)) { /* longer than the buffer: keep the rest of it in this line, not as a setting */
            int c;
            while ((c = fgetc(f)) != EOF && c != '\n') {}
            continue;
        }
        parse_line(l);
    }
    fclose(f);
}

const char *settings_path(void) { return path; }

static Line *find(const char *key) {
    for (int i = nlines - 1; i >= 0; i--) /* the last one counts, like most config files */
        if (lines[i].key && strcmp(lines[i].key, key) == 0) return &lines[i];
    return NULL;
}

const char *settings_get(const char *key) {
    Line *l = find(key);
    return l ? l->value : NULL;
}

static bool one_line(const char *s) { return !strchr(s, '\n') && !strchr(s, '\r'); }

static bool write_file(void) {
    if (!path) return true;
    size_t n = strlen(path);
    char *tmp = malloc(n + 5);
    if (!tmp) abort();
    snprintf(tmp, n + 5, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    bool ok = f != NULL;
    if (ok) {
        if (nlines == 0 || lines[0].key) fputs(HEADER, f); /* a new file explains itself */
        for (int i = 0; i < nlines; i++) fprintf(f, "%s\n", lines[i].text);
        ok = fflush(f) == 0 && !ferror(f);
        ok = fclose(f) == 0 && ok;
        ok = ok && rename(tmp, path) == 0; /* replaces the old file in one step */
        if (!ok) remove(tmp);
    }
    free(tmp);
    return ok;
}

bool settings_set(const char *key, const char *value) {
    if (!key || !value || !one_line(key) || !one_line(value) || strchr(key, '=') || strchr(key, '#')) return false;
    char *k = dup_trimmed(key, strlen(key)), *v = dup_trimmed(value, strlen(value));
    if (!k[0]) {
        free(k);
        free(v);
        return false;
    }
    size_t n = strlen(k) + strlen(v) + 4;
    char *text = malloc(n);
    if (!text) abort();
    snprintf(text, n, "%s = %s", k, v);
    Line *l = find(k);
    if (!l) l = push_line();
    free(l->text);
    free(l->key);
    free(l->value);
    l->text = text;
    l->key = k;
    l->value = v;
    return write_file();
}
