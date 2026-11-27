/* Window state, grid/camera handling, dialogs and scene rendering (main/GraphicsHandler). */
#include "app.h"
#include "charcreate.h"
#include "draw.h"
#include "hud_game.h"
#include "hud_theme.h"
#include "move_ui.h"
#include "target_ui.h"
#include "blocks.h"
#include "dodges.h"
#include "moves.h"
#include "radial_game.h"
#include "settings.h"
#include "image.h"
#include "ops.h"
#include "netmsg.h"
#include "session.h"
#include "textwrap.h"

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const Color DARK_SECONDARY = { 0x62, 0x69, 0x72, 255 };
static const Color LIGHT_SECONDARY = { 0, 0, 0, 255 };

/* recorded grid / debug-overlay vertices, see draw_grid_lines */
static DrCache grid_cache, debug_cache;

/* The log's wrapped lines, newest first: entry k is the k-th line from the bottom. */
typedef struct {
    int msg;
    TextSpan span;
} LogLine;
static DA(LogLine) log_lines; /* draw_log */

static Color text_secondary(void) { return gh->darkMode ? DARK_SECONDARY : LIGHT_SECONDARY; }

/* ---- setup ------------------------------------------------------------------------------- */

static void add_theme(const char *name, uint32_t primary, uint32_t secondary) {
    Theme *t = &gh->themes[gh->nthemes++];
    snprintf(t->name, sizeof(t->name), "%s", name);
    for (char *p = t->name; *p; p++) *p = (char)((*p >= 'A' && *p <= 'Z') ? *p + 32 : *p);
    t->primary = rgb_hex(primary);
    t->secondary = rgb_hex(secondary);
}

/* ---- UI scale (Ctrl + / Ctrl -) ---- */

/* One step makes the console text and the toolbox 1 px bigger; the HUD and the radial menu follow the console's
 * font size. The settings file keeps the steps from the normal size as "ui_scale". */
enum { UI_SCALE_MIN = -24, UI_SCALE_MAX = 64, TOOLBOX_MIN = 8 };
static int ui_scale_steps; /* from the normal size */

static bool ui_step(int delta) {
    int to = ui_scale_steps + delta;
    if (to < UI_SCALE_MIN || to > UI_SCALE_MAX) return false;
    if (delta < 0 && gh->toolbox.size <= TOOLBOX_MIN) return false;
    console_change_font_size(delta);
    toolbox_change_size(&gh->toolbox, delta);
    ui_scale_steps = to;
    return true;
}

void gfx_change_ui_scale(int delta) {
    if (!ui_step(delta)) return;
    char v[16];
    snprintf(v, sizeof(v), "%d", ui_scale_steps);
    if (!settings_set("ui_scale", v)) console_log("Could not save the UI scale to %s", settings_path());
}

/* The steps saved last time; a value that isn't a whole number in range is ignored. */
static void ui_scale_from_settings(void) {
    const char *v = settings_get("ui_scale");
    if (!v) return;
    char *end;
    long n = strtol(v, &end, 10);
    if (end == v || *end || n < UI_SCALE_MIN || n > UI_SCALE_MAX) return;
    int dir = n > 0 ? 1 : -1;
    while (ui_scale_steps != n && ui_step(dir)) {}
}

bool gfx_init(App *app, const char *resource_dir) {
    gh = app;
    snprintf(gh->resourceDir, sizeof(gh->resourceDir), "%s", resource_dir);
    gh->hexSize = 40;
    gh->thickness = 2;
    gh->backgroundRows = 20;
    gh->backgroundCols = 20;
    gh->isFlat = true;
    gh->darkMode = true;
    gh->zoomFactor = 1;
    gh->mouseActive = true;
    gh->mode = DRAG_MODE;
    gh->moveSpeed = 3;
    gh->running = true;
    gh->dialogEvent = SDL_RegisterEvents(1);

    add_theme("Purple", 0x4e0a56, 0xda80ff);
    add_theme("Red",    0x101820, 0xff0000);
    add_theme("Green",  0x101820, 0x1a7a4c);
    add_theme("Yellow", 0xFEE715, 0x101820);
    add_theme("Light",  0xD0D0D0, 0x000000);
    add_theme("Dark",   0x0D1219, 0x626972);
    add_theme("Black",  0x000000, 0x1c2022);

    console_init(&gh->console);
    toolbox_init(&gh->toolbox, resource_dir);
    hud_game_init(resource_dir);
    radial_game_init();
    ui_scale_from_settings();
    const char *theme = settings_get("theme"); /* the one chosen with :t last time */
    if (!theme || !gfx_change_theme(theme)) gfx_change_theme("light");
    return true;
}

void gfx_shutdown(void) {
    dr_cache_free(&grid_cache);
    dr_cache_free(&debug_cache);
    da_free(&log_lines);
    for (int i = 0; i < gh->entities.count; i++) entity_free(gh->entities.items[i]);
    for (int i = 0; i < gh->clipboard.count; i++) entity_free(gh->clipboard.items[i]);
    for (int i = 0; i < gh->markers.count; i++) free(gh->markers.items[i]);
    free(gh->totalLength);
    da_free(&gh->entities);
    da_free(&gh->clipboard);
    da_free(&gh->markers);
    da_free(&gh->measures);
    da_free(&gh->selectedTiles);
    da_free(&gh->selectedEntities);
    ts_free(&gh->entityRangeTiles);
    da_free(&gh->entityPreviewTiles);
    da_free(&gh->attackTiles);
    da_free(&gh->path);
    move_ui_cancel();
    target_ui_cancel();
    da_free(&gh->attackMarks);
    da_free(&gh->blocks);
    da_free(&gh->blockLocks);
    da_free(&gh->dodges);
    cc_shutdown(); /* the draft's picture */
    da_free(&gh->moveMode.path);
    da_free(&gh->init);
    da_free(&gh->initMarkers);
    da_free(&gh->waypoints);
    da_free(&gh->dialog.tiles);
    da_free(&gh->dialog.ids);
    da_free(&gh->confirm.tiles);
    vkr_texture_release(gh->backgroundImage);
    radial_game_shutdown();
    hud_game_shutdown();
    toolbox_free(&gh->toolbox);
    console_free(&gh->console);
}

/* ---- themes & console visibility ------------------------------------------------------------ */

/* A theme change fades the colours from the old theme to the new one over THEME_FADE_MS. */
#define THEME_FADE_MS 250
static Theme fade_from, fade_shown; /* fade_shown: this frame's colours while fading */
static Uint64 fade_start;
static bool fading, theme_set;

static Color mix(Color a, Color b, float f) {
    return (Color){ (uint8_t)lroundf(a.r + (b.r - a.r) * f), (uint8_t)lroundf(a.g + (b.g - a.g) * f),
                    (uint8_t)lroundf(a.b + (b.b - a.b) * f), (uint8_t)lroundf(a.a + (b.a - a.a) * f) };
}

