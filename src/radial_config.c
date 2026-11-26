/* The radial menus: one tree of entries per menu. Edit these tables to change the menus; radial_game.c chooses
 * which menu a hex gets and handles the actions by id (see radial.h).
 *
 * "actions" is the menu of a character you control. "tile" and "character" (other hexes) are still placeholders, so
 * the framework can be tried out: neutral ids and labels, the placeholder icon, a submenu
 * with a nested submenu, a second submenu to switch to, an entry that needs something on the tile (disabled on an
 * empty one) and one only the GM sees. */
#include "radial.h"

#define ACTION(id_, label_, accent_, ...) \
    { .id = (id_), .icon = "placeholder", .label = (label_), .accent = (accent_), .kind = RADIAL_ACTION, __VA_ARGS__ }
#define SUBMENU(id_, label_, accent_, children_, ...)                                                            \
    { .id = (id_), .icon = "placeholder", .label = (label_), .accent = (accent_), .kind = RADIAL_SUBMENU,        \
      RADIAL_CHILDREN(children_), __VA_ARGS__ }
#define MENU(id_, center_, entries_) { (id_), (center_), (entries_), (int)(sizeof(entries_) / sizeof((entries_)[0])) }

static const RadialEntry NESTED[] = {
    ACTION("nested.1", "Nested 1", HUD_ACCENT_BLUE),
    ACTION("nested.2", "Nested 2", HUD_ACCENT_BLUE),
    ACTION("nested.3", "Nested 3", HUD_ACCENT_BLUE),
};

static const RadialEntry SUB_A[] = {
    ACTION("a.1", "Option A1", HUD_ACCENT_BLUE),
    ACTION("a.2", "Option A2", HUD_ACCENT_BLUE),
    SUBMENU("a.more", "More (A)", HUD_ACCENT_BLUE, NESTED),
    ACTION("a.3", "Option A3", HUD_ACCENT_BLUE),
    ACTION("a.4", "Option A4", HUD_ACCENT_BLUE, .enabled_if = "occupied"),
    ACTION("a.5", "Option A5", HUD_ACCENT_BLUE),
    ACTION("a.6", "Option A6", HUD_ACCENT_BLUE),
};

static const RadialEntry SUB_B[] = {
    ACTION("b.1", "Option B1", HUD_ACCENT_VIOLET),
    ACTION("b.2", "Option B2", HUD_ACCENT_VIOLET),
    ACTION("b.3", "Option B3", HUD_ACCENT_VIOLET),
    ACTION("b.4", "Option B4", HUD_ACCENT_VIOLET),
};

static const RadialEntry CENTER = ACTION("center", "Main action", HUD_ACCENT_GOLD);

/* any hex without a character */
static const RadialEntry TILE[] = {
    ACTION("action.1", "Action 1", HUD_ACCENT_RED),
    ACTION("action.2", "Action 2", HUD_ACCENT_GREEN),
    SUBMENU("menu.a", "Menu A", HUD_ACCENT_BLUE, SUB_A),
    ACTION("action.3", "Action 3 (needs something on the tile)", HUD_ACCENT_GOLD, .enabled_if = "occupied"),
    SUBMENU("menu.b", "Menu B", HUD_ACCENT_VIOLET, SUB_B),
    ACTION("action.gm", "GM action", HUD_ACCENT_GREY, .visible_if = "gm"),
};

/* a hex with a character on it */
static const RadialEntry CHARACTER[] = {
    ACTION("action.1", "Action 1", HUD_ACCENT_RED),
    SUBMENU("menu.a", "Menu A", HUD_ACCENT_BLUE, SUB_A),
    ACTION("action.2", "Action 2", HUD_ACCENT_GREEN),
    SUBMENU("menu.b", "Menu B", HUD_ACCENT_VIOLET, SUB_B),
    ACTION("action.gm", "GM action", HUD_ACCENT_GREY, .visible_if = "gm"),
};

/* ---- a character you control (radial_game.c): its actions by type, the Tales of Honor action colours ---- */

/* Move actions (moves.h): the ids are MOVE_ACTIONS' ids, each entry's rule of the same key says whether the
 * character can use it now. */
#define MOVE(id_, label_, accent_) ACTION(id_, label_, accent_, .enabled_if = id_)

static const RadialEntry MINOR[] = {
    MOVE("move.step", "Step: 1 tile, 1 stamina", HUD_ACCENT_MINOR),
    MOVE("move.sprint", "Sprint: your speed, 3 stamina, after another move action", HUD_ACCENT_MINOR),
};

static const RadialEntry MAJOR[] = {
    MOVE("move.walk", "Walk: half your speed", HUD_ACCENT_MAJOR),
    MOVE("move.run", "Run: your speed, 2 stamina", HUD_ACCENT_MAJOR),
    MOVE("move.crawl", "Crawl: half your speed, 2 stamina", HUD_ACCENT_MAJOR),
};

/* clockwise from the top */
static const RadialEntry ACTIONS[] = {
    SUBMENU("minor", "Minor action", HUD_ACCENT_MINOR, MINOR),
    ACTION("info", "Info", HUD_ACCENT_INFO),
    { .id = "mental", .icon = "placeholder", .label = "Mental action", .accent = HUD_ACCENT_MENTAL,
      .kind = RADIAL_SUBMENU }, /* no mental actions yet */
    SUBMENU("major", "Major action", HUD_ACCENT_MAJOR, MAJOR),
};

const RadialMenu RADIAL_MENUS[] = {
    MENU("tile", &CENTER, TILE),
    MENU("character", &CENTER, CHARACTER),
    MENU("actions", NULL, ACTIONS),
};
const int RADIAL_NMENUS = (int)(sizeof(RADIAL_MENUS) / sizeof(RADIAL_MENUS[0]));
