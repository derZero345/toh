/* Mouse and keyboard handling (main/IOHandler). */
#include "app.h"
#include "ops.h"

#include <stdio.h>

#define CON (&gh->console)

static void select_entity(void);
static void select_entity_no_deselection(void);
static void select_tile(void);

void io_toggle_game_master(void) { gh->gameMaster = !gh->gameMaster; }

/* ---- tool selection --------------------------------------------------------------------- */

static void select_tool(int index) {
    Toolbox *tb = &gh->toolbox;
    if (index < 0 || index >= tb->ntools) return;
    tb->selected = index;
    gh->mode = tb->tools[index].mode;

    /* drop an unfinished measurement when leaving the measure tool */
    if (gh->mode != LENGTH_MODE && gh->measures.count > 0) {
        if (!gh->measures.items[gh->measures.count - 1].finished) gh->measures.count--;
        io_update_total_length();
        gh->totalLength->coords = gh->totalLength->displayCoords = (Vec2){ tb->size, 20 };
    }
    if (gh->mode != LINE_MODE) gh->hasLineAttack = false;
    if (gh->mode != AREA_MODE) gh->hasAreaAttack = false;
    if (gh->mode != CONE_MODE) gh->hasConeAttack = false;
    da_clear(&gh->attackTiles);
}

static void select_tool_mode(int mode) {
    for (int i = 0; i < gh->toolbox.ntools; i++)
        if (gh->toolbox.tools[i].mode == mode) { select_tool(i); return; }
}

/* ---- helpers ---------------------------------------------------------------------------------- */

void io_update_total_length(void) {
    int length = 0;
    for (int i = 0; i < gh->measures.count; i++) length += measure_length(&gh->measures.items[i]);
    if (!gh->totalLength) {
        gh->totalLength = marker_new(gh->mousePos, MARKER_STAT, false);
        snprintf(gh->totalLength->suffix, sizeof(gh->totalLength->suffix), "ft");
    }
    gh->totalLength->stat = length;
    gh->totalLength->coords = gh->totalLength->displayCoords = gh->mousePos;
    gh->totalLength->visible = length > 0; /* hide when 0 */
}

static void log_mouse_pos(Vec2 p) {
    if (!gh->mouseLog) {
        gh->mouseLog = marker_new((Vec2){ 0, 0 }, MARKER_COORDINATES, true);
        marker_add(gh->mouseLog);
    }
    gh->mouseLog->coords = gh->mouseLog->displayCoords = p;
}

static void refresh_preview(void) {
    da_clear(&gh->entityPreviewTiles);
    gfx_add_entity_preview_tiles();
}

/* Hexagon under the cursor and dependent previews. */
static void update_mouse_tile(void) {
    Hex h;
    if (!hex_find_closest(gh->mousePos, &h)) return;
    if (!gh->hasCurrentHex || !hex_eq(h, gh->currentHexagon) || !gh->hasTileUnderMouse) {
        gh->currentHexagon = h;
        gh->hasCurrentHex = true;
        gh->tileUnderMouse = h;
        gh->hasTileUnderMouse = true;
        if (gh->isShiftDown) refresh_preview();
    }
}

/* Moves the keyboard cursor to neighbour 1..6, scrolling the view when it leaves the window. */
static void move_tile_under_mouse(int dir) {
    if (!gh->hasTileUnderMouse) {
        Hex h;
        if (hex_find_closest((Vec2){ gh->winW / 2, gh->winH / 2 }, &h)) {
            gh->tileUnderMouse = h;
            gh->hasTileUnderMouse = true;
        }
    }
    if (gh->hasTileUnderMouse) {
        Hex cur = gh->tileUnderMouse;
        Hex next = hex_neighbor_raw(cur, dir - 1);
        Vec2 oc = hex_center(cur), nc = hex_center(next);
        if (nc.x < 0 || nc.y < 0 || nc.x > gh->winW || nc.y > console_panel_rect().y)
            gfx_grid_offset((int)(oc.x - nc.x), (int)(oc.y - nc.y));
        gh->tileUnderMouse = next;
        gh->hasTileUnderMouse = true;
    }
    gh->mouseActive = false;
}

