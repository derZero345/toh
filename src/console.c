/* In-app console: log, input line, history, auto-completion and all commands
 * (main/Consol, main/ConsolPanel, main/CommandCompleter). */
#include "app.h"
#include "ops.h"
#include "savefmt.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <strings.h>

#define C (&gh->console)

/* ---- command completer ----------------------------------------------------------------- */

typedef struct {
    const char *name;
    const char *const *args; /* NULL-terminated, NULL = no arguments */
} CommandDef;

static const char *const ARGS_CHARACTER[] = { "delete", "size", "maxhealth", "armorclass", "speed", "initiative", "icon", "owner", NULL };
static const char *const ARGS_DELETE[]    = { "delete", NULL };
static const char *const ARGS_HELP[]      = { "quit", "background", "debug", "character", "wall", "entity", "help",
                                              "roll", "grid", "gamemaster", "init", "clear", "theme", "teleport", "save", "load", NULL };
static const char *const ARGS_INIT[]      = { "add", "remove", "clear", "show", "next", NULL };
static const char *const ARGS_THEME[]     = { "purple", "red", "green", "yellow", "light", "dark", "black", NULL };

static const CommandDef COMMANDS[] = {
    { "quit", NULL },        { "background", NULL },        { "debug", NULL },
    { "character", ARGS_CHARACTER }, { "wall", ARGS_DELETE }, { "entity", ARGS_DELETE },
    { "help", ARGS_HELP },   { "roll", NULL },              { "grid", NULL },
    { "gamemaster", NULL },  { "init", ARGS_INIT },         { "clear", NULL },
    { "theme", ARGS_THEME },
    /* arguments depend on the position and on the existing waypoints: see teleport_suggestions */
    { "teleport", NULL },    { "tp", NULL },
    { "save", NULL },        { "load", NULL },
};
#define NCOMMANDS ((int)(sizeof(COMMANDS) / sizeof(COMMANDS[0])))

static const CommandDef *find_command(const char *name) {
    for (int i = 0; i < NCOMMANDS; i++)
        if (strcmp(COMMANDS[i].name, name) == 0) return &COMMANDS[i];
    return NULL;
}

static void clear_suggestions(void) {
    for (int i = 0; i < C->suggestions.count; i++) free(C->suggestions.items[i]);
    da_clear(&C->suggestions);
}

static void suggest_if_prefix(const char *candidate, const char *prefix) {
    if (strncasecmp(candidate, prefix, strlen(prefix)) == 0) da_push(&C->suggestions, str_dup(candidate));
}

static void suggest_waypoints(const char *prefix) {
    for (int i = 0; i < gh->waypoints.count; i++) suggest_if_prefix(gh->waypoints.items[i].name, prefix);
}

/* tp / teleport: suggestions depend on the position of the word being completed
 *   tp <waypoint name | waypoint>
 *   tp waypoint <create | delete>
 *   tp waypoint delete <waypoint name>
 *   tp waypoint create <new name>      (nothing to suggest) */
static void teleport_suggestions(char **words, int pos, const char *prefix) {
    bool wp = pos >= 2 && (!strcasecmp(words[1], "waypoint") || !strcasecmp(words[1], "wp"));
    if (pos == 1) {
        suggest_if_prefix("waypoint", prefix);
        suggest_waypoints(prefix);
    } else if (pos == 2 && wp) {
        suggest_if_prefix("create", prefix);
        suggest_if_prefix("delete", prefix);
    } else if (pos == 3 && wp && (!strcasecmp(words[2], "delete") || !strcasecmp(words[2], "d"))) {
        suggest_waypoints(prefix);
    }
}

/* CommandCompleter.getSuggestions */
static void get_suggestions(const char *input) {
    clear_suggestions();
    if (!input[0]) return;

    char buf[CONSOLE_TEXT_MAX];
    snprintf(buf, sizeof(buf), "%s", input);
    char *words[64];
    int nwords = 0;
    for (char *tok = strtok(buf, " \t"); tok && nwords < 64; tok = strtok(NULL, " \t")) words[nwords++] = tok;
    if (nwords == 0) return;

    size_t len = strlen(input);
    bool trailing_space = input[len - 1] == ' ';
    /* index of the word being completed; a trailing space starts a new, empty one */
    int pos = trailing_space ? nwords : nwords - 1;
    if (pos > 0 && (!strcasecmp(words[0], "tp") || !strcasecmp(words[0], "teleport"))) {
        teleport_suggestions(words, pos, trailing_space ? "" : words[nwords - 1]);
        return;
    }
    if (trailing_space) {
        /* command + space: all arguments of the command */
        const CommandDef *cmd = find_command(words[0]);
        if (cmd && cmd->args)
            for (int i = 0; cmd->args[i]; i++) da_push(&C->suggestions, str_dup(cmd->args[i]));
        return;
    }
    if (nwords > 1) {
        const CommandDef *cmd = find_command(words[0]);
        const char *prefix = words[nwords - 1];
        if (cmd && cmd->args)
            for (int i = 0; cmd->args[i]; i++)
                if (strncmp(cmd->args[i], prefix, strlen(prefix)) == 0)
                    da_push(&C->suggestions, str_dup(cmd->args[i]));
    } else {
        for (int i = 0; i < NCOMMANDS; i++)
            if (strncmp(COMMANDS[i].name, words[0], strlen(words[0])) == 0)
                da_push(&C->suggestions, str_dup(COMMANDS[i].name));
    }
}

