/* In-app console: log, input line, history, auto-completion and all commands
 * (main/Consol, main/ConsolPanel, main/CommandCompleter). */
#include "app.h"
#include "hud.h"
#include "settings.h"
#include "ops.h"
#include "party.h"
#include "savefmt.h"
#include "session.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <strings.h>

#define C (&gh->console)

/* ---- command completer ----------------------------------------------------------------- */

/* An argument and its short form (NULL = none); console_command accepts both. */
typedef struct {
    const char *name, *shortName;
} ArgDef;

typedef struct {
    const char *name, *shortName; /* "character" / ":c"; Tab completes either */
    const ArgDef *args;           /* { NULL } terminated, NULL = no arguments */
} CommandDef;

static const ArgDef ARGS_CHARACTER[] = {
    { "delete", "d" },     { "size", "sz" },       { "maxhealth", "h" }, { "speed", "s" },
    { "initiative", "in" }, { "maxstamina", "ms" }, { "stamina", "st" },  { "spend", NULL },
    { "recover", NULL },   { "icon", "ic" },       { "owner", "ow" },    { "name", "nm" },
    { "hp", NULL },        { "damage", "dmg" },    { "heal", NULL },     { "party", "pt" },
    { NULL, NULL },
};
static const ArgDef ARGS_ON_OFF[] = { { "on", NULL }, { "off", NULL }, { NULL, NULL } };
static const ArgDef ARGS_DELETE[] = { { "delete", "d" }, { NULL, NULL } };
static const ArgDef ARGS_HELP[]   = {
    { "quit", ":q" },        { "background", ":b" }, { "debug", ":d" },      { "character", ":c" },
    { "wall", ":w" },        { "entity", ":e" },     { "help", ":h" },       { "roll", ":r" },
    { "grid", ":g" },        { "gamemaster", ":gm" }, { "init", ":i" },      { "clear", ":cl" },
    { "theme", ":t" },       { "teleport", "tp" },   { "save", ":save" },    { "load", ":load" },
    { "host", ":host" },     { "unhost", ":unhost" }, { "join", ":join" },   { "leave", ":leave" },
    { "players", ":players" }, { "kick", ":kick" },  { "ban", ":ban" },      { "unban", ":unban" },
    { "claim", ":claim" },   { "hud", ":hud" },   { "party", ":party" },   { "testmode", ":tm" },
    { NULL, NULL },
};
static const ArgDef ARGS_INIT[]  = { { "add", "a" }, { "remove", "r" }, { "clear", "c" }, { "show", "s" },
                                     { "next", "n" }, { NULL, NULL } };
static const ArgDef ARGS_THEME[] = { { "purple", NULL }, { "red", NULL }, { "green", NULL }, { "yellow", NULL },
                                     { "light", NULL },  { "dark", NULL }, { "black", NULL }, { NULL, NULL } };

static const CommandDef COMMANDS[] = {
    { "quit", ":q", NULL },               { "background", ":b", NULL }, { "debug", ":d", NULL },
    { "character", ":c", ARGS_CHARACTER }, { "wall", ":w", ARGS_DELETE }, { "entity", ":e", ARGS_DELETE },
    { "help", ":h", ARGS_HELP },          { "roll", ":r", NULL },       { "grid", ":g", NULL },
    { "gamemaster", ":gm", NULL },        { "init", ":i", ARGS_INIT },  { "clear", ":cl", NULL },
    { "theme", ":t", ARGS_THEME },
    /* arguments depend on the position and on the existing waypoints: see teleport_suggestions */
    { "teleport", "tp", NULL },
    { "save", ":save", NULL },            { "load", ":load", NULL },
    { "host", ":host", NULL },            { "unhost", ":unhost", NULL }, { "join", ":join", NULL },
    { "leave", ":leave", NULL },          { "players", ":players", NULL }, { "kick", ":kick", NULL },
    { "ban", ":ban", NULL },              { "unban", ":unban", NULL },  { "claim", ":claim", NULL },  { "hud", ":hud", ARGS_ON_OFF },
    { "party", ":party", NULL },          { "testmode", ":tm", ARGS_ON_OFF },
};
#define NCOMMANDS ((int)(sizeof(COMMANDS) / sizeof(COMMANDS[0])))

static const CommandDef *find_command(const char *name) {
    for (int i = 0; i < NCOMMANDS; i++)
        if (!strcasecmp(COMMANDS[i].name, name) || !strcasecmp(COMMANDS[i].shortName, name)) return &COMMANDS[i];
    return NULL;
}