/* Updates fade_shown for this frame; false once the fade is over. */
static bool theme_fade_step(void) {
    if (!fading) return false;
    float f = (float)(SDL_GetTicks() - fade_start) / THEME_FADE_MS;
    if (f >= 1) return fading = false;
    f = f * f * (3 - 2 * f); /* smoothstep */
    const Theme *to = &gh->themes[gh->currentTheme];
    fade_shown = *to;
    fade_shown.primary = mix(fade_from.primary, to->primary, f);
    fade_shown.secondary = mix(fade_from.secondary, to->secondary, f);
    return true;
}

const Theme *gfx_theme(void) { return fading ? &fade_shown : &gh->themes[gh->currentTheme]; }

bool gfx_change_theme(const char *name) {
    for (int i = 0; i < gh->nthemes; i++) {
        if (strcasecmp(gh->themes[i].name, name) == 0) {
            if (theme_set && i != gh->currentTheme) { /* the first theme (at start) appears without a fade */
                fade_from = *gfx_theme(); /* from what is on screen, also in the middle of a fade */
                fade_shown = fade_from;
                fade_start = SDL_GetTicks();
                fading = true;
            }
            gh->currentTheme = i;
            theme_set = true;
            return true;
        }
    }
    console_log("There is no such theme as \"%s\"", name);
    return false;
}

void gfx_toggle_console(void) {
    gh->console.active = !gh->console.active;
    gh->console.swallowText = false;
}

void gfx_set_console_visibility(bool visible) {
    if (gh->console.active != visible) gfx_toggle_console();
}

/* ---- camera ---------------------------------------------------------------------------------- */

static void update_background_size(void) {
    if (!gh->backgroundImage) return;
    double image_ratio = (double)vkr_texture_width(gh->backgroundImage) / vkr_texture_height(gh->backgroundImage);
    double format_ratio = (double)gh->backgroundCols / gh->backgroundRows;
    gh->drawHeight = gh->hexSize * gh->backgroundCols * 2;
    gh->drawWidth = (int)(gh->drawHeight * image_ratio * format_ratio);
}

void gfx_set_background_texture(Texture *t) {
    vkr_texture_retain(t);
    vkr_texture_release(gh->backgroundImage);
    gh->backgroundImage = t;
    update_background_size();
}

void gfx_grid_offset(int dx, int dy) {
    gh->gridOffsetX += dx;
    gh->gridOffsetY += dy;
    gh->hasTileUnderMouse = false;
}

void gfx_center_on(Hex h) {
    Vec2 c = hex_center(h);
    gfx_grid_offset((int)lround(gh->winW / 2.0 - c.x), (int)lround(gh->winH / 2.0 - c.y));
    gfx_out_of_bounds_correction();
}

/* Keeps the background image from being dragged out of view. */
void gfx_out_of_bounds_correction(void) {
    if (!gh->backgroundImage) return;
    update_background_size();
    if (gh->gridOffsetX > gh->winW)          gh->gridOffsetX = gh->winW;
    if (-gh->gridOffsetX > gh->drawWidth)    gh->gridOffsetX = -gh->drawWidth;
    if (gh->gridOffsetY > gh->winH)          gh->gridOffsetY = gh->winH;
    if (-gh->gridOffsetY > gh->drawHeight)   gh->gridOffsetY = -gh->drawHeight;
}

void gfx_zoom(int notches, Vec2 point, bool has_point) {
    int prev = gh->hexSize;
    double prev_zoom = prev / 40.0;
    if ((notches < 0 && gh->hexSize < 200) || (notches > 0 && gh->hexSize > 15)) {
        int step = gh->hexSize * gh->zoomFactor / 20;
        gh->hexSize -= notches * (step > 1 ? step : 1);
    } else {
        return; /* zoom limits */
    }
    double new_zoom = gh->hexSize / 40.0;
    if (has_point) {
        /* keep the point under the cursor fixed */
        double wx = (point.x - gh->gridOffsetX) / prev_zoom;
        double wy = (point.y - gh->gridOffsetY) / prev_zoom;
        gh->gridOffsetX = (int)point.x - (int)(wx * new_zoom);
        gh->gridOffsetY = (int)point.y - (int)(wy * new_zoom);
    }
    gh->thickness = gh->hexSize / 30 > 1 ? gh->hexSize / 30 : 1;
    gfx_out_of_bounds_correction();
}

void gfx_drag(Vec2 p) {
    if (!gh->hasDragStart) return;
    int dx = (int)p.x - (int)gh->dragStart.x;
    int dy = (int)p.y - (int)gh->dragStart.y;
    gfx_grid_offset(dx, dy);
    gfx_out_of_bounds_correction();
    gh->dragStart = p;
}

void gfx_toggle_grid_orientation(void) { gh->isFlat = !gh->isFlat; }

void gfx_add_entity_preview_tiles(void) {
    if (gh->selectedEntities.count == 0 || !gh->hasTileUnderMouse) return;
    Entity *sel = entity_by_id(gh->selectedEntities.items[0]);
    if (!sel || sel->type != ENTITY_CHARACTER) return;
    Hex tiles[MAX_OCCUPIED];
    int n = character_occupied_tiles(gh->tileUnderMouse, sel->size, tiles);
    for (int i = 0; i < n; i++) {
        if (hl_contains(&gh->entityPreviewTiles, tiles[i])) continue;
        da_push(&gh->entityPreviewTiles, tiles[i]);
        /* movement path preview when inside the range */
        if (ts_has(&gh->entityRangeTiles, tiles[i]))
            astar_path_within(sel->tile, tiles[i], sel->speed, sel, &gh->path);
    }
}

/* ---- file dialogs (asynchronous replacement for JFileChooser) ----------------------------------- */

static void SDLCALL dialog_callback(void *userdata, const char *const *filelist, int filter) {
    (void)userdata;
    (void)filter;
    if (!filelist) fprintf(stderr, "File dialog error: %s\n", SDL_GetError());
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = gh->dialogEvent;
    ev.user.data1 = (filelist && filelist[0]) ? SDL_strdup(filelist[0]) : NULL;
    SDL_PushEvent(&ev);
}