/* ---- log panel ------------------------------------------------------------------------------ */

void console_init(Console *c) {
    memset(c, 0, sizeof(*c));
    c->historyIndex = -1;
    c->suggestionIndex = -1;
    c->active = true;
    c->showLog = true;
    c->fontSize = 16;
}

void console_free(Console *c) {
    for (int i = 0; i < c->logs.count; i++) free(c->logs.items[i]);
    for (int i = 0; i < c->history.count; i++) free(c->history.items[i]);
    for (int i = 0; i < c->suggestions.count; i++) free(c->suggestions.items[i]);
    da_free(&c->logs);
    da_free(&c->history);
    da_free(&c->suggestions);
    da_free(&c->diceMarkers);
}

void console_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[2048];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    da_push(&C->logs, str_dup(buf));
    console_scroll_log(1); /* auto-scroll to the new message */
}

void console_scroll_log(int notches) {
    C->scrollLog += notches;
    if (C->scrollLog < 0) C->scrollLog = 0;
    /* may become negative with few messages: lines are then bottom-aligned */
    if (C->scrollLog > C->logs.count - CONSOLE_LOG_LINES) C->scrollLog = C->logs.count - CONSOLE_LOG_LINES;
}

void console_clear_logs(void) {
    for (int i = 0; i < C->logs.count; i++) free(C->logs.items[i]);
    da_clear(&C->logs);
}

void console_toggle_log(void) { C->showLog = !C->showLog; }

void console_change_font_size(int delta) {
    C->fontSize += delta;
    if (C->fontSize < 4) C->fontSize = 4;
}

int console_panel_height(void) {
    return 1 + C->fontSize * 8 / 5 * (C->showLog ? CONSOLE_LOG_LINES + 1 : 1);
}

Rect console_panel_rect(void) {
    if (!C->active) return (Rect){ 0, gh->winH, gh->winW, 0 };
    int h = console_panel_height();
    return (Rect){ 0, gh->winH - h, gh->winW, h };
}

/* ---- text editing ------------------------------------------------------------------------- */

void console_set_text(const char *text) {
    snprintf(C->text, sizeof(C->text), "%s", text);
    C->caret = (int)strlen(C->text);
}

void console_insert(const char *s) {
    size_t len = strlen(C->text), add = strlen(s);
    if (len + add >= sizeof(C->text)) return;
    memmove(C->text + C->caret + add, C->text + C->caret, len - (size_t)C->caret + 1);
    memcpy(C->text + C->caret, s, add);
    C->caret += (int)add;
}

static int prev_char(int pos) {
    if (pos <= 0) return 0;
    pos--;
    while (pos > 0 && ((unsigned char)C->text[pos] & 0xC0) == 0x80) pos--;
    return pos;
}

static int next_char(int pos) {
    int len = (int)strlen(C->text);
    if (pos >= len) return len;
    pos++;
    while (pos < len && ((unsigned char)C->text[pos] & 0xC0) == 0x80) pos++;
    return pos;
}

void console_key_edit(SDL_Keycode key) {
    int len = (int)strlen(C->text);
    switch (key) {
    case SDLK_LEFT:  C->caret = prev_char(C->caret); break;
    case SDLK_RIGHT: C->caret = next_char(C->caret); break;
    case SDLK_HOME:  C->caret = 0; break;
    case SDLK_END:   C->caret = len; break;
    case SDLK_BACKSPACE:
        if (C->caret > 0) {
            int p = prev_char(C->caret);
            memmove(C->text + p, C->text + C->caret, (size_t)(len - C->caret + 1));
            C->caret = p;
        }
        break;
    case SDLK_DELETE:
        if (C->caret < len) {
            int n = next_char(C->caret);
            memmove(C->text + C->caret, C->text + n, (size_t)(len - n + 1));
        }
        break;
    default: break;
    }
}

