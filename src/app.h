#ifndef PF_APP_H
#define PF_APP_H

/*
 * Shared state and module interfaces. The structure mirrors the original Java
 * packages:
 *   main/GraphicsHandler -> graphics.c   (window state, scene rendering)
 *   main/GameHandler     -> game.c       (tick, movement, initiative)
 *   main/IOHandler       -> input.c      (mouse & keyboard)
 *   main/Consol*, CommandCompleter -> console.c
 *   main/Toolbox, tools package      -> tools.c
 *   entities package, fx/Marker      -> entity.c
 *   fx/Hexagon, calc/Calc, calc/AStar -> hex.c, astar.c
 *   calc/Dice                        -> dice.c
 */

#include <SDL3/SDL.h>

#include "util.h"
#include "vk_renderer.h"

/* ==== hex grid ============================================================ */

/* Grid coordinate. Java stored this as Point(row, col). */
typedef struct { int row, col; } Hex;
typedef DA(Hex) HexList;

static inline bool hex_eq(Hex a, Hex b) { return a.row == b.row && a.col == b.col; }

int  hl_index(const HexList *l, Hex h);
bool hl_contains(const HexList *l, Hex h);
bool hl_remove(HexList *l, Hex h); /* removes first occurrence */
void hl_copy(HexList *dst, const HexList *src);

/* Set of tiles inside a square around a centre, stored as one bit per tile. */
typedef struct {
    Hex origin;     /* top-left tile (row/col of the square's first cell) */
    int side;       /* side length in tiles; 0 = empty set */
    uint8_t *bits;
} TileSet;

/* Empties the set and resizes it to cover centre +/- radius. */
void ts_reset(TileSet *s, Hex center, int radius);
void ts_free(TileSet *s);
bool ts_has(const TileSet *s, Hex h);
void ts_add(TileSet *s, Hex h); /* ignored outside the covered square */

typedef struct { int x, y, z; } Cube;

/* Named tile for the teleport command. */
#define WAYPOINT_NAME_MAX 32
typedef struct {
    char name[WAYPOINT_NAME_MAX];
    Hex tile;
} Waypoint;

Vec2 hex_center(Hex h);
void hex_corners(Hex h, Vec2 out[6]);
/* Is the hex part of the currently generated (visible) grid? */
bool hex_visible(Hex h);
void grid_visible_range(int *first_row, int *last_row, int *first_col, int *last_col);
/* Neighbour 0..5 (Java's getNeighbors order) without visibility checks. */
Hex  hex_neighbor_raw(Hex h, int index);
/* Java getNeighbor(hex, 1..6): returns false if the neighbour is not visible. */
bool hex_neighbor(Hex h, int number, Hex *out);
Cube hex_to_cube(int row, int col);
Hex  cube_to_hex(Cube c);
/* GraphicsHandler.findClosestHexagon: nearest visible hex centre within hexSize-1. */
bool hex_find_closest(Vec2 p, Hex *out);
/* Same, but also considers hexes outside the rendered part of the grid. */
bool hex_find_closest_any(Vec2 p, Hex *out);

/* ==== entities ============================================================== */

typedef enum { ENTITY_CHARACTER, ENTITY_WALL } EntityType;

enum { SIZE_TINY = -2, SIZE_SMALL = -1, SIZE_NORMAL = 0, SIZE_LARGE = 1, SIZE_HUGE = 2, SIZE_GARGANTUAN = 3 };

#define MAX_OCCUPIED 12

struct Marker;

typedef struct Entity {
    EntityType type;
    Texture *image;
    Hex tile;
    Vec2 location;
    int size, maxHealth, health, attack, ac, speed, initiative;
    struct Marker *debugMarker;
} Entity;

typedef DA(Entity *) EntityList;

Entity *entity_new_character(Texture *image, Hex tile, int size, int maxhealth, int ac, int speed, int initiative);
Entity *entity_new_wall(Texture *image, Hex tile);
/* Shallow copy sharing the image; registers its own debug marker if requested. */
Entity *entity_clone(const Entity *e, bool with_marker);
void    entity_free(Entity *e);
void    entity_set_image(Entity *e, Texture *image);
int     entity_occupied_tiles(const Entity *e, Hex out[MAX_OCCUPIED]);
int     character_occupied_tiles(Hex center, int size, Hex out[MAX_OCCUPIED]);
double  entity_draw_size(const Entity *e);
bool    entity_occupies(const Entity *e, Hex h);
/* GraphicsHandler.selectEntity: entity whose *main* tile is h. */
Entity *entity_at(Hex h);
/* GraphicsHandler.deleteEntities: 0 = characters, 1 = walls, else all. */
void    entities_delete_type(int type);

/* ==== markers =============================================================== */