/* ---- selection ---------------------------------------------------------------------------- */

static void select_entity(void) {
    for (int i = 0; i < gh->selectedEntityTiles.count; i++) {
        if (hex_eq(gh->currentHexagon, gh->selectedEntityTiles.items[i])) {
            da_remove_at(&gh->selectedEntityTiles, i); /* toggle off */
            return;
        }
    }
    select_entity_no_deselection();
}

static void select_entity_no_deselection(void) {
    for (int i = 0; i < gh->entities.count; i++) {
        Entity *en = gh->entities.items[i];
        Hex tiles[MAX_OCCUPIED];
        int n = entity_occupied_tiles(en, tiles);
        for (int k = 0; k < n; k++) {
            if (hex_eq(gh->currentHexagon, tiles[k])) {
                da_push(&gh->selectedEntityTiles, en->tile);
                gh->hasSelectedEntity = true;
            }
        }
    }

    if (gh->hasSelectedEntity) ts_free(&gh->entityRangeTiles);
    if (!gh->hasSelectedEntity || gh->selectedEntityTiles.count == 0) return;

    /* movement range of the first selected character; astar_range already checks
     * that every tile the character would cover is free of other entities */
    Entity *ch = entity_at(gh->selectedEntityTiles.items[0]);
    if (!ch || ch->type != ENTITY_CHARACTER) return;
    astar_range(ch->tile, ch->speed, ch, &gh->entityRangeTiles);
}

static void select_tile(void) {
    da_clear(&gh->path);
    for (int i = 0; i < gh->selectedTiles.count; i++) {
        if (hex_eq(gh->currentHexagon, gh->selectedTiles.items[i])) {
            da_remove_at(&gh->selectedTiles, i); /* toggle off */
            return;
        }
    }
    if (!gh->isShiftDown) da_push(&gh->selectedTiles, gh->currentHexagon);
    if (gh->selectedEntityTiles.count == 0) return;

    Entity *sel = entity_at(gh->selectedEntityTiles.items[0]);
    bool is_char = sel && sel->type == ENTITY_CHARACTER;
    /* Shift+click inside the range moves the character; GM mode moves anywhere. */
    if (is_char && ((gh->isShiftDown && ts_has(&gh->entityRangeTiles, gh->currentHexagon)) || gh->gameMaster)) {
        Op op = { .type = OP_MOVE, .id = sel->id, .tile = gh->currentHexagon };
        if (ops_submit(&op) != OPR_OK) return;

        gh->hasSelectedEntity = false;
        da_clear(&gh->selectedEntityTiles);
        ts_free(&gh->entityRangeTiles);
        da_clear(&gh->selectedTiles);

        /* reselect the entity at its new position */
        Hex h;
        if (hex_find_closest(gh->mousePos, &h)) {
            gh->currentHexagon = h;
            gh->hasCurrentHex = true;
            select_entity();
        }
    }
}

/* ---- mouse buttons -------------------------------------------------------------------------- */

static void lmb_pressed(void) {
    gh->LMBDown = true;
    for (int i = 0; i < gh->toolbox.ntools; i++)
        if (rect_contains(gh->toolbox.tools[i].hitbox, gh->mousePos)) select_tool(i);

    if (!gh->isCtrlDown && !gh->isShiftDown) {
        da_clear(&gh->selectedEntityTiles);
        da_clear(&gh->selectedTiles);
    }
    Hex h;
    if (!hex_find_closest(gh->mousePos, &h)) return;
    gh->currentHexagon = h;
    gh->hasCurrentHex = true;
    select_tile();
    if (!gh->isShiftDown) select_entity();
}