/* ---- history & completion ----------------------------------------------------------------------- */

void console_arrow_up(void) {
    if (C->history.count == 0) return;
    if (C->historyIndex == C->history.count) snprintf(C->currentInput, sizeof(C->currentInput), "%s", C->text);
    if (C->historyIndex > 0) {
        C->historyIndex--;
        console_set_text(C->history.items[C->historyIndex]);
    }
}

void console_arrow_down(void) {
    if (C->historyIndex < C->history.count - 1) {
        C->historyIndex++;
        console_set_text(C->history.items[C->historyIndex]);
    } else if (C->historyIndex == C->history.count - 1) {
        C->historyIndex = C->history.count;
        console_set_text(C->currentInput);
    }
    C->caret = (int)strlen(C->text);
}

void console_autocomplete(void) {
    const char *orig = C->originalInput;
    if (C->suggestionIndex == -1) {
        snprintf(C->originalInput, sizeof(C->originalInput), "%s", C->text);
        size_t len = strlen(orig);

        if (len > 0 && orig[len - 1] == ' ') {
            /* command followed by a space: list its arguments */
            get_suggestions(orig);
            if (C->suggestions.count > 1) {
                console_log("Possible arguments:");
                for (int i = 0; i < C->suggestions.count; i++) console_log("- %s", C->suggestions.items[i]);
            }
            C->suggestionIndex = 0;
            return;
        }

        get_suggestions(orig);
        const char *last_space = strrchr(orig, ' ');
        const char *last_word = last_space ? last_space + 1 : orig;
        for (int i = C->suggestions.count - 1; i >= 0; i--)
            if (strcmp(C->suggestions.items[i], last_word) == 0) {
                free(C->suggestions.items[i]);
                da_remove_at(&C->suggestions, i);
            }
        if (C->suggestions.count > 1) {
            console_log("Possible completions:");
            for (int i = 0; i < C->suggestions.count; i++) console_log("- %s", C->suggestions.items[i]);
        }
        C->suggestionIndex = 0;
    } else if (C->suggestions.count > 0) {
        C->suggestionIndex = (C->suggestionIndex + 1) % C->suggestions.count;
    }

    if (C->suggestions.count > 0) {
        const char *last_space = strrchr(orig, ' ');
        char buf[CONSOLE_TEXT_MAX];
        int base = last_space ? (int)(last_space - orig) + 1 : 0;
        snprintf(buf, sizeof(buf), "%.*s%s", base, orig, C->suggestions.items[C->suggestionIndex]);
        console_set_text(buf);
    }
}

void console_reset_suggestions(void) {
    C->originalInput[0] = '\0';
    clear_suggestions();
    C->suggestionIndex = -1;
}

void console_display_confirm(const char *text) {
    console_set_text(text);
    C->confirm = true;
}

/* ---- commands ------------------------------------------------------------------------------------ */

typedef struct {
    char *argv[64];
    int argc;
    char buf[CONSOLE_TEXT_MAX];
} Args;

static void split_args(const char *input, Args *a) {
    snprintf(a->buf, sizeof(a->buf), "%s", input);
    a->argc = 0;
    for (char *tok = strtok(a->buf, " \t"); tok && a->argc < 64; tok = strtok(NULL, " \t"))
        a->argv[a->argc++] = tok;
}

static void lower(char *s) { for (; *s; s++) *s = (char)tolower((unsigned char)*s); }

static bool parse_int_arg(const char *s, int *out) {
    if (!s) return false;
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s || *end) return false;
    *out = (int)v;
    return true;
}

static void cmd_theme(Args *a) {
    if (a->argc > 1) {
        gfx_change_theme(a->argv[1]);
        return;
    }
    console_log("The following themes are available:");
    for (int i = 0; i < gh->nthemes; i++) {
        const Theme *t = &gh->themes[i];
        console_log("%s: Primary ff%02x%02x%02x, Secondary ff%02x%02x%02x", t->name,
                    t->primary.r, t->primary.g, t->primary.b, t->secondary.r, t->secondary.g, t->secondary.b);
    }
}

static void cmd_clear(void) {
    for (int i = 0; i < C->history.count; i++) free(C->history.items[i]);
    da_clear(&C->history);
    C->historyIndex = 0;
    console_clear_logs();
}

static bool is_delete(const char *arg) { return strcmp(arg, "delete") == 0 || strcmp(arg, "d") == 0; }