void gfx_request_file(ActionKind kind, const HexList *tiles, int size, int maxHealth, int speed, int initiative,
                      int maxStamina, const char *name, const char *party, const int *attr) {
    if (gh->dialog.kind != ACTION_NONE) {
        console_log("A file dialog is already open");
        return;
    }
    PendingAction *a = &gh->dialog;
    a->kind = kind;
    if (tiles) hl_copy(&a->tiles, tiles);
    else da_clear(&a->tiles);
    da_clear(&a->ids);
    if (kind == ACTION_CHARACTER_ICON) /* the selected characters */
        for (int i = 0; i < gh->selectedEntities.count; i++) da_push(&a->ids, gh->selectedEntities.items[i]);
    a->size = size;
    a->maxHealth = maxHealth;
    a->speed = speed;
    a->initiative = initiative;
    a->maxStamina = maxStamina;
    snprintf(a->name, sizeof(a->name), "%s", name ? name : "");
    snprintf(a->party, sizeof(a->party), "%s", party ? party : "");
    for (int i = 0; i < ATTR_COUNT; i++) a->attr[i] = attr ? attr[i] : 0;

    if (gh->autoFile) {
        gfx_handle_file_result(gh->autoFile);
        return;
    }
    static const SDL_DialogFileFilter save_filters[] = { { "Tales of Honor saves (*.tuhh)", "tuhh" } };
    static const SDL_DialogFileFilter filters[] = { { "PNG Images (*.png)", "png" } };
    if (kind == ACTION_SAVE)
        SDL_ShowSaveFileDialog(dialog_callback, NULL, gh->window, save_filters, 1, NULL);
    else if (kind == ACTION_LOAD)
        SDL_ShowOpenFileDialog(dialog_callback, NULL, gh->window, save_filters, 1, NULL, false);
    else
        SDL_ShowOpenFileDialog(dialog_callback, NULL, gh->window, filters, 1, NULL, false);
}

/* User images keep their pixels on the CPU: save files need them. */
static Texture *texture_from_image(Image *img) {
    Texture *t = vkr_texture_create_kept(img->w, img->h, img->px);
    img->px = NULL; /* now owned by the texture */
    image_free(img);
    return t;
}

void gfx_handle_file_result(const char *path) {
    PendingAction *a = &gh->dialog;
    ActionKind kind = a->kind;
    a->kind = ACTION_NONE;
    if (kind == ACTION_NONE) return;
    if (kind == ACTION_SAVE || kind == ACTION_LOAD) {
        save_handle_dialog(kind, path);
        return;
    }
    if (!path) {
        if (kind == ACTION_SPAWN_CHARACTERS) console_log("Character summon canceled");
        else if (kind == ACTION_WALLS) console_log("Wall summon canceled");
        return;
    }

    Image img;
    if (!image_load_png(path, &img)) {
        console_log("Could not load image \"%s\"", path);
        return;
    }

    switch (kind) {
    case ACTION_BACKGROUND: {
        Texture *t = texture_from_image(&img);
        Op op = { .type = OP_SET_BACKGROUND, .image = t };
        ops_submit(&op);
        vkr_texture_release(t);
        break;
    }
    case ACTION_SPAWN_CHARACTERS: {
        Image cut = image_cut_circle(&img);
        image_free(&img);
        if (session_is_client()) { /* players ask the GM */
            Image small = image_scale_to_fit(&cut, NET_CREATE_MAX_DIM);
            image_free(&cut);
            if (a->tiles.count == 1)
                session_request_character(&small, a->tiles.items[0], a->size, a->maxHealth, a->speed,
                                          a->initiative, a->maxStamina, a->name, a->attr);
            image_free(&small);
            break;
        }
        Texture *t = texture_from_image(&cut);
        Op op = { .type = OP_SPAWN };
        for (int i = 0; i < a->tiles.count; i++) {
            EntitySpec s = { .type = ENTITY_CHARACTER, .image = t, .tile = a->tiles.items[i], .size = a->size,
                             .maxHealth = a->maxHealth, .health = a->maxHealth, .speed = a->speed,
                             .initiative = a->initiative, .maxStamina = a->maxStamina, .stamina = a->maxStamina,
                             .evasion = EVASION_DEFAULT };
            memcpy(s.attr, a->attr, sizeof(s.attr));
            snprintf(s.name, sizeof(s.name), "%s", a->name);
            snprintf(s.party, sizeof(s.party), "%s", a->party);
            da_push(&op.specs, s);
        }
        ops_submit(&op);
        op_free(&op);
        vkr_texture_release(t);
        break;
    }
    case ACTION_CREATE_PICTURE: { /* the creation steps' picture (charcreate.h) */
        Image cut = image_cut_circle(&img);
        image_free(&img);
        Image small = { 0 };
        if (session_is_client()) small = image_scale_to_fit(&cut, NET_CREATE_MAX_DIM); /* for the request */
        Texture *t = texture_from_image(&cut);
        cc_set_picture(t, small); /* retains t, takes small */
        vkr_texture_release(t);
        break;
    }
    case ACTION_WALLS: {
        Image cut = image_cut_hex(&img, gh->isFlat);
        image_free(&img);
        Texture *t = texture_from_image(&cut);
        Op op = { .type = OP_SPAWN };
        for (int i = 0; i < a->tiles.count; i++)
            da_push(&op.specs, ((EntitySpec){ .type = ENTITY_WALL, .image = t, .tile = a->tiles.items[i] }));
        ops_submit(&op);
        op_free(&op);
        vkr_texture_release(t);
        break;
    }
    case ACTION_CHARACTER_ICON: {
        Image cut = image_cut_circle(&img);
        image_free(&img);
        Texture *t = texture_from_image(&cut);
        Op op = { .type = OP_SET_ICON, .image = t };
        for (int i = 0; i < a->ids.count; i++) {
            Entity *e = entity_by_id(a->ids.items[i]);
            if (e && e->type == ENTITY_CHARACTER) da_push(&op.ids, e->id); /* ids are unique */
        }
        if (op.ids.count > 0) ops_submit(&op);
        op_free(&op);
        vkr_texture_release(t);
        break;
    }
    default:
        image_free(&img);
        break;
    }
}

void gfx_set_background_image(const char *path) {
    if (!path) {
        gfx_request_file(ACTION_BACKGROUND, NULL, 0, 0, 0, 0, 0, NULL, NULL, NULL);
        return;
    }
    gh->dialog.kind = ACTION_BACKGROUND;
    gfx_handle_file_result(path);
}

static void spawn_confirmed(bool yes, void *ctx) {
    (void)ctx;
    PendingAction *c = &gh->confirm;
    if (c->kind == ACTION_SPAWN_CHARACTERS && yes)
        gfx_request_file(ACTION_SPAWN_CHARACTERS, &c->tiles, c->size, c->maxHealth, c->speed, c->initiative, c->maxStamina, c->name, c->party, c->attr);
    c->kind = ACTION_NONE;
}