static void rmb_pressed(void) {
    switch (gh->mode) {
    case DRAG_MODE:
        if (gh->mouseActive) {
            gh->dragStart = gh->mousePos;
            gh->hasDragStart = true;
        }
        break;
    case LENGTH_MODE:
        if (gh->measures.count > 0 && measure_finish(&gh->measures.items[gh->measures.count - 1]))
            return; /* double click on the origin cleared all measurements */
        if (!gh->hasTileUnderMouse) return;
        da_push(&gh->measures, ((Measure){ .origin = gh->tileUnderMouse }));
        break;
    case LINE_MODE:
        if (!gh->hasTileUnderMouse) return;
        gh->lineAttack = (LineAttack){ .origin = gh->tileUnderMouse };
        gh->hasLineAttack = true;
        break;
    case AREA_MODE:
        if (!gh->hasTileUnderMouse) return;
        gh->areaAttack = (AreaAttack){ .origin = gh->tileUnderMouse, .originPoint = hex_center(gh->tileUnderMouse) };
        gh->hasAreaAttack = true;
        break;
    case CONE_MODE:
        if (!gh->hasTileUnderMouse) return;
        gh->coneAttack = (ConeAttack){ .origin = gh->tileUnderMouse, .originPoint = hex_center(gh->tileUnderMouse),
                                       .angle = 91 };
        gh->hasConeAttack = true;
        break;
    }
}

/* ---- mouse motion & wheel --------------------------------------------------------------------- */

static void mouse_moved(Vec2 p) {
    if (gh->debugMode) log_mouse_pos(p);
    gh->mousePos = p;
    if (gh->mouseActive) update_mouse_tile();
    if (gh->mode == LENGTH_MODE) io_update_total_length();
}

static void mouse_dragged(Vec2 p) {
    gh->mousePos = p;
    if (gh->debugMode) log_mouse_pos(p);
    Hex h;
    if (hex_find_closest(p, &h)) {
        gh->currentHexagon = h;
        gh->hasCurrentHex = true;
        /* Ctrl + drag: multi-select tiles */
        if (gh->isCtrlDown && gh->LMBDown && !hl_contains(&gh->selectedTiles, h)) {
            da_push(&gh->selectedTiles, h);
            select_entity();
        }
    }
    if (gh->mode == DRAG_MODE) gfx_drag(p);
}

static void mouse_wheel(int notches) {
    if (notches == 0) return;
    if (rect_contains(console_panel_rect(), gh->mousePos)) {
        console_scroll_log(notches);
        return;
    }
    if (gh->isCtrlDown) {
        /* reserved */
    } else if (gh->isShiftDown) {
        if (gh->mode == CONE_MODE && gh->hasConeAttack) cone_change_angle(&gh->coneAttack, notches);
    } else {
        gfx_zoom(notches, gh->mousePos, true);
    }
}

/* ---- keyboard ------------------------------------------------------------------------------------ */

static void copy_entities(void) {
    for (int i = 0; i < gh->clipboard.count; i++) entity_free(gh->clipboard.items[i]);
    da_clear(&gh->clipboard);
    for (int i = 0; i < gh->selectedEntityTiles.count; i++) {
        Entity *en = entity_at(gh->selectedEntityTiles.items[i]);
        if (en) da_push(&gh->clipboard, entity_clone(en, false));
    }
}

static void paste_entities(void) {
    if (gh->clipboard.count == 0 || !gh->hasTileUnderMouse) return;
    Hex copy = gh->clipboard.items[0]->tile, paste = gh->tileUnderMouse;
    Cube pivot = hex_to_cube(paste.row - copy.row, paste.col - copy.col);
    Op op = { .type = OP_SPAWN };
    for (int i = 0; i < gh->clipboard.count; i++) {
        Entity *en = gh->clipboard.items[i];
        Cube cp = hex_to_cube(en->tile.row, en->tile.col);
        /* like the Java version, the clipboard moves along, so the next paste is relative to this one */
        en->tile = cube_to_hex((Cube){ pivot.x + cp.x, pivot.y + cp.y, pivot.z + cp.z });
        da_push(&op.specs, op_spec_from_entity(en)); /* copies have no owner */
    }
    ops_submit(&op);
    op_free(&op);
}