static bool has_prefix(const char *s, const char *prefix) {
    return s && strncasecmp(s, prefix, strlen(prefix)) == 0;
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
    if (pos > 0) {
        /* arguments of the command (all of them after a space); a short form such as "ms"
         * completes to its long one */
        const CommandDef *cmd = find_command(words[0]);
        const char *prefix = trailing_space ? "" : words[nwords - 1];
        if (cmd && cmd->args)
            for (const ArgDef *arg = cmd->args; arg->name; arg++)
                if (has_prefix(arg->name, prefix) || has_prefix(arg->shortName, prefix))
                    da_push(&C->suggestions, str_dup(arg->name));
    } else {
        /* the command itself: ":c" completes the short forms (:c, :cl, :claim), "c" the long ones */
        for (int i = 0; i < NCOMMANDS; i++) {
            const CommandDef *cmd = &COMMANDS[i];
            const char *hit = has_prefix(cmd->name, words[0])        ? cmd->name
                            : has_prefix(cmd->shortName, words[0]) ? cmd->shortName : NULL;
            if (hit) da_push(&C->suggestions, str_dup(hit));
        }
    }
}

/* ---- log panel ------------------------------------------------------------------------------ */

void console_init(Console *c) {
    memset(c, 0, sizeof(*c));
    c->historyIndex = -1;
    c->suggestionIndex = -1;
    c->active = false; /* hidden at start: ':' or Ctrl + Enter opens it */
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
    C->logScroll = 0; /* show the new message */
}

/* Positive notches scroll towards newer lines. How far back it can go depends on the wrapping at the current
 * width, so draw_console clamps the other end. */
void console_scroll_log(int notches) {
    C->logScroll -= notches;
    if (C->logScroll < 0) C->logScroll = 0;
}