static bool op_has_id(const Op *op, uint32_t id) {
    for (int i = 0; i < op->ids.count; i++)
        if (op->ids.items[i] == id) return true;
    return false;
}

static void submit_simple(OpType type) {
    Op op = { .type = type };
    ops_submit(&op);
}

/* :c / :w / :e delete remove every entity of a type (0 = characters, 1 = walls), like the Java version. */
static void delete_entities(int type) {
    Op op = { .type = OP_DELETE };
    for (int i = 0; i < gh->entities.count; i++) {
        const Entity *e = gh->entities.items[i];
        if ((type == 0) == (e->type == ENTITY_CHARACTER)) da_push(&op.ids, e->id);
    }
    if (op.ids.count > 0) ops_submit(&op);
    op_free(&op);
}

static void cmd_wall(Args *a) {
    if (a->argc > 1 && is_delete(a->argv[1])) delete_entities(1);
    else gfx_summon_wall();
}

static void cmd_entity(Args *a) {
    /* The original passes the wall type here as well. */
    if (a->argc > 1 && is_delete(a->argv[1])) delete_entities(1);
}

static void cmd_character(Args *a) {
    int size = 0, max_health = 0, ac = 0, speed = 0, initiative = 0;
    bool has[6] = { 0 }; /* size, maxhealth, ac, speed, initiative (= STAT_* bit order), icon */
    bool has_owner = false;
    char owner[PLAYER_NAME_MAX] = "";
    for (int i = 1; i < a->argc;) {
        char key[64];
        snprintf(key, sizeof(key), "%s", a->argv[i]);
        lower(key);
        const char *val = (i + 1 < a->argc) ? a->argv[i + 1] : NULL;
        if (strcmp(key, "d") == 0 || strcmp(key, "delete") == 0) {
            delete_entities(0);
            return;
        } else if (strcmp(key, "ic") == 0 || strcmp(key, "icon") == 0) {
            has[5] = true;
            i += 1;
            continue;
        } else if (strcmp(key, "ow") == 0 || strcmp(key, "owner") == 0) {
            if (!val) {
                console_log("Expected a player name or \"none\" after \"%s\"", a->argv[i]);
                return;
            }
            if (strcasecmp(val, "none") == 0) {
                owner[0] = '\0';
            } else if (savefmt_owner_valid(val)) {
                snprintf(owner, sizeof(owner), "%s", val);
            } else {
                console_log("Player names are 1 to %d bytes long, without spaces", PLAYER_NAME_MAX - 1);
                return;
            }
            has_owner = true;
            i += 2;
            continue;
        }
        int *target = NULL;
        int slot = -1;
        if      (!strcmp(key, "sz") || !strcmp(key, "size"))       { target = &size; slot = 0; }
        else if (!strcmp(key, "h")  || !strcmp(key, "maxhealth"))  { target = &max_health; slot = 1; }
        else if (!strcmp(key, "ac") || !strcmp(key, "armorclass")) { target = &ac; slot = 2; }
        else if (!strcmp(key, "s")  || !strcmp(key, "speed"))      { target = &speed; slot = 3; }
        else if (!strcmp(key, "in") || !strcmp(key, "initiative")) { target = &initiative; slot = 4; }
        if (target && parse_int_arg(val, target)) has[slot] = true;
        else if (target) console_log("Expected a number after \"%s\"", a->argv[i]);
        if (slot == 3 && has[3] && speed > MAX_SPEED) {
            console_log("Speed is capped at %d", MAX_SPEED);
            speed = MAX_SPEED;
        }
        i += 2;
    }

    if (gh->selectedEntityTiles.count == 0) {
        if (has_owner) console_log("Select the characters first");
        else gfx_spawn_character(size, max_health, ac, speed, initiative);
        return;
    }

    Op stats = { .type = OP_SET_STATS, .size = size, .maxHealth = max_health, .ac = ac, .speed = speed,
                 .initiative = initiative };
    for (int s = 0; s < 5; s++)
        if (has[s]) stats.statMask |= 1 << s;
    Op own = { .type = OP_SET_OWNER };
    snprintf(own.name, sizeof(own.name), "%s", owner);
    HexList icon_targets = { 0 };
    for (int i = 0; i < gh->selectedEntityTiles.count; i++) {
        Entity *e = entity_at(gh->selectedEntityTiles.items[i]);
        if (!e || e->type != ENTITY_CHARACTER || op_has_id(&stats, e->id)) continue;
        da_push(&stats.ids, e->id);
        da_push(&own.ids, e->id);
        if (has[5]) da_push(&icon_targets, e->tile);
    }
    if (stats.statMask && stats.ids.count > 0) ops_submit(&stats);
    if (has_owner && own.ids.count > 0 && ops_submit(&own) == OPR_OK) {
        if (owner[0]) console_log("%d character(s) now belong to %s", own.ids.count, owner);
        else console_log("%d character(s) no longer have an owner", own.ids.count);
    }
    if (has[5] && icon_targets.count > 0) gfx_request_file(ACTION_CHARACTER_ICON, &icon_targets, 0, 0, 0, 0, 0);
    da_free(&icon_targets);
    op_free(&stats);
    op_free(&own);
}