void gfx_spawn_character(int size, int maxhealth, int speed, int initiative, int maxStamina, const char *name,
                         const char *party, const int *attr) {
    if (session_is_client() && gh->selectedTiles.count > 1) { /* players ask for one character at a time */
        console_log("Select one tile");
        return;
    }
    HexList tiles = { 0 };
    if (gh->selectedTiles.count == 1) {
        da_push(&tiles, gh->selectedTiles.items[0]);
    } else if (gh->selectedTiles.count > 1) {
        /* ask first (Java: AnswerWaiter on a background thread) */
        if (console_confirm_busy()) {
            console_log("Answer the open question first");
            return;
        }
        PendingAction *c = &gh->confirm;
        c->kind = ACTION_SPAWN_CHARACTERS;
        hl_copy(&c->tiles, &gh->selectedTiles);
        c->size = size;
        c->maxHealth = maxhealth;
        c->speed = speed;
        c->initiative = initiative;
        c->maxStamina = maxStamina;
        snprintf(c->name, sizeof(c->name), "%s", name ? name : "");
        snprintf(c->party, sizeof(c->party), "%s", party ? party : "");
        for (int i = 0; i < ATTR_COUNT; i++) c->attr[i] = attr ? attr[i] : 0;
        gfx_set_console_visibility(true);
        char msg[128];
        snprintf(msg, sizeof(msg), "are you sure you want to create [%d] Characters: (Y/N)", gh->selectedTiles.count);
        console_confirm(msg, spawn_confirmed, NULL);
        return;
    } else if (gh->hasTileUnderMouse) {
        da_push(&tiles, gh->tileUnderMouse);
    } else {
        Hex h;
        if (hex_find_closest((Vec2){ gh->winW / 2, gh->winH / 2 }, &h)) da_push(&tiles, h);
    }
    if (tiles.count == 0) console_log("Character summon canceled");
    else gfx_request_file(ACTION_SPAWN_CHARACTERS, &tiles, size, maxhealth, speed, initiative, maxStamina, name, party, attr);
    da_free(&tiles);
}

void gfx_summon_wall(void) {
    if (gh->selectedTiles.count == 0) {
        console_log("Select the tiles to place walls on first");
        return;
    }
    gfx_request_file(ACTION_WALLS, &gh->selectedTiles, 0, 0, 0, 0, 0, NULL, NULL, NULL);
}

/* Per-frame tool updates (done during painting in the Java version). */
void gfx_update_tools(void) {
    Vec2 a, b;
    if (gh->hasLineAttack && line_get(&gh->lineAttack, &a, &b)) line_attacked(&gh->lineAttack);
    if (gh->hasAreaAttack) area_attacked(&gh->areaAttack);
    if (gh->hasConeAttack && cone_radius(&gh->coneAttack) > gh->hexSize) cone_attacked(&gh->coneAttack);
}

/* ---- rendering ------------------------------------------------------------------------------------- */

static void draw_hex(Hex h, float thickness, Color c) {
    Vec2 pts[6];
    hex_corners(h, pts);
    dr_polygon(pts, 6, thickness, c);
}

static void draw_border(float x, float y, float w, float h, Color c) {
    dr_fill_rect(x, y, w, 1, c);
    dr_fill_rect(x, y + h - 1, w, 1, c);
    dr_fill_rect(x, y, 1, h, c);
    dr_fill_rect(x + w - 1, y, 1, h, c);
}

void gfx_character_rect(Hex tile, int size, float *x, float *y, float *d) {
    double hs = gh->hexSize;
    Entity tmp = { .type = ENTITY_CHARACTER, .size = size };
    double ds = entity_draw_size(&tmp);
    Vec2 c = hex_center(tile);
    double left;
    switch (size) {
    case SIZE_HUGE: left = c.x; break; /* centred on the triangle, right of the centre tile */
    default:        left = c.x - (ds * hs / 2); break;
    }
    *x = (float)left;
    *y = (float)(c.y - ds * hs / 2);
    *d = (float)(ds * hs);
}

void gfx_entity_rect(const Entity *e, float *x, float *y, float *d) {
    if (e->slots <= 1) {
        gfx_character_rect(e->tile, e->size, x, y, d);
        return;
    }
    /* k places on a ring inside the tile's inner circle (radius R), each as large as fits without overlapping:
     * neighbouring centres are 2 r sin(pi/k) apart with r = R - dk/2, which gives dk = 2 R s / (1 + s) */
    const double PI = 3.14159265358979323846;
    double hs = gh->hexSize, R = SQRT3 / 2 * hs;
    int k = e->slots > 7 ? 7 : e->slots; /* more than 7 (only Wee ones) overlap */
    double s = sin(PI / k), dk = 2 * R * s / (1 + s), r = R - dk / 2;
    double own = entity_draw_size(e) * hs, dd = own < dk ? own : dk;
    double a = PI + 2 * PI * (e->slot % k) / k; /* place 0 on the left, then clockwise */
    Vec2 c = hex_center(e->tile);
    *d = (float)dd;
    *x = (float)(c.x + r * cos(a) - dd / 2);
    *y = (float)(c.y + r * sin(a) - dd / 2);
}

static void draw_entities(void) {
    double hs = gh->hexSize;
    entity_layout();
    for (int i = 0; i < gh->entities.count; i++) {
        Entity *e = gh->entities.items[i];
        Vec2 c = hex_center(e->tile);
        double ds = entity_draw_size(e);
        if (e->type == ENTITY_CHARACTER) {
            float x, y, d;
            gfx_entity_rect(e, &x, &y, &d);
            e->location = (Vec2){ x, y };
            e->drawSize = d;
            dr_image(e->image, (int)e->location.x, (int)e->location.y, (int)d, (int)d);
        } else {
            e->location = (Vec2){ c.x - hs, c.y - (SQRT3 * hs / 2) };
            dr_image(e->image, (int)e->location.x, (int)e->location.y, (int)(ds * hs * 2 / SQRT3), (int)(ds * hs));
        }
        if (e->debugMarker) {
            e->debugMarker->coords = e->location;
            e->debugMarker->displayCoords = (Vec2){ e->tile.row, e->tile.col };
        }
    }
}

/* During a session: a ring in the owner's color around every owned character (thicker around ours). */
static void draw_owner_rings(void) {
    float th = gh->hexSize / 15.0f < 2 ? 2 : gh->hexSize / 15.0f;
    for (int i = 0; i < gh->entities.count; i++) {
        const Entity *e = gh->entities.items[i];
        uint32_t color;
        bool mine;
        if (e->type != ENTITY_CHARACTER || !session_owner_color(e->owner, &color, &mine)) continue;
        dr_stroke_oval((float)e->location.x, (float)e->location.y, e->drawSize, e->drawSize, mine ? th * 2 : th,
                       rgb_hex(color));
    }
}

/* ---- cached grid layers ------------------------------------------------------------------------- */

/* Everything the plain grid's geometry depends on, apart from its position. The
 * grid is periodic: moving the first visible hex by any number of rows/columns of
 * the same parity is a pure translation, so the cached hexes are reused while
 * panning and only rebuilt when the zoom, orientation, theme or the number of
 * visible rows/columns changes. */
typedef struct {
    int hexSize, thickness, rows, cols, rowParity, colParity;
    bool isFlat;
    Color color;
} GridKey;

/* The debug overlay shows absolute coordinates, so it is only reused while the view is unchanged. */
typedef struct {
    int hexSize, gridOffsetX, gridOffsetY, r0, r1, c0, c1;
    bool isFlat;
    float pixelScale;
    Color color;
    unsigned fontGeneration;
} DebugKey;

static GridKey grid_key;
static DebugKey debug_key;