void console_clear_logs(void) {
    for (int i = 0; i < C->logs.count; i++) free(C->logs.items[i]);
    da_clear(&C->logs);
    C->logScroll = 0;
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
        /* what is typed already (":c" next to ":cl" and ":claim") comes last: Tab first moves on,
         * and cycling returns to it */
        for (int i = 0; i < C->suggestions.count - 1; i++)
            if (strcasecmp(C->suggestions.items[i], last_word) == 0) {
                char *typed = C->suggestions.items[i];
                da_remove_at(&C->suggestions, i);
                da_push(&C->suggestions, typed);
                break;
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

static ConfirmFn confirm_fn;
static void *confirm_ctx;
static char confirm_question[CONSOLE_TEXT_MAX]; /* the input line starts with it; the answer follows */

bool console_confirm_busy(void) { return C->confirm; }

bool console_confirm(const char *question, ConfirmFn fn, void *ctx) {
    if (C->confirm) return false;
    console_set_text(question);
    snprintf(confirm_question, sizeof(confirm_question), "%s", question);
    C->confirm = true;
    confirm_fn = fn;
    confirm_ctx = ctx;
    return true;
}

/* ---- commands ------------------------------------------------------------------------------------ */

typedef struct {
    char *argv[64];
    int argc;
    char buf[CONSOLE_TEXT_MAX];
} Args;

static void args_split(const char *input, Args *a) {
    a->argc = split_args(input, a->buf, sizeof(a->buf), a->argv, 64);
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
        /* remembered for the next start; a theme shown by :load is not */
        if (gfx_change_theme(a->argv[1]) && !settings_set("theme", gh->themes[gh->currentTheme].name))
            console_log("Could not save the theme to %s", settings_path());
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
    int size = SIZE_NORMAL, max_health = 0, speed = 0, initiative = 0, max_stamina = 0;
    bool has[6] = { 0 }; /* size, maxhealth, speed, initiative, maxstamina (= STAT_* bit order), icon */
    bool has_owner = false;
    char owner[PLAYER_NAME_MAX] = "";
    char name[CHAR_NAME_MAX] = "";
    bool has_name = false;
    char party[CHAR_NAME_MAX] = "";
    bool has_party = false;
    struct { int mode, value; bool stamina; } res[8]; /* hp / stamina changes, in the order given */
    int nres = 0;
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
        if (strcmp(key, "nm") == 0 || strcmp(key, "name") == 0) {
            if (!val || !savefmt_char_name_valid(val)) {
                console_log("Names are 1 to %d bytes long", CHAR_NAME_MAX - 1);
                return;
            }
            snprintf(name, sizeof(name), "%s", val);
            has_name = true;
            i += 2;
            continue;
        }
        if (strcmp(key, "pt") == 0 || strcmp(key, "party") == 0) {
            /* the word after it is always the name; nothing after it: leave the party */
            if (val && !savefmt_char_name_valid(val)) {
                console_log("Names are 1 to %d bytes long", CHAR_NAME_MAX - 1);
                return;
            }
            snprintf(party, sizeof(party), "%s", val ? val : "");
            has_party = true;
            i += 2;
            continue;
        }
        bool is_st = !strcmp(key, "st") || !strcmp(key, "stamina") || !strcmp(key, "spend") || !strcmp(key, "recover");
        if (is_st || !strcmp(key, "hp") || !strcmp(key, "damage") || !strcmp(key, "dmg") || !strcmp(key, "heal")) {
            int v;
            bool change = strcmp(key, "hp") != 0 && strcmp(key, "st") != 0 && strcmp(key, "stamina") != 0;
            if (!parse_int_arg(val, &v) || (change && v < 0)) {
                console_log("Expected a number after \"%s\"", a->argv[i]);
                return;
            }
            if (v > SAVEFMT_MAX_HP || v < -SAVEFMT_MAX_HP) {
                console_log("Values go from -%d to %d", SAVEFMT_MAX_HP, SAVEFMT_MAX_HP);
                return;
            }
            if (nres == 8) {
                console_log("At most 8 HP or stamina changes per command");
                return;
            }
            res[nres].stamina = is_st;
            if (!strcmp(key, "spend")) res[nres].mode = STAMINA_SPEND;
            else res[nres].mode = change ? HEALTH_CHANGE : HEALTH_SET;
            res[nres].value = !strcmp(key, "damage") || !strcmp(key, "dmg") ? -v : v;
            nres++;
            i += 2;
            continue;
        }
        int *target = NULL;
        int slot = -1;
        if      (!strcmp(key, "sz") || !strcmp(key, "size"))       { target = &size; slot = 0; }
        else if (!strcmp(key, "h")  || !strcmp(key, "maxhealth"))  { target = &max_health; slot = 1; }
        else if (!strcmp(key, "s")  || !strcmp(key, "speed"))      { target = &speed; slot = 2; }
        else if (!strcmp(key, "ms") || !strcmp(key, "maxstamina")) { target = &max_stamina; slot = 4; }
        else if (!strcmp(key, "in") || !strcmp(key, "initiative")) { target = &initiative; slot = 3; }
        if (target && parse_int_arg(val, target)) has[slot] = true;
        else if (target) console_log("Expected a number after \"%s\"", a->argv[i]);
        if (slot == 0 && has[0] && (size < SIZE_MIN || size > SIZE_MAX_CLASS)) {
            console_log("Sizes go from -2 (Wee) to 4 (Gigantic)");
            return;
        }
        if (slot == 4 && has[4] && (max_stamina < 0 || max_stamina > SAVEFMT_MAX_HP)) {
            console_log("Max stamina goes from 0 to %d", SAVEFMT_MAX_HP);
            return;
        }
        if (slot == 2 && has[2] && speed > MAX_SPEED) {
            console_log("Speed is capped at %d", MAX_SPEED);
            speed = MAX_SPEED;
        }
        i += 2;
    }

    if (has_party && session_is_client()) { /* parties are the GM's; nothing is sent */
        console_log("%s", ops_result_text(OPR_GM_ONLY));
        return;
    }

    if (gh->selectedEntities.count == 0) {
        /* a bare "party" leaves a party: nothing to create */
        if (has_owner || nres > 0 || (has_party && !party[0])) console_log("Select the characters first");
        else gfx_spawn_character(size, max_health, speed, initiative, max_stamina, has_name ? name : NULL,
                                 has_party ? party : NULL);
        return;
    }

    Op stats = { .type = OP_SET_STATS, .size = size, .maxHealth = max_health, .speed = speed,
                 .initiative = initiative, .maxStamina = max_stamina };
    for (int s = 0; s < 5; s++)
        if (has[s]) stats.statMask |= 1 << s;
    Op own = { .type = OP_SET_OWNER };
    snprintf(own.name, sizeof(own.name), "%s", owner);
    int icon_targets = 0;
    for (int i = 0; i < gh->selectedEntities.count; i++) {
        Entity *e = entity_by_id(gh->selectedEntities.items[i]);
        if (!e || e->type != ENTITY_CHARACTER || op_has_id(&stats, e->id)) continue;
        da_push(&stats.ids, e->id);
        da_push(&own.ids, e->id);
        icon_targets++;
    }
    if (stats.statMask && stats.ids.count > 0) ops_submit(&stats);
    if ((has_name || has_party || nres > 0) && stats.ids.count == 0) console_log("Select the characters first");
    if (has_name && stats.ids.count > 0) {
        Op nm = { .type = OP_SET_NAME };
        snprintf(nm.charName, sizeof(nm.charName), "%s", name);
        for (int i = 0; i < stats.ids.count; i++) da_push(&nm.ids, stats.ids.items[i]);
        ops_submit(&nm);
        op_free(&nm);
    }
    if (has_party && stats.ids.count > 0) {
        Op pt = { .type = OP_SET_PARTY };
        snprintf(pt.party, sizeof(pt.party), "%s", party);
        for (int i = 0; i < stats.ids.count; i++) da_push(&pt.ids, stats.ids.items[i]);
        ops_submit(&pt);
        op_free(&pt);
    }
    for (int k = 0; k < nres && stats.ids.count > 0; k++) {
        if (res[k].stamina && !(stats.statMask & STAT_MAXSTAMINA && max_stamina > 0)) {
            const Entity *none = NULL; /* a character without a stamina stat */
            for (int i = 0; i < stats.ids.count && !none; i++) {
                const Entity *e = entity_by_id(stats.ids.items[i]);
                if (e && e->maxStamina <= 0) none = e;
            }
            if (none) {
                char nm[CHAR_NAME_MAX + 16];
                entity_display_name(none, nm, sizeof(nm));
                console_log("%s has no stamina; give it some with :c maxstamina <n>", nm);
                continue;
            }
        }
        Op h = { .type = res[k].stamina ? OP_SET_STAMINA : OP_SET_HEALTH, .healthMode = res[k].mode,
                 .value = res[k].value };
        for (int i = 0; i < stats.ids.count; i++) da_push(&h.ids, stats.ids.items[i]);
        ops_submit(&h);
        op_free(&h);
    }
    if (has_owner && own.ids.count > 0 && ops_submit(&own) == OPR_OK) {
        if (owner[0]) console_log("%d character(s) now belong to %s", own.ids.count, owner);
        else console_log("%d character(s) no longer have an owner", own.ids.count);
    }
    if (has[5] && icon_targets > 0) gfx_request_file(ACTION_CHARACTER_ICON, NULL, 0, 0, 0, 0, 0, NULL, NULL);
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
        console_log("  delete | size <-2..4> | maxhealth <n> | speed <n>");
        console_log("  sizes: -2 wee, -1 tiny, 0 small, 1 normal, 2 big, 3 huge, 4 gigantic");
        console_log("  initiative <n> | icon | owner <player|none>");
        console_log("  name <name> (quotes for spaces) | hp <n> | damage <n> | heal <n>");
        console_log("  maxstamina <n> | stamina <n> | spend <n> | recover <n>");
        console_log("  party <name> | party (last: leave the party) - GM only");
        console_log("  players: name, HP and stamina of your own characters; with nothing selected,");
        console_log("  asks the GM for a new character of your own");
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
        console_log("  While it runs, right click your character on its turn to move with move actions");
    } else if (!strcmp(arg, "clear") || !strcmp(arg, ":cl")) {
        console_log(":cl or clear # Clear the command log and history");
    } else if (!strcmp(arg, "theme") || !strcmp(arg, ":t")) {
        console_log(":t  or theme [name] # List themes or choose one by name (remembered for the next start)");
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
    } else if (!strcmp(arg, "host") || !strcmp(arg, ":host")) {
        console_log(":host [--direct] [name] # Host this table as GM (name defaults to GM)");
        console_log("  goes through the relay when one is set and copies the join code to the clipboard");
        console_log("  --direct: players connect to this computer (opens the router port)");
    } else if (!strcmp(arg, "unhost") || !strcmp(arg, ":unhost")) {
        console_log(":unhost # End the session; players keep a copy of the table");
    } else if (!strcmp(arg, "join") || !strcmp(arg, ":join")) {
        console_log(":join <code> <name> # Join the GM's table; your table is replaced by it");
    } else if (!strcmp(arg, "leave") || !strcmp(arg, ":leave")) {
        console_log(":leave # Leave the table; you keep a copy of it");
    } else if (!strcmp(arg, "players") || !strcmp(arg, ":players")) {
        console_log(":players # List the people at the table");
    } else if (!strcmp(arg, "kick") || !strcmp(arg, ":kick")) {
        console_log(":kick <name> # GM: remove a player from the table");
    } else if (!strcmp(arg, "ban") || !strcmp(arg, ":ban")) {
        console_log(":ban [name] # GM: remove a player and keep their name and address out; no name: list the bans");
    } else if (!strcmp(arg, "unban") || !strcmp(arg, ":unban")) {
        console_log(":unban <name> # GM: let a banned player join again");
    } else if (!strcmp(arg, "claim") || !strcmp(arg, ":claim")) {
        console_log(":claim # Ask the GM for the selected character (it must have no owner)");
    } else if (!strcmp(arg, "party") || !strcmp(arg, ":party")) {
        console_log(":party # List the parties (players: the parties of their own characters)");
    } else if (!strcmp(arg, "hud") || !strcmp(arg, ":hud")) {
        console_log(":hud [on|off] # Show or hide the HUD at the bottom of the window");
    } else if (!strcmp(arg, "testmode") || !strcmp(arg, ":tm")) {
        console_log(":tm or testmode [on|off] # GM: test mode for the whole table (no argument: switch)");
        console_log("  move actions work outside initiative and off-turn, and used actions or too little");
        console_log("  stamina don't stop them; they still use the action and spend stamina (not below 0)");
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
        console_log(":host / :join     # Play together: host as GM, or join with a code");
        console_log(":unhost / :leave  # End or leave the session");
        console_log(":players / :kick  # List or remove players");
        console_log(":ban / :unban     # Keep a player out of the session, or let them back in");
        console_log(":claim            # Ask the GM for the selected character");
        console_log(":party            # List the parties");
        console_log(":hud [on|off]     # Show or hide the HUD");
        console_log(":tm or testmode   # GM: test actions without their requirements");
    }
}