enum { MARKER_COORDINATES = 0, MARKER_STAT = 1, MARKER_DICE = 2, MARKER_DICERESULT = 3, MARKER_TILE = 4 };

typedef struct Marker {
    int purpose;
    bool isDebug;
    Vec2 coords;
    Vec2 displayCoords;
    bool hasColor;
    Color color;
    double stat;
    bool visible;
    char suffix[16];
    Entity *attached;
    double debugValue;
} Marker;

typedef DA(Marker *) MarkerList;

Marker *marker_new(Vec2 location, int purpose, bool debug);
Marker *marker_new_stat(double stat, Vec2 coords, int purpose, bool debug);
void    marker_text(const Marker *m, char *buf, size_t n);
void    marker_update(Marker *m);
/* Adds/removes from the global marker list (removal also frees). */
void    marker_add(Marker *m);
void    marker_remove(Marker *m);

/* ==== pathfinding (calc/AStar) =================================================== */

/* Returns true and fills `path` (start..end) when a path exists. Like the Java
 * version an empty path is returned when start == end. */
bool astar_path(Hex start, Hex end, bool ignore_obstacles, HexList *path);
/* Like astar_path (with obstacles), but only finds paths of at most max_steps steps.
 * Much cheaper when the target is known to be close, e.g. inside a movement range. */
bool astar_path_within(Hex start, Hex end, int max_steps, HexList *path);
/* Characters can't be faster than this; keeps the movement range bounded. */
#define MAX_SPEED 1000
/* Reachable tiles within `speed` steps (moving may be NULL). */
void astar_range(Hex center, int speed, Entity *moving, TileSet *out);
int  hex_distance(Hex a, Hex b);

/* ==== dice ======================================================================= */

/* Parses "NdM" with an optional "+K"/"-K" modifier. */
bool dice_parse(const char *spec, int *num, int *sides, int *modifier);
/* Fills rolls[0..num-1]; returns the sum including the modifier. */
int  dice_roll(int num, int sides, int modifier, int *rolls);

/* ==== tools ===================================================================== */

enum { DRAG_MODE = 0, LENGTH_MODE = 1, AREA_MODE = 2, LINE_MODE = 3, CONE_MODE = 4 };

typedef struct {
    Texture *icon;
    int mode;
    int sort;
    Rect hitbox;
} Tool;

typedef struct {
    Tool tools[5];
    int ntools;
    int selected; /* index into tools, -1 = none */
    int size;
} Toolbox;

typedef struct {
    Hex origin;
    bool finished;
    Hex finishedPoint;
} Measure;

typedef struct {
    Hex origin;
    Vec2 a, b;
    bool hasLine;
} LineAttack;

typedef struct {
    Hex origin;
    Vec2 originPoint;
    double radius;
} AreaAttack;

typedef struct {
    Hex origin;
    Vec2 originPoint;
    double radius, angle, startAngle;
} ConeAttack;

void toolbox_init(Toolbox *tb, const char *resource_dir);
void toolbox_free(Toolbox *tb);
void toolbox_change_size(Toolbox *tb, int delta);

bool measure_line(Measure *m, Vec2 *a, Vec2 *b);
bool measure_finish(Measure *m); /* true => all measurements were cleared */
int  measure_length(const Measure *m);

bool line_get(LineAttack *l, Vec2 *a, Vec2 *b);
int  line_length(const LineAttack *l);
void line_attacked(LineAttack *l);

double area_radius(AreaAttack *a);
double area_radius_in_tiles(AreaAttack *a);
void   area_attacked(AreaAttack *a);

double cone_radius(ConeAttack *c);
double cone_radius_in_tiles(ConeAttack *c);
double cone_start_angle(ConeAttack *c);
void   cone_change_angle(ConeAttack *c, int notches);
void   cone_attacked(ConeAttack *c);

/* ==== console ===================================================================== */

#define CONSOLE_LOG_LINES 5
#define CONSOLE_TEXT_MAX  1024

typedef struct {
    DA(char *) logs;
    DA(char *) history;
    int historyIndex;
    char currentInput[CONSOLE_TEXT_MAX];

    char text[CONSOLE_TEXT_MAX];
    int caret; /* byte offset */
    float scrollX;

    bool confirm;

    DA(char *) suggestions;
    int suggestionIndex;
    char originalInput[CONSOLE_TEXT_MAX];

    int scrollLog;
    bool active;
    bool showLog;
    int fontSize;

    struct { struct Marker **items; int count, cap; } diceMarkers;
    bool swallowText;
} Console;