static void draw_grid_lines(int r0, int r1, int c0, int c1, float th, Color color) {
    GridKey key;
    memset(&key, 0, sizeof(key)); /* padding takes part in memcmp */
    key.hexSize = gh->hexSize;
    key.thickness = gh->thickness;
    key.rows = r1 - r0;
    key.cols = c1 - c0;
    key.rowParity = r0 & 1;
    key.colParity = c0 & 1;
    key.isFlat = gh->isFlat;
    key.color = color;

    Vec2 origin = hex_center((Hex){ r0, c0 });
    if (!grid_cache.valid || memcmp(&key, &grid_key, sizeof(key)) != 0) {
        dr_cache_begin();
        for (int r = r0; r <= r1; r++)
            for (int c = c0; c <= c1; c++) draw_hex((Hex){ r, c }, th, color);
        dr_cache_end(&grid_cache, (float)origin.x, (float)origin.y);
        grid_key = key;
        return; /* the recorded vertices are already part of this frame */
    }
    dr_cache_draw(&grid_cache, (float)origin.x, (float)origin.y);
}

static void draw_debug_labels(int r0, int r1, int c0, int c1) {
    int hs = gh->hexSize;
    DebugKey key;
    memset(&key, 0, sizeof(key));
    key.hexSize = hs;
    key.gridOffsetX = gh->gridOffsetX;
    key.gridOffsetY = gh->gridOffsetY;
    key.r0 = r0; key.r1 = r1; key.c0 = c0; key.c1 = c1;
    key.isFlat = gh->isFlat;
    key.pixelScale = gh->pixelScale;
    key.color = text_secondary();
    key.fontGeneration = font_atlas_generation();

    if (debug_cache.valid && memcmp(&key, &debug_key, sizeof(key)) == 0) {
        dr_cache_draw(&debug_cache, 0, 0);
        return;
    }
    dr_cache_begin();
    for (int r = r0; r <= r1; r++) {
        for (int c = c0; c <= c1; c++) {
            Vec2 p = hex_center((Hex){ r, c });
            /* culling is off while recording, so skip labels that are clearly off screen */
            if (p.x < -2 * hs || p.y < -2 * hs || p.x > gh->winW + 2 * hs || p.y > gh->winH + 2 * hs) continue;
            dr_fill_oval((int)p.x - hs / 10, (int)p.y - hs / 10, hs / 10, hs / 10, COLOR_RED);
            char buf[32];
            snprintf(buf, sizeof(buf), "[%d|%d]", r, c);
            int digits = (int)strlen(buf);
            dr_text(FONT_BOLD, hs / 3, (int)p.x - hs / 3 - 2 * digits, (int)p.y + hs / 5, key.color, buf);
        }
    }
    dr_cache_end(&debug_cache, 0, 0);
    /* glyphs rasterised while recording may have filled the atlas; then the key won't match next frame */
    key.fontGeneration = font_atlas_generation();
    debug_key = key;
}

/* The tiles attacked this round (attacks.h): an arrow from the tile's centre in the attack's direction (towards the
 * tile or tiles the direction names), a ring for neutral. */
static void draw_attack_marks(void) {
    float hs = (float)gh->hexSize, th = (float)gh->thickness;
    Color col = HUD_THEME.accent[HUD_ACCENT_RED];
    for (int i = 0; i < gh->attackMarks.count; i++) {
        const AttackMark *m = &gh->attackMarks.items[i];
        if (!hex_visible(m->tile)) continue;
        Vec2 c = hex_center(m->tile);
        if (m->dir == DIR_NEUTRAL) {
            float r = hs * 0.3f;
            dr_stroke_oval(c.x - r, c.y - r, 2 * r, 2 * r, th * 2.5f, col);
            continue;
        }
        Hex to[6];
        int n = attack_dir_tiles(m->from, m->tile, (AttackDir)m->dir, to);
        if (n == 0) continue;
        double ux = 0, uy = 0; /* towards the middle of the tiles it points at */
        for (int k = 0; k < n; k++) {
            Vec2 p = hex_center(to[k]);
            ux += p.x - c.x;
            uy += p.y - c.y;
        }
        double len = sqrt(ux * ux + uy * uy);
        if (len <= 0) continue;
        ux /= len;
        uy /= len;
        float tail = hs * 0.35f, head = hs * 0.55f, wing = hs * 0.25f;
        float ex = (float)(c.x + ux * head), ey = (float)(c.y + uy * head);
        dr_line((float)(c.x - ux * tail), (float)(c.y - uy * tail), ex, ey, th * 2.5f, col);
        for (int s = -1; s <= 1; s += 2) { /* the head: two strokes back at +-35 degrees */
            double a = atan2(-uy, -ux) + s * 0.6;
            dr_line(ex, ey, (float)(ex + cos(a) * wing), (float)(ey + sin(a) * wing), th * 2.5f, col);
        }
    }
}

/* The blocked tiles (blocks.h): a blue hexagon inside the tile and, at its bottom, the combined directions and Block
 * Value, e.g. "UL 2" (neutral is always blocked). */
static void draw_block_marks(void) {
    float hs = (float)gh->hexSize, th = (float)gh->thickness;
    Color col = HUD_THEME.accent[HUD_ACCENT_BLUE];
    for (int i = 0; i < gh->blocks.count; i++) {
        Hex tile = gh->blocks.items[i].tile;
        bool first = true; /* one mark per tile */
        for (int k = 0; k < i && first; k++) first = !hex_eq(gh->blocks.items[k].tile, tile);
        if (!first || !hex_visible(tile)) continue;
        int dirs, bv;
        block_on_tile(tile, &dirs, &bv);
        Vec2 c = hex_center(tile), corners[6];
        hex_corners(tile, corners);
        for (int k = 0; k < 6; k++)
            corners[k] = (Vec2){ c.x + (corners[k].x - c.x) * 0.78, c.y + (corners[k].y - c.y) * 0.78 };
        dr_polygon(corners, 6, th * 2.5f, col);
        char label[16];
        snprintf(label, sizeof(label), "%s%s%s %d", dirs & DIR_BIT(DIR_UP) ? "U" : "",
                 dirs & DIR_BIT(DIR_LEFT) ? "L" : "", dirs & DIR_BIT(DIR_RIGHT) ? "R" : "", bv);
        float size = hs * 0.32f, w = dr_text_width(FONT_BOLD, size, label);
        float x = (float)c.x - w / 2, y = (float)c.y + hs * 0.62f;
        dr_fill_round_rect(x - 3, y - size * 0.85f, w + 6, size * 1.1f, 4, 4, col);
        dr_text(FONT_BOLD, size, x, y, rgb_hex(0xFFFFFF), label);
    }
}

/* The tiles a dodger left (dodges.h): a green hexagon inside the tile, an arrow toward its step and, on the other
 * side, its Dodge Value, e.g. "DV 6". */