void console_show_dice(const int *rolls, int num, int total) {
    for (int i = 0; i < C->diceMarkers.count; i++) marker_remove(C->diceMarkers.items[i]);
    da_clear(&C->diceMarkers);
    for (int i = 0; i < num; i++) {
        Vec2 pos = { (int)((gh->winW - 50) * rand_unit() - 25), (int)((gh->winH - 50) * rand_unit() - 25) };
        Marker *m = marker_new_stat(rolls[i], pos, MARKER_DICE, false);
        marker_add(m);
        da_push(&C->diceMarkers, m);
    }
    Marker *m = marker_new_stat(total, (Vec2){ 0, 0 }, MARKER_DICERESULT, false);
    marker_add(m);
    da_push(&C->diceMarkers, m);
}

static void cmd_roll(Args *a) {
    int num, sides, mod;
    if (a->argc < 2 || !dice_parse(a->argv[1], &num, &sides, &mod)) {
        console_log("Wrong dice syntax - use e.g. roll 2d6 or roll 1d20+3");
        return;
    }
    if (session_roll(a->argv[1])) return; /* in a session the host rolls */
    int *rolls = calloc((size_t)(num > 0 ? num : 1), sizeof(int));
    int total = dice_roll(num, sides, mod, rolls);
    console_show_dice(rolls, num, total);
    free(rolls);
}