static void cmd_help(Args *a) {
    const char *arg = a->argc > 1 ? a->argv[1] : "";
    if (!strcmp(arg, "quit") || !strcmp(arg, ":q")) {
        console_log(":q  or quit # Quit the application - :q! also quits with unsaved changes");
    } else if (!strcmp(arg, "background") || !strcmp(arg, ":b")) {
        console_log(":b  or background [file.png] # Set a background image (opens a file dialog)");
    } else if (!strcmp(arg, "debug") || !strcmp(arg, ":d")) {
        console_log(":d  or debug # Toggle debug mode - No Arguments");
    } else if (!strcmp(arg, "character") || !strcmp(arg, ":c")) {
        console_log(":c  or character # Create a character, or edit the selected ones");
        console_log("  delete | size <-2..3> | maxhealth <n> | armorclass <n>");
        console_log("  speed <n> | initiative <n> | icon | owner <player|none>");
    } else if (!strcmp(arg, "wall") || !strcmp(arg, ":w")) {
        console_log(":w  or wall # Place walls on the selected tiles - delete: remove walls");
    } else if (!strcmp(arg, "entity") || !strcmp(arg, ":e")) {
        console_log(":e  or entity # Generic entity manipulation - delete");
    } else if (!strcmp(arg, "help") || !strcmp(arg, ":h")) {
        console_log(":h  or help [command] # Show available commands or details for one");
    } else if (!strcmp(arg, "roll") || !strcmp(arg, ":r")) {
        console_log(":r  or roll <dice> # Roll dice, e.g. roll 2d6 or roll 1d20+3");
    } else if (!strcmp(arg, "grid") || !strcmp(arg, ":g")) {
        console_log(":g  or grid # Toggle flat/pointy grid orientation (unstable)");
    } else if (!strcmp(arg, "gamemaster") || !strcmp(arg, ":gm")) {
        console_log(":gm or gamemaster # Toggle gamemaster mode (move without range limits)");
    } else if (!strcmp(arg, "init") || !strcmp(arg, ":i")) {
        console_log(":i  or init # Add all entities to the initiative order");
        console_log("  add | remove | clear | show | next");
    } else if (!strcmp(arg, "clear") || !strcmp(arg, ":cl")) {
        console_log(":cl or clear # Clear the command log and history");
    } else if (!strcmp(arg, "theme") || !strcmp(arg, ":t")) {
        console_log(":t  or theme [name] # List themes or choose one by name");
    } else if (!strcmp(arg, "teleport") || !strcmp(arg, "tp")) {
        console_log("tp  or teleport # Centre the view on a tile or waypoint");
        console_log("  tp <row> <col>  (also [row|col] as shown in debug mode)");
        console_log("  tp <waypoint>");
        console_log("  tp waypoint # list waypoints");
        console_log("  tp waypoint create <name> [row col] # default: selected tile, else screen centre");
        console_log("  tp waypoint delete <name>");
    } else if (!strcmp(arg, "save") || !strcmp(arg, ":save")) {
        console_log(":save [file] # Save the table as .tuhh");
        console_log("  without a file: save to the current file, or choose one in a dialog");
    } else if (!strcmp(arg, "load") || !strcmp(arg, ":load")) {
        console_log(":load [file] # Load a saved table (opens a dialog without a file)");
        console_log("  :load! [file] discards unsaved changes");
    } else {
        console_log("Available commands:");
        console_log(":q  or quit       # Quit the application");
        console_log(":b  or background # Set a background image");
        console_log(":d  or debug      # Toggle debug mode");
        console_log(":c  or character  # Create or edit a character");
        console_log(":w  or wall       # Place a wall");
        console_log(":e  or entity     # Generic entity manipulation (delete)");
        console_log(":r  or roll       # Roll dice (e.g. roll 2d6+1)");
        console_log(":g  or grid       # Change grid orientation (unstable)");
        console_log(":gm or gamemaster # Toggle gamemaster mode");
        console_log(":i  or init       # Manage initiative order");
        console_log(":h  or help       # Show available commands");
        console_log(":cl or clear      # Clear the command log and history");
        console_log(":t  or theme      # Show or choose a theme");
        console_log("tp  or teleport   # Jump to a tile or waypoint");
        console_log(":save [file]      # Save the table");
        console_log(":load[!] [file]   # Load a saved table");
    }
}