static void draw_dodge_marks(void) {
    float hs = (float)gh->hexSize, th = (float)gh->thickness;
    Color col = HUD_THEME.accent[HUD_ACCENT_GREEN];
    for (int i = 0; i < gh->dodges.count; i++) {
        const DodgeRec *d = &gh->dodges.items[i];
        for (int t = 0; t < d->nfrom; t++) {
            Hex tile = d->from[t];
            if (!hex_visible(tile)) continue;
            Vec2 c = hex_center(tile), corners[6], to = hex_center(hex_neighbor_raw(tile, d->step));
            hex_corners(tile, corners);
            for (int k = 0; k < 6; k++)
                corners[k] = (Vec2){ c.x + (corners[k].x - c.x) * 0.62, c.y + (corners[k].y - c.y) * 0.62 };
            dr_polygon(corners, 6, th * 2.5f, col);
            double ux = to.x - c.x, uy = to.y - c.y, len = sqrt(ux * ux + uy * uy);
            if (len > 0) {
                ux /= len;
                uy /= len;
            }
            if (len > 0) {
                float head = hs * 0.5f, wing = hs * 0.2f;
                float ex = (float)(c.x + ux * head), ey = (float)(c.y + uy * head);
                dr_line((float)c.x, (float)c.y, ex, ey, th * 2.5f, col);
                for (int s = -1; s <= 1; s += 2) {
                    double a = atan2(-uy, -ux) + s * 0.6;
                    dr_line(ex, ey, (float)(ex + cos(a) * wing), (float)(ey + sin(a) * wing), th * 2.5f, col);
                }
            }
            char label[16];
            snprintf(label, sizeof(label), "DV %d", d->value);
            float size = hs * 0.32f, w = dr_text_width(FONT_BOLD, size, label);
            float x = (float)(c.x - ux * hs * 0.42) - w / 2, y = (float)(c.y - uy * hs * 0.42) + size * 0.3f;
            dr_fill_round_rect(x - 3, y - size * 0.85f, w + 6, size * 1.1f, 4, 4, col);
            dr_text(FONT_BOLD, size, x, y, rgb_hex(0xFFFFFF), label);
        }
    }
}

static void draw_grid(void) {
    const Theme *t = gfx_theme();
    float th = (float)gh->thickness;
    dr_set_alpha(0.5f); /* grid transparency */

    int r0, r1, c0, c1;
    grid_visible_range(&r0, &r1, &c0, &c1);
    draw_grid_lines(r0, r1, c0, c1, th, t->secondary);

    /* other people's presence, under our own highlights */
    dr_set_alpha(1.0f);
    session_draw_presence();
    dr_set_alpha(0.5f);

    if (gh->hasTileUnderMouse && !gh->hasDragStart)
        draw_hex(gh->tileUnderMouse, th + (gh->mouseActive ? 2 : 4), t->secondary);

    for (int i = 0; i < gh->selectedTiles.count; i++) draw_hex(gh->selectedTiles.items[i], th + 2, COLOR_RED);

    for (int i = 0; i < gh->selectedEntities.count; i++) {
        Entity *e = entity_by_id(gh->selectedEntities.items[i]);
        if (!e) continue;
        Hex tiles[MAX_OCCUPIED];
        int n = entity_occupied_tiles(e, tiles);
        for (int k = 0; k < n; k++) draw_hex(tiles[k], th + 2, COLOR_BLUE);
        if (e->type == ENTITY_CHARACTER && e->slots > 1) /* on a shared tile: which one */
            dr_stroke_oval((float)e->location.x - 2, (float)e->location.y - 2, e->drawSize + 4, e->drawSize + 4, th + 2,
                           COLOR_BLUE);
    }
    /* movement range: only the rendered tiles need drawing */
    if (gh->entityRangeTiles.side > 0)
        for (int r = r0; r <= r1; r++)
            for (int c = c0; c <= c1; c++) {
                Hex h = { r, c };
                if (ts_has(&gh->entityRangeTiles, h) && !hl_contains(&gh->entityPreviewTiles, h))
                    draw_hex(h, th + 2, COLOR_GREEN);
            }
    for (int i = 0; i < gh->attackTiles.count; i++)
        if (!hl_contains(&gh->entityPreviewTiles, gh->attackTiles.items[i]))
            draw_hex(gh->attackTiles.items[i], th + 2, COLOR_MAGENTA);

    /* choosing where a move action goes: its range in the colour of the action's slot */
    move_ui_update();
    if (gh->moveMode.active) {
        Entity *me = entity_by_id(gh->moveMode.id);
        HudAccent acc = me && move_slot(me, (MoveAction)gh->moveMode.action) == SLOT_MINOR ? HUD_ACCENT_MINOR
                                                                                           : HUD_ACCENT_MAJOR;
        dr_set_alpha(0.9f);
        for (int r = r0; r <= r1; r++)
            for (int c = c0; c <= c1; c++) {
                Hex h = { r, c };
                if (ts_has(&gh->moveMode.range, h) && (!me || !hex_eq(h, me->tile)))
                    draw_hex(h, th + 2, HUD_THEME.accentLight[acc]);
            }
        if (gh->moveMode.path.count > 0)
            draw_hex(gh->moveMode.path.items[gh->moveMode.path.count - 1], th + 4, HUD_THEME.accent[acc]);
        dr_set_alpha(0.5f);
    }

    /* choosing where an attack or a block goes: its range in the colour of its slot, the tile under the pointer (or the
     * one its ring is open on) stronger */
    target_ui_update();
    if (gh->targetMode.active) {
        HudAccent acc = target_ui_slot() == SLOT_MINOR ? HUD_ACCENT_MINOR : HUD_ACCENT_MAJOR;
        dr_set_alpha(0.9f);
        for (int r = r0; r <= r1; r++)
            for (int c = c0; c <= c1; c++) {
                Hex h = { r, c };
                if (ts_has(&gh->targetMode.range, h)) draw_hex(h, th + 2, HUD_THEME.accentLight[acc]);
            }
        Hex focus = gh->targetMode.hasTarget ? gh->targetMode.target : gh->tileUnderMouse;
        if ((gh->targetMode.hasTarget || gh->hasTileUnderMouse) && ts_has(&gh->targetMode.range, focus))
            draw_hex(focus, th + 4, HUD_THEME.accent[acc]);
        dr_set_alpha(0.5f);
    }

    /* placing a new character (charcreate.h): its picture under the mouse, its tiles green if it fits, else red */
    if (cc_placing() && gh->hasTileUnderMouse) {
        const CharCreate *cc = cc_state();
        Hex tiles[MAX_OCCUPIED];
        int n = character_occupied_tiles(gh->tileUnderMouse, cc->size, tiles);
        bool fits = character_fits(gh->tileUnderMouse, cc->size, NULL);
        dr_set_alpha(0.9f);
        for (int i = 0; i < n; i++)
            draw_hex(tiles[i], th + 3, HUD_THEME.accent[fits ? HUD_ACCENT_GREEN : HUD_ACCENT_RED]);
        float x, y, d;
        gfx_character_rect(gh->tileUnderMouse, cc->size, &x, &y, &d);
        dr_set_alpha(0.75f);
        if (cc->picture) dr_image(cc->picture, x, y, d, d);
        dr_set_alpha(0.5f);
    }

    bool in_range = gh->hasTileUnderMouse && ts_has(&gh->entityRangeTiles, gh->tileUnderMouse);
    Color preview = in_range ? rgb_hex(0x48CAE4) : rgb_hex(0xFB5607);
    for (int i = 0; i < gh->entityPreviewTiles.count; i++) draw_hex(gh->entityPreviewTiles.items[i], th + 2, preview);

    dr_set_alpha(1.0f);
}