void console_init(Console *c);
void console_free(Console *c);
void console_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void console_scroll_log(int notches);
void console_clear_logs(void);
void console_toggle_log(void);
void console_change_font_size(int delta);
int  console_panel_height(void);
Rect console_panel_rect(void);
void console_command(const char *input);
void console_arrow_up(void);
void console_arrow_down(void);
void console_autocomplete(void);
void console_reset_suggestions(void);
void console_display_confirm(const char *text);
void console_set_text(const char *text);
void console_insert(const char *text);
void console_key_edit(SDL_Keycode key); /* caret movement / deletion */

/* ==== application state =========================================================== */

typedef struct { char name[16]; Color primary, secondary; } Theme;

typedef struct { Entity *entity; double value; } InitEntry;

typedef enum {
    ACTION_NONE,
    ACTION_BACKGROUND,
    ACTION_SPAWN_CHARACTERS,
    ACTION_WALLS,
    ACTION_CHARACTER_ICON,
} ActionKind;

typedef struct {
    ActionKind kind;
    HexList tiles;
    int size, maxHealth, ac, speed, initiative;
} PendingAction;

typedef struct App {
    SDL_Window *window;
    int winW, winH;      /* logical window size */
    float pixelScale;
    bool running;
    char resourceDir[512];
    const char *autoFile; /* test hook: skip file dialogs */
    Uint32 dialogEvent;

    /* GraphicsHandler */
    int hexSize, thickness, backgroundRows, backgroundCols;
    int gridOffsetX, gridOffsetY;
    bool isFlat, darkMode, debugMode;
    int zoomFactor;
    bool hasDragStart;
    Vec2 dragStart;
    Texture *backgroundImage;
    int drawWidth, drawHeight;

    Theme themes[8];
    int nthemes;
    int currentTheme;

    DA(Measure) measures;
    MarkerList markers;
    EntityList entities;
    HexList selectedTiles, selectedEntityTiles, entityPreviewTiles, attackTiles, path;
    TileSet entityRangeTiles;
    DA(Waypoint) waypoints;

    bool hasTileUnderMouse;
    Hex tileUnderMouse;
    Marker *totalLength;
    bool hasLineAttack, hasAreaAttack, hasConeAttack;
    LineAttack lineAttack;
    AreaAttack areaAttack;
    ConeAttack coneAttack;

    Toolbox toolbox;
    Console console;

    /* IOHandler */
    bool hasCurrentHex;
    Hex currentHexagon;
    int mode;
    Marker *mouseLog;
    EntityList clipboard;
    bool isRShiftDown, isShiftDown, isCtrlDown, isAltDown;
    bool LMBDown, RMBDown, ADown, DDown, SDown, WDown;
    bool hasSelectedEntity, gameMaster, mouseActive;
    Vec2 mousePos;

    /* GameHandler */
    int moveSpeed;
    DA(InitEntry) init;
    MarkerList initMarkers;
    Entity *currentTurn;
    int currentTurnIndex;

    /* pending asynchronous actions (replaces Java's blocking dialogs / AnswerWaiter) */
    PendingAction dialog;  /* waiting for the file dialog */
    PendingAction confirm; /* waiting for a Y/N answer in the console */
} App;

extern App *gh;

/* graphics.c */
bool gfx_init(App *app, const char *resource_dir);
void gfx_shutdown(void);
void gfx_draw_frame(const char *screenshot_path);
void gfx_change_theme(const char *name);
const Theme *gfx_theme(void);
void gfx_toggle_console(void);
void gfx_set_console_visibility(bool visible);
void gfx_grid_offset(int dx, int dy);
/* Moves the camera so that h is in the centre of the window. */
void gfx_center_on(Hex h);
void gfx_out_of_bounds_correction(void);
void gfx_zoom(int notches, Vec2 point, bool has_point);
void gfx_drag(Vec2 p);
void gfx_toggle_grid_orientation(void);
void gfx_add_entity_preview_tiles(void);
void gfx_request_file(ActionKind kind, const HexList *tiles, int size, int maxHealth, int ac, int speed, int initiative);
void gfx_handle_file_result(const char *path); /* path NULL = cancelled */
void gfx_spawn_character(int size, int maxhealth, int ac, int speed, int initiative);
void gfx_spawn_character_confirmed(bool yes);
void gfx_summon_wall(void);
void gfx_set_background_image(const char *path_or_null);
void gfx_update_tools(void);

/* game.c */
void game_tick(void);
void game_move_character(Hex target, Entity *c);
void game_initiative_list(Entity **entities, int n);
void game_remove_from_initiative(Entity *e);
void game_update_initiative_markers(void);
void game_toggle_show_initiative(void);
void game_next_turn(void);
void game_clear_initiative(void);
bool game_is_in_initiative(const Entity *e);

/* input.c */
void io_handle_event(const SDL_Event *e);
void io_update_total_length(void);
void io_toggle_game_master(void);

#endif