static void cmd_roll(Args *a) {
    for (int i = 0; i < C->diceMarkers.count; i++) marker_remove(C->diceMarkers.items[i]);
    da_clear(&C->diceMarkers);

    int num, sides, mod;
    if (a->argc < 2 || !dice_parse(a->argv[1], &num, &sides, &mod)) {
        console_log("Wrong dice syntax - use e.g. roll 2d6 or roll 1d20+3");
        return;
    }
    int *rolls = calloc((size_t)(num > 0 ? num : 1), sizeof(int));
    int total = dice_roll(num, sides, mod, rolls);
    for (int i = 0; i < num; i++) {
        Vec2 pos = { (int)((gh->winW - 50) * rand_unit() - 25), (int)((gh->winH - 50) * rand_unit() - 25) };
        Marker *m = marker_new_stat(rolls[i], pos, MARKER_DICE, false);
        marker_add(m);
        da_push(&C->diceMarkers, m);
    }
    Marker *m = marker_new_stat(total, (Vec2){ 0, 0 }, MARKER_DICERESULT, false);
    marker_add(m);
    da_push(&C->diceMarkers, m);
    free(rolls);
}

static void collect_selected_entities(EntityList *out) {
    for (int i = 0; i < gh->selectedEntityTiles.count; i++) {
        Entity *e = entity_at(gh->selectedEntityTiles.items[i]);
        if (!e) continue;
        bool dup = false;
        for (int k = 0; k < out->count; k++) if (out->items[k] == e) dup = true;
        if (!dup) da_push(out, e);
    }
}

/* Adds the entities that are not in the initiative yet, each with a fresh roll. */
static void roll_initiative(Entity **entities, int n) {
    Op op = { .type = OP_INIT_ADD };
    for (int i = 0; i < n; i++) {
        if (game_is_in_initiative(entities[i]) || op_has_id(&op, entities[i]->id)) continue;
        da_push(&op.ids, entities[i]->id);
        da_push(&op.values, rand_unit());
    }
    if (op.ids.count > 0) ops_submit(&op);
    op_free(&op);
}

static void cmd_init(Args *a) {
    if (a->argc < 2) {
        roll_initiative(gh->entities.items, gh->entities.count);
        return;
    }
    char sub[32];
    snprintf(sub, sizeof(sub), "%s", a->argv[1]);
    lower(sub);
    EntityList sel = { 0 };
    if (!strcmp(sub, "add") || !strcmp(sub, "a")) {
        collect_selected_entities(&sel);
        roll_initiative(sel.items, sel.count);
    } else if (!strcmp(sub, "remove") || !strcmp(sub, "r")) {
        collect_selected_entities(&sel);
        Op op = { .type = OP_INIT_REMOVE };
        for (int i = 0; i < sel.count; i++)
            if (game_is_in_initiative(sel.items[i])) da_push(&op.ids, sel.items[i]->id);
        if (op.ids.count > 0) ops_submit(&op);
        op_free(&op);
    } else if (!strcmp(sub, "clear") || !strcmp(sub, "c")) {
        submit_simple(OP_INIT_CLEAR);
    } else if (!strcmp(sub, "show") || !strcmp(sub, "s")) {
        submit_simple(OP_INIT_SHOW);
    } else if (!strcmp(sub, "next") || !strcmp(sub, "n")) {
        /* names the current turn, so a repeated or crossing "next" can't skip a turn */
        Op op = { .type = OP_INIT_NEXT, .id = gh->currentTurn ? gh->currentTurn->id : 0 };
        ops_submit(&op);
    }
    da_free(&sel);
}

/* ---- teleport & waypoints ------------------------------------------------------------- */

static Waypoint *find_waypoint(const char *name) {
    for (int i = 0; i < gh->waypoints.count; i++)
        if (strcasecmp(gh->waypoints.items[i].name, name) == 0) return &gh->waypoints.items[i];
    return NULL;
}

/* Parses tile coordinates from argv[first..]: "row col", "row,col", "row|col" or
 * "[row|col]" (the format debug mode shows). Everything from `first` on must be used. */