static void draw_marker(Marker *m) {
    if (m->isDebug && !gh->debugMode && !(m->purpose == MARKER_STAT && m->attached)) return;
    if (!m->visible) return;
    marker_update(m);
    char text[128];
    marker_text(m, text, sizeof(text));
    float x = (int)m->coords.x, y = (int)m->coords.y;
    switch (m->purpose) {
    case MARKER_COORDINATES:
        dr_text(FONT_BOLD, 18, x, y, text_secondary(), text);
        break;
    case MARKER_STAT:
        dr_text(FONT_BOLD, 18, x, y, m->hasColor ? m->color : text_secondary(), text);
        break;
    case MARKER_DICE: {
        int digits = (int)strlen(text);
        dr_fill_round_rect(x, y, 16 * digits + 16, 16 * digits + 16, digits * 2, digits * 2, COLOR_RED);
        dr_text(FONT_BOLD, 32, x + 8, y + 20 + 8 * digits, text_secondary(), text);
        break;
    }
    case MARKER_DICERESULT:
        dr_text(FONT_BOLD, 50, gh->winW / 2, gh->winH / 2, text_secondary(), text);
        break;
    default:
        break;
    }
}

static void draw_length_label(Vec2 a, Vec2 b, int length) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%dft", length);
    dr_text(FONT_BOLD, gh->hexSize / 2, (int)(a.x + (b.x - a.x) / 2), (int)(a.y + (b.y - a.y) / 2), COLOR_BLACK, buf);
}

static void draw_fx(void) {
    int hs = gh->hexSize, th = gh->thickness;

    for (int i = 0; i < gh->markers.count; i++) draw_marker(gh->markers.items[i]);
    if (gh->totalLength) draw_marker(gh->totalLength);

    if (gh->debugMode) {
        /* hexagon centres and grid coordinates */
        int r0, r1, c0, c1;
        grid_visible_range(&r0, &r1, &c0, &c1);
        draw_debug_labels(r0, r1, c0, c1);
    }

    /* measurements */
    for (int i = 0; i < gh->measures.count; i++) {
        Measure *m = &gh->measures.items[i];
        Vec2 a, b;
        if (!measure_line(m, &a, &b)) continue;
        dr_line(a.x, a.y, b.x, b.y, th * 5, COLOR_RED);
        draw_length_label(a, b, measure_length(m));
    }

    /* line attack */
    if (gh->hasLineAttack) {
        Vec2 a, b;
        if (line_get(&gh->lineAttack, &a, &b)) {
            dr_line(a.x, a.y, b.x, b.y, th * 5, COLOR_RED);
            draw_length_label(a, b, line_length(&gh->lineAttack));
        }
    }

    /* area attack: circle */
    if (gh->hasAreaAttack) {
        Vec2 o = hex_center(gh->areaAttack.origin);
        double r = area_radius(&gh->areaAttack);
        dr_stroke_oval((int)(o.x - r), (int)(o.y - r), (int)r * 2, (int)r * 2, th * 2, COLOR_RED);
        char buf[32];
        snprintf(buf, sizeof(buf), "%dft", (int)(area_radius_in_tiles(&gh->areaAttack) * 5));
        dr_text(FONT_BOLD, hs / 2, (int)o.x, (int)o.y, COLOR_BLACK, buf);
    }

    /* cone attack: arc segment */
    if (gh->hasConeAttack) {
        ConeAttack *c = &gh->coneAttack;
        double radius = cone_radius(c);
        if (radius > hs) {
            Vec2 o = hex_center(c->origin);
            double start = cone_start_angle(c);
            double end = start + c->angle;
            double inner = hs / 2;
            dr_stroke_arc((int)(o.x - radius), (int)(o.y - radius), (int)radius * 2, (int)radius * 2,
                          (int)start, (int)c->angle, th * 2, COLOR_RED);
            dr_stroke_arc((int)(o.x - inner), (int)(o.y - inner), (int)inner * 2, (int)inner * 2,
                          (int)start, (int)c->angle, th * 2, COLOR_RED);
            double edges[2] = { start, end };
            for (int k = 0; k < 2; k++) {
                double a = edges[k] * -M_PI / 180;
                dr_line((int)(o.x + cos(a) * hs / 2), (int)(o.y + sin(a) * hs / 2),
                        (int)(o.x + cos(a) * radius), (int)(o.y + sin(a) * radius), th * 2, COLOR_RED);
            }
            char buf[32];
            snprintf(buf, sizeof(buf), "%dft", (int)(cone_radius_in_tiles(c) * 5));
            dr_text(FONT_BOLD, hs / 2, (int)o.x, (int)o.y, COLOR_BLACK, buf);
        }
    }

    /* movement path */
    for (int i = 0; i + 1 < gh->path.count; i++) {
        Vec2 a = hex_center(gh->path.items[i]), b = hex_center(gh->path.items[i + 1]);
        dr_line((int)a.x, (int)a.y, (int)b.x, (int)b.y, th * 3, COLOR_RED);
    }
    /* the way a move action would take */
    for (int i = 0; i + 1 < gh->moveMode.path.count; i++) {
        Vec2 a = hex_center(gh->moveMode.path.items[i]), b = hex_center(gh->moveMode.path.items[i + 1]);
        dr_line((int)a.x, (int)a.y, (int)b.x, (int)b.y, th * 3, rgb_hex(0xFFFFFF));
    }
    draw_block_marks();
    draw_dodge_marks();
    draw_attack_marks();
}

static void draw_toolbox(void) {
    const Theme *t = gfx_theme();
    Toolbox *tb = &gh->toolbox;
    float h = (float)(gh->winH - console_panel_rect().h);
    dr_fill_rect(0, 0, tb->size, h, t->secondary);
    draw_border(0, 0, tb->size, h, t->secondary);
    dr_set_clip(0, 0, tb->size, h);
    for (int i = 0; i < tb->ntools; i++)
        if (tb->tools[i].icon) dr_image(tb->tools[i].icon, 1, i * tb->size, tb->size - 2, tb->size - 2);
    if (tb->selected >= 0) {
        Rect r = tb->tools[tb->selected].hitbox;
        dr_stroke_rect(r.x, r.y, r.w, r.h, 2, COLOR_BLUE);
    }
    dr_clear_clip();
}