static void select_all_entities(void) {
    for (int i = 0; i < gh->entities.count; i++) {
        Entity *en = gh->entities.items[i];
        gh->currentHexagon = en->tile;
        gh->hasCurrentHex = true;
        if (hl_contains(&gh->selectedEntityTiles, en->tile)) continue;
        select_entity_no_deselection();
    }
}

static void zoom_at_cursor(int notches) {
    if (gh->hasTileUnderMouse) gfx_zoom(notches, hex_center(gh->tileUnderMouse), true);
    else gfx_zoom(notches, (Vec2){ gh->winW / 2, gh->winH / 2 }, true);
}

static void key_pressed(const SDL_KeyboardEvent *k) {
    SDL_Keycode key = k->key;
    CON->swallowText = false;

    if (CON->active) {
        switch (key) {
        case SDLK_UP:   console_arrow_up(); break;
        case SDLK_DOWN: console_arrow_down(); break;
        case SDLK_TAB:  console_autocomplete(); break;
        default:
            console_reset_suggestions();
            console_key_edit(key);
            break;
        }
    } else {
        switch (key) {
        case SDLK_W: gh->WDown = true; break;
        case SDLK_A: gh->ADown = true; break;
        case SDLK_S: gh->SDown = true; break;
        case SDLK_D: gh->DDown = true; break;
        case SDLK_LEFT:  move_tile_under_mouse(gh->isRShiftDown ? 5 : 6); break;
        case SDLK_RIGHT: move_tile_under_mouse(gh->isRShiftDown ? 2 : 3); break;
        case SDLK_UP:
            if (gh->isRShiftDown) zoom_at_cursor(-1);
            else move_tile_under_mouse(1);
            break;
        case SDLK_DOWN:
            if (gh->isRShiftDown) zoom_at_cursor(1);
            else move_tile_under_mouse(4);
            break;
        default: break;
        }
    }

    /* always active */
    switch (key) {
    case SDLK_LSHIFT:
        gh->isShiftDown = true;
        if (gh->selectedEntityTiles.count > 0) refresh_preview();
        break;
    case SDLK_RSHIFT: gh->isRShiftDown = true; break;
    case SDLK_LCTRL: case SDLK_RCTRL: gh->isCtrlDown = true; break;
    case SDLK_LALT:  case SDLK_RALT:  gh->isAltDown = true; break;
    case SDLK_RETURN: case SDLK_KP_ENTER:
        if (k->repeat) break;
        if (gh->isCtrlDown) { gfx_toggle_console(); break; }
        if (gh->isAltDown)  { console_toggle_log(); break; }
        if (CON->active)    { console_command(CON->text); break; }
        if (!gh->mouseActive) {
            /* simulate a click on the keyboard cursor */
            gh->mousePos = gh->hasTileUnderMouse ? hex_center(gh->tileUnderMouse)
                                                 : (Vec2){ gh->winW / 2, gh->winH / 2 };
            if (gh->isRShiftDown) rmb_pressed();
            else lmb_pressed();
        }
        break;
    case SDLK_PLUS: case SDLK_KP_PLUS: case SDLK_EQUALS:
        if (gh->isCtrlDown) {
            console_change_font_size(1);
            toolbox_change_size(&gh->toolbox, 1);
        }
        break;
    case SDLK_MINUS: case SDLK_KP_MINUS:
        if (gh->isCtrlDown && gh->toolbox.size > 8) {
            console_change_font_size(-1);
            toolbox_change_size(&gh->toolbox, -1);
        }
        break;
    case SDLK_A:
        if (gh->isCtrlDown) select_all_entities();
        else if (gh->isAltDown) select_tool_mode(AREA_MODE);
        break;
    case SDLK_C:
        if (gh->isCtrlDown) copy_entities();
        else if (gh->isAltDown) select_tool_mode(CONE_MODE);
        break;
    case SDLK_V:
        if (gh->isCtrlDown) {
            paste_entities();
            if (CON->active) {
                char *clip = SDL_GetClipboardText();
                if (clip) {
                    for (char *p = clip; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
                    console_insert(clip);
                    SDL_free(clip);
                }
            }
        } else if (gh->isAltDown) {
            select_tool_mode(LINE_MODE);
        }
        break;
    case SDLK_S: if (gh->isAltDown) select_tool_mode(LENGTH_MODE); break;
    case SDLK_X: if (gh->isAltDown) select_tool_mode(DRAG_MODE); break;
    case SDLK_PERIOD: case SDLK_COLON: case SDLK_SEMICOLON:
        /* ':' opens the console with ":" typed (Shift+. on German layouts) */
        if (!CON->active && (key == SDLK_COLON || (gh->isShiftDown || gh->isRShiftDown))) {
            gfx_set_console_visibility(true);
            console_set_text(":");
            CON->swallowText = true;
        }
        break;
    default: break;
    }
}

static void key_released(const SDL_KeyboardEvent *k) {
    switch (k->key) {
    case SDLK_W: gh->WDown = false; break;
    case SDLK_A: gh->ADown = false; break;
    case SDLK_S: gh->SDown = false; break;
    case SDLK_D: gh->DDown = false; break;
    case SDLK_LSHIFT:
        gh->isShiftDown = false;
        da_clear(&gh->entityPreviewTiles);
        break;
    case SDLK_RSHIFT: gh->isRShiftDown = false; break;
    case SDLK_LCTRL: case SDLK_RCTRL: gh->isCtrlDown = false; break;
    case SDLK_LALT:  case SDLK_RALT:  gh->isAltDown = false; break;
    default: break;
    }
}

static void text_input(const char *text) {
    if (CON->swallowText) {
        CON->swallowText = false;
        return;
    }
    if (!CON->active) return;
    SDL_Keymod mods = SDL_GetModState();
    if (mods & (SDL_KMOD_CTRL | SDL_KMOD_LALT)) return;
    console_insert(text);
}

/* ---- dispatch ---------------------------------------------------------------------------------- */

void io_handle_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        gh->mouseActive = true;
        gh->mousePos = (Vec2){ e->button.x, e->button.y };
        if (e->button.button == SDL_BUTTON_LEFT) lmb_pressed();
        else if (e->button.button == SDL_BUTTON_RIGHT) {
            gh->RMBDown = true;
            rmb_pressed();
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e->button.button == SDL_BUTTON_LEFT) gh->LMBDown = false;
        else if (e->button.button == SDL_BUTTON_RIGHT) {
            gh->RMBDown = false;
            if (gh->mode == DRAG_MODE) gh->hasDragStart = false;
        }
        break;
    case SDL_EVENT_MOUSE_MOTION: {
        Vec2 p = { e->motion.x, e->motion.y };
        if (e->motion.state & (SDL_BUTTON_LMASK | SDL_BUTTON_MMASK | SDL_BUTTON_RMASK)) mouse_dragged(p);
        else mouse_moved(p);
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
        int y = e->wheel.integer_y, x = e->wheel.integer_x;
        if (e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED) { y = -y; x = -x; }
        int notches = -y; /* Java: positive rotation = towards the user */
        if (notches == 0 && gh->isShiftDown) notches = -x;
        gh->mousePos = (Vec2){ e->wheel.mouse_x, e->wheel.mouse_y };
        mouse_wheel(notches);
        break;
    }
    case SDL_EVENT_KEY_DOWN:
        key_pressed(&e->key);
        break;
    case SDL_EVENT_KEY_UP:
        key_released(&e->key);
        break;
    case SDL_EVENT_TEXT_INPUT:
        text_input(e->text.text);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        /* avoid stuck keys (a known issue of the Java version) */
        gh->isShiftDown = gh->isRShiftDown = gh->isCtrlDown = gh->isAltDown = false;
        gh->WDown = gh->ADown = gh->SDown = gh->DDown = false;
        gh->LMBDown = gh->RMBDown = false;
        gh->hasDragStart = false;
        da_clear(&gh->entityPreviewTiles);
        break;
    default:
        break;
    }
}