static bool parse_coords(const Args *a, int first, Hex *out) {
    if (first >= a->argc) return false;
    char buf[CONSOLE_TEXT_MAX] = { 0 };
    for (int i = first; i < a->argc; i++) {
        size_t len = strlen(buf);
        snprintf(buf + len, sizeof(buf) - len, "%s ", a->argv[i]);
    }
    for (char *p = buf; *p; p++)
        if (*p == '[' || *p == ']' || *p == '|' || *p == ',') *p = ' ';
    int row, col, used = 0;
    if (sscanf(buf, "%d %d %n", &row, &col, &used) != 2 || buf[used] != '\0') return false;
    *out = (Hex){ row, col };
    return true;
}

/* Tile used when a waypoint is created without coordinates: the most recently
 * selected tile, otherwise the tile in the centre of the window. */
static bool default_waypoint_tile(Hex *out) {
    if (gh->selectedTiles.count > 0) {
        *out = gh->selectedTiles.items[gh->selectedTiles.count - 1];
        return true;
    }
    return hex_find_closest((Vec2){ gh->winW / 2.0, gh->winH / 2.0 }, out);
}

static bool is_waypoint_keyword(const char *s) {
    return !strcasecmp(s, "waypoint") || !strcasecmp(s, "wp");
}

static void cmd_waypoint(Args *a) {
    /* tp waypoint                       -> list
     * tp waypoint create <name> [coords]
     * tp waypoint delete <name> */
    if (a->argc < 3) {
        if (gh->waypoints.count == 0) {
            console_log("No waypoints - create one with: tp waypoint create <name> [row col]");
            return;
        }
        console_log("Waypoints:");
        for (int i = 0; i < gh->waypoints.count; i++) {
            const Waypoint *w = &gh->waypoints.items[i];
            console_log("  %s [%d|%d]", w->name, w->tile.row, w->tile.col);
        }
        return;
    }
    char sub[32];
    snprintf(sub, sizeof(sub), "%s", a->argv[2]);
    lower(sub);
    bool create = !strcmp(sub, "create") || !strcmp(sub, "c");
    bool del = !strcmp(sub, "delete") || !strcmp(sub, "d");
    if (!create && !del) {
        console_log("Unknown waypoint action \"%s\" - use create or delete", a->argv[2]);
        return;
    }
    if (a->argc < 4) {
        console_log("Missing waypoint name");
        return;
    }
    const char *name = a->argv[3];

    if (del) {
        Waypoint *w = find_waypoint(name);
        if (!w) {
            console_log("No waypoint named \"%s\"", name);
            return;
        }
        Op op = { .type = OP_WAYPOINT_DELETE };
        snprintf(op.name, sizeof(op.name), "%s", w->name);
        if (ops_submit(&op) == OPR_OK) console_log("Deleted waypoint %s", op.name);
        return;
    }

    if (is_waypoint_keyword(name)) {
        console_log("\"%s\" can't be used as a waypoint name", name);
        return;
    }
    if (strlen(name) >= WAYPOINT_NAME_MAX) {
        console_log("Waypoint names can be at most %d characters long", WAYPOINT_NAME_MAX - 1);
        return;
    }
    Hex tile;
    if (a->argc > 4) {
        if (!parse_coords(a, 4, &tile)) {
            console_log("Expected coordinates as <row> <col>, e.g. tp waypoint create %s 3 7", name);
            return;
        }
    } else if (!default_waypoint_tile(&tile)) {
        console_log("Couldn't find a tile for the waypoint");
        return;
    }
    Waypoint *w = find_waypoint(name);
    bool existed = w != NULL;
    char shown[WAYPOINT_NAME_MAX];
    snprintf(shown, sizeof(shown), "%s", existed ? w->name : name);
    Op op = { .type = OP_WAYPOINT_SET, .tile = tile };
    snprintf(op.name, sizeof(op.name), "%s", name);
    if (ops_submit(&op) != OPR_OK) return;
    if (existed) console_log("Moved waypoint %s to [%d|%d]", shown, tile.row, tile.col);
    else console_log("Created waypoint %s at [%d|%d]", shown, tile.row, tile.col);
}

/* tp <row> <col> | tp <waypoint> | tp waypoint ... */
static void cmd_teleport(Args *a) {
    if (a->argc < 2) {
        console_log("Usage: tp <row> <col> | tp <waypoint> | tp waypoint create <name> [row col] | tp waypoint delete <name>");
        return;
    }
    if (is_waypoint_keyword(a->argv[1])) {
        cmd_waypoint(a);
        return;
    }
    Hex tile;
    if (parse_coords(a, 1, &tile)) {
        gfx_center_on(tile);
        return;
    }
    if (a->argc == 2) {
        Waypoint *w = find_waypoint(a->argv[1]);
        if (w) {
            gfx_center_on(w->tile);
            return;
        }
        console_log("No waypoint named \"%s\"", a->argv[1]);
        return;
    }
    console_log("Expected coordinates as <row> <col> or a waypoint name");
}