static void collect_selected_entities(EntityList *out) {
    for (int i = 0; i < gh->selectedEntities.count; i++) {
        Entity *e = entity_by_id(gh->selectedEntities.items[i]);
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
    size_t n = strlen(out);
    for (; n > 0 && isspace((unsigned char)out[n - 1]); n--) out[n - 1] = '\0';
    if (n >= 2 && out[0] == '"' && out[n - 1] == '"') { /* "my map" -> my map */
        memmove(out, out + 1, n - 2);
        out[n - 2] = '\0';
    }
    return out;
}

void console_command(const char *raw) {
    char input[CONSOLE_TEXT_MAX];
    snprintf(input, sizeof(input), "%s", raw);
    bool blank = true;
    for (const char *p = input; *p; p++) if (!isspace((unsigned char)*p)) blank = false;
    if (blank && !C->confirm) return; /* an empty answer to a question is yes */

    /* history (skip duplicates of the last entry) */
    if (!blank && !(C->history.count > 1 && strcmp(input, C->history.items[C->history.count - 1]) == 0))
        da_push(&C->history, str_dup(input));
    C->historyIndex = C->history.count;
    console_set_text("");
    C->currentInput[0] = '\0';

    if (C->confirm) {
        /* Y/N answer to a confirmation prompt */
        Answer ans = confirm_answer(confirm_question, input);
        if (ans == ANSWER_UNCLEAR) {
            console_log("Please answer y or n");
            console_set_text(confirm_question);
            return;
        }
        C->confirm = false;
        ConfirmFn fn = confirm_fn;
        confirm_fn = NULL;
        if (fn) fn(ans == ANSWER_YES, confirm_ctx);
        if (!C->confirm) session_prompt_free();
        return;
    }

    Args a;
    args_split(input, &a);
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
    else if (!strcmp(cmd, "gamemaster") || !strcmp(cmd, ":gm")) {
        if (session_is_client()) console_log("%s", ops_result_text(OPR_GM_ONLY));
        else io_toggle_game_master();
    }
    else if (!strcmp(cmd, "host") || !strcmp(cmd, ":host")) {
        bool direct = a.argc > 1 && !strcmp(a.argv[1], "--direct");
        int at = direct ? 2 : 1;
        if (a.argc > at + 1 || (a.argc > at && a.argv[at][0] == '-'))
            console_log("Usage: :host [--direct] [name]");
        else session_cmd_host(a.argc > at ? a.argv[at] : "GM", direct);
    }
    else if (!strcmp(cmd, "unhost") || !strcmp(cmd, ":unhost")) session_cmd_unhost();
    else if (!strcmp(cmd, "join") || !strcmp(cmd, ":join")) {
        if (a.argc != 3) console_log("Usage: :join <code> <name>");
        else session_cmd_join(a.argv[1], a.argv[2]);
    }
    else if (!strcmp(cmd, "leave") || !strcmp(cmd, ":leave"))   session_cmd_leave();
    else if (!strcmp(cmd, "players") || !strcmp(cmd, ":players")) session_cmd_players();
    else if (!strcmp(cmd, "kick") || !strcmp(cmd, ":kick")) {
        if (a.argc != 2) console_log("Usage: :kick <name>");
        else session_cmd_kick(a.argv[1]);
    }
    else if (!strcmp(cmd, "ban") || !strcmp(cmd, ":ban")) {
        if (a.argc > 2) console_log("Usage: :ban [name]");
        else session_cmd_ban(a.argc == 2 ? a.argv[1] : NULL);
    }
    else if (!strcmp(cmd, "unban") || !strcmp(cmd, ":unban")) {
        if (a.argc != 2) console_log("Usage: :unban <name>");
        else session_cmd_unban(a.argv[1]);
    }
    else if (!strcmp(cmd, "claim") || !strcmp(cmd, ":claim"))   session_cmd_claim();
    else if (!strcmp(cmd, "party") || !strcmp(cmd, ":party"))
        party_log(session_is_client() ? session_my_name() : NULL);
    else if (!strcmp(cmd, "hud") || !strcmp(cmd, ":hud")) {
        if (a.argc == 1) hud_set_visible(!hud_visible());
        else if (a.argc == 2 && !strcmp(a.argv[1], "on")) hud_set_visible(true);
        else if (a.argc == 2 && !strcmp(a.argv[1], "off")) hud_set_visible(false);
        else console_log("Usage: :hud [on|off]");
    }
    else if (!strcmp(cmd, "testmode") || !strcmp(cmd, ":tm")) {
        bool on = !gh->testMode;
        if (a.argc == 2 && !strcmp(a.argv[1], "on")) on = true;
        else if (a.argc == 2 && !strcmp(a.argv[1], "off")) on = false;
        if (a.argc > 2 || (a.argc == 2 && strcmp(a.argv[1], "on") && strcmp(a.argv[1], "off"))) {
            console_log("Usage: :tm [on|off]");
        } else if (session_is_client()) {
            console_log("%s", ops_result_text(OPR_GM_ONLY));
        } else if (on == gh->testMode) {
            console_log("Test mode is already %s", on ? "on" : "off");
        } else {
            Op tm = { .type = OP_SET_TEST_MODE, .value = on };
            ops_submit(&tm);
        }
    }
    else if (!strcmp(cmd, "init") || !strcmp(cmd, ":i"))        cmd_init(&a);
    else if (!strcmp(cmd, "teleport") || !strcmp(cmd, "tp"))    cmd_teleport(&a);
    else if (!strcmp(cmd, "clear") || !strcmp(cmd, ":cl"))      cmd_clear();
    else if (!strcmp(cmd, "theme") || !strcmp(cmd, ":t"))       cmd_theme(&a);
    else console_log("Invalid input: \"%s\" - Try help for a list of commands", input);
}
