/*
 * User settings: a plain text file of "key = value" lines that the game reads at start and rewrites when a setting
 * changes. Lines it doesn't know (comments, blank lines, other keys) are kept as they are, so the file can be edited
 * by hand while the game is closed.
 *
 * The file is settings.txt next to the program (it is in .gitignore). TOH_SETTINGS=<file> uses another file;
 * TOH_SETTINGS= (empty) uses none, so nothing is remembered (the tests do that).
 *
 * Keys: theme (the colour theme chosen with :t), ui_scale (Ctrl + / Ctrl - steps from the normal size, -24..64).
 */
#ifndef TOH_SETTINGS_H
#define TOH_SETTINGS_H

#include <stdbool.h>

/* Reads the file at path; a missing file is no error. NULL = no file: settings_set only keeps values in memory. */
void settings_init(const char *path);
void settings_free(void);
const char *settings_path(void); /* NULL without a file */
/* The value of key, or NULL. Valid until the next settings_set or settings_free. */
const char *settings_get(const char *key);
/* Sets key and rewrites the file. False if the file couldn't be written, or key / value are not one line or the
 * key is empty or holds '=' or '#'. */
bool settings_set(const char *key, const char *value);

#endif