/* Everything after the first word, without surrounding blanks: file paths may contain spaces. */
static const char *rest_of_line(const char *input, char *out, size_t cap) {
    const char *p = input;
    while (isspace((unsigned char)*p)) p++;
    while (*p && !isspace((unsigned char)*p)) p++;
    while (isspace((unsigned char)*p)) p++;
    snprintf(out, cap, "%s", p);
    for (size_t n = strlen(out); n > 0 && isspace((unsigned char)out[n - 1]); n--) out[n - 1] = '\0';
    return out;
}

static bool ends_with(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && strcmp(s + a - b, suffix) == 0;
}

void console_command(const char *raw) {
    char input[CONSOLE_TEXT_MAX];
    snprintf(input, sizeof(input), "%s", raw);
    bool blank = true;
    for (const char *p = input; *p; p++) if (!isspace((unsigned char)*p)) blank = false;
    if (blank) return;

    /* history (skip duplicates of the last entry) */
    if (!(C->history.count > 1 && strcmp(input, C->history.items[C->history.count - 1]) == 0))
        da_push(&C->history, str_dup(input));
    C->historyIndex = C->history.count;
    console_set_text("");
    C->currentInput[0] = '\0';

    if (C->confirm) {
        /* Y/N answer to a confirmation prompt */
        char low[CONSOLE_TEXT_MAX];
        snprintf(low, sizeof(low), "%s", input);
        lower(low);
        C->confirm = false;
        gfx_spawn_character_confirmed(!(ends_with(low, "n") || low[0] == 'n'));
        return;
    }

    Args a;
    split_args(input, &a);
    if (a.argc == 0) return;
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "%s", a.argv[0]);
    lower(cmd);

    char rest[CONSOLE_TEXT_MAX];
    if      (!strcmp(cmd, "quit") || !strcmp(cmd, ":q"))        quit_command(false);
    else if (!strcmp(cmd, "quit!") || !strcmp(cmd, ":q!"))      quit_command(true);
    else if (!strcmp(cmd, "save") || !strcmp(cmd, ":save"))     save_command(rest_of_line(input, rest, sizeof(rest)));
    else if (!strcmp(cmd, "load") || !strcmp(cmd, ":load"))     load_command(rest_of_line(input, rest, sizeof(rest)), false);
    else if (!strcmp(cmd, "load!") || !strcmp(cmd, ":load!"))   load_command(rest_of_line(input, rest, sizeof(rest)), true);
    else if (!strcmp(cmd, "background") || !strcmp(cmd, ":b"))  gfx_set_background_image(a.argc > 1 ? a.argv[1] : NULL);
    else if (!strcmp(cmd, "debug") || !strcmp(cmd, ":d"))       gh->debugMode = !gh->debugMode;
    else if (!strcmp(cmd, "character") || !strcmp(cmd, ":c"))   cmd_character(&a);
    else if (!strcmp(cmd, "wall") || !strcmp(cmd, ":w"))        cmd_wall(&a);
    else if (!strcmp(cmd, "entity") || !strcmp(cmd, ":e"))      cmd_entity(&a);
    else if (!strcmp(cmd, "help") || !strcmp(cmd, ":h"))        cmd_help(&a);
    else if (!strcmp(cmd, "roll") || !strcmp(cmd, ":r"))        cmd_roll(&a);
    else if (!strcmp(cmd, "grid") || !strcmp(cmd, ":g"))        submit_simple(OP_GRID_ORIENTATION);
    else if (!strcmp(cmd, "gamemaster") || !strcmp(cmd, ":gm")) io_toggle_game_master();
    else if (!strcmp(cmd, "init") || !strcmp(cmd, ":i"))        cmd_init(&a);
    else if (!strcmp(cmd, "teleport") || !strcmp(cmd, "tp"))    cmd_teleport(&a);
    else if (!strcmp(cmd, "clear") || !strcmp(cmd, ":cl"))      cmd_clear();
    else if (!strcmp(cmd, "theme") || !strcmp(cmd, ":t"))       cmd_theme(&a);
    else console_log("Invalid input: \"%s\" - Try help for a list of commands", input);
}