/* Wraps the newest messages to width w (only as many as the view needs) and draws CONSOLE_LOG_LINES rows,
 * bottom-aligned, scrolled back by con->logScroll lines. */
static void draw_log(Console *con, float x, float y, float w, float line_h, Color c) {
    float fs = (float)con->fontSize, ascent, descent;
    dr_font_metrics(FONT_PLAIN, fs, &ascent, &descent);
    int need = CONSOLE_LOG_LINES + con->logScroll;
    TextSpan sp[128]; /* a 2 KiB message at a usable width has far fewer lines; any more are left out */
    da_clear(&log_lines);
    for (int m = con->logs.count - 1; m >= 0 && log_lines.count < need; m--) {
        int n = text_wrap(FONT_PLAIN, fs, con->logs.items[m], w, sp, 128);
        if (n > 128) n = 128;
        for (int k = n - 1; k >= 0; k--) {
            LogLine l = { m, sp[k] };
            da_push(&log_lines, l);
        }
    }
    int max_scroll = log_lines.count > CONSOLE_LOG_LINES ? log_lines.count - CONSOLE_LOG_LINES : 0;
    if (con->logScroll > max_scroll) con->logScroll = max_scroll; /* scrolled past the oldest line */
    for (int row = 0; row < CONSOLE_LOG_LINES; row++) {
        int k = con->logScroll + CONSOLE_LOG_LINES - 1 - row;
        if (k >= log_lines.count) continue;
        const LogLine *l = &log_lines.items[k];
        char text[2048];
        snprintf(text, sizeof(text), "%.*s", l->span.len, con->logs.items[l->msg] + l->span.start);
        dr_text(FONT_PLAIN, fs, x, y + row * line_h + (line_h + ascent - descent) / 2, c, text);
    }
}

static void draw_console(void) {
    Console *con = &gh->console;
    if (!con->active) return;
    const Theme *t = gfx_theme();
    Rect r = console_panel_rect();
    float fs = (float)con->fontSize;
    float input_h = fminf((float)r.h, (float)(con->fontSize * 8 / 5 + 2));
    float ascent, descent;
    dr_font_metrics(FONT_PLAIN, fs, &ascent, &descent);

    /* log lines on a translucent background */
    if (con->showLog) {
        float log_h = r.h - input_h;
        dr_fill_rect(r.x, r.y, r.w, log_h, color_alpha(t->primary, 220));
        float line_h = (log_h - 10) / CONSOLE_LOG_LINES;
        dr_set_clip(r.x + 5, r.y, r.w - 10, log_h);
        draw_log(con, r.x + 5, r.y + 5, r.w - 10, line_h, t->secondary);
        dr_clear_clip();
    }

    /* input line */
    float iy = r.y + r.h - input_h;
    dr_fill_rect(r.x, iy, r.w, input_h, t->primary);
    draw_border(r.x, iy, r.w, input_h, t->secondary);

    char prefix[CONSOLE_TEXT_MAX];
    snprintf(prefix, sizeof(prefix), "%.*s", con->caret, con->text);
    float caret_x = dr_text_width(FONT_PLAIN, fs, prefix);
    float visible = r.w - 10;
    if (caret_x - con->scrollX > visible) con->scrollX = caret_x - visible;
    if (caret_x < con->scrollX) con->scrollX = caret_x;
    if (dr_text_width(FONT_PLAIN, fs, con->text) < visible) con->scrollX = 0;

    float base = iy + (input_h + ascent - descent) / 2;
    dr_set_clip(r.x + 2, iy, r.w - 4, input_h);
    dr_text(FONT_PLAIN, fs, r.x + 4 - con->scrollX, base, t->secondary, con->text);
    if ((SDL_GetTicks() / 500) % 2 == 0)
        dr_fill_rect(r.x + 4 - con->scrollX + caret_x, base - ascent, 1, ascent + descent, t->secondary);
    dr_clear_clip();

    draw_border(r.x, r.y, r.w, r.h, t->secondary);
}

/* ---- HUD (bottom centre) ------------------------------------------------------------------------------ */

static Uint64 hud_last_ms;   /* time of the previous frame */
static bool bars_moving;     /* a HUD bar moved in the previous frame */

bool gfx_bars_moving(void) { return bars_moving; }

static void draw_hud(void) {
    Uint64 now = SDL_GetTicks();
    float dt = bars_moving ? (float)(now - hud_last_ms) / 1000.0f : 0; /* idle: start at this frame */
    if (dt > 0.1f) dt = 0.1f;
    hud_last_ms = now;
    bars_moving = hud_game_update(dt);
    hud_draw();
}

/* ":tm": a label at the top of the map while test mode is on, so nobody forgets it */
static void draw_test_mode(void) {
    if (!gh->testMode) return;
    const char *text = "TEST MODE";
    float fs = 16, pad = 8, ascent, descent;
    dr_font_metrics(FONT_BOLD, fs, &ascent, &descent);
    float w = dr_text_width(FONT_BOLD, fs, text) + 2 * pad, h = ascent + descent + pad;
    float left = (float)gh->toolbox.size, x = left + ((float)gh->winW - left - w) / 2, y = 8;
    dr_fill_round_rect(x, y, w, h, 8, 8, HUD_THEME.accent[HUD_ACCENT_GOLD]);
    dr_text(FONT_BOLD, fs, x + pad, y + pad / 2 + ascent, HUD_THEME.panel, text);
}

/* ---- radial menu (on top of everything) --------------------------------------------------------------------- */

static Uint64 radial_last_ms;
static bool radial_moving; /* the menu animated in the previous frame */

static void draw_radial(void) {
    Uint64 now = SDL_GetTicks();
    float dt = radial_moving ? (float)(now - radial_last_ms) / 1000.0f : 0; /* idle: start at this frame */
    if (dt > 0.1f) dt = 0.1f;
    radial_last_ms = now;
    radial_moving = radial_game_update(dt);
    radial_draw();
}

bool gfx_draw_frame(const char *screenshot_path) {
    bool theme_fading = theme_fade_step();
    const Theme *t = gfx_theme();
    dr_begin((float)gh->winW, (float)gh->winH, gh->pixelScale);

    /* layers in the same order as the Swing panels: background, entities, grid, effects, UI */
    if (gh->backgroundImage) {
        update_background_size();
        dr_image(gh->backgroundImage, gh->gridOffsetX, gh->gridOffsetY, gh->drawWidth, gh->drawHeight);
    }
    draw_entities();
    draw_owner_rings();
    session_draw_request_preview();
    draw_grid();
    draw_fx();
    draw_toolbox();
    draw_hud();
    draw_test_mode();
    draw_console();
    draw_radial();

    dr_end_and_render(t->primary, screenshot_path);
    return bars_moving || theme_fading || radial_moving;
}
