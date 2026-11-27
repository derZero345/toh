/* Game loop tick, character movement and initiative tracking (main/GameHandler). */
#include "app.h"
#include "blocks.h"
#include "dodges.h"

/* Called at ~60 Hz: camera panning with WASD. */
void game_tick(void) {
    if (gh->isAltDown) return;              /* pause movement */
    else if (gh->isShiftDown) gh->moveSpeed = 12;
    else if (gh->isCtrlDown)  gh->moveSpeed = 6;
    else                      gh->moveSpeed = 3;

    if (gh->ADown) gfx_grid_offset(gh->moveSpeed, 0);
    if (gh->DDown) gfx_grid_offset(-gh->moveSpeed, 0);
    if (gh->SDown) gfx_grid_offset(0, -gh->moveSpeed);
    if (gh->WDown) gfx_grid_offset(0, gh->moveSpeed);

    gfx_out_of_bounds_correction();
}

void game_move_character(Hex target, Entity *c) {
    if (!c) return;
    astar_path(c->tile, target, false, &gh->path);
    c->tile = target;
}

/* ---- initiative ----------------------------------------------------------------- */

static int init_index(const Entity *e) {
    for (int i = 0; i < gh->init.count; i++)
        if (gh->init.items[i].entity == e) return i;
    return -1;
}

bool game_is_in_initiative(const Entity *e) { return init_index(e) >= 0; }

/* Appends e with the given roll value and creates its marker. */
static void initiative_push(Entity *e, double value) {
    da_push(&gh->init, ((InitEntry){ e, value }));

    /* visual marker above the entity */
    Hex tiles[MAX_OCCUPIED];
    entity_occupied_tiles(e, tiles);
    Vec2 c = hex_center(tiles[0]);
    Marker *m = marker_new_stat(0, (Vec2){ (int)c.x, (int)c.y - gh->hexSize }, MARKER_STAT, false);
    m->attached = e;
    m->debugValue = value;
    da_push(&gh->initMarkers, m);
    marker_add(m);
}

/* Highest initiative first; stable so equal values keep their order. */
static void sort_initiative(void) {
    for (int i = 1; i < gh->init.count; i++) {
        InitEntry cur = gh->init.items[i];
        int j = i - 1;
        while (j >= 0 && gh->init.items[j].value < cur.value) {
            gh->init.items[j + 1] = gh->init.items[j];
            j--;
        }
        gh->init.items[j + 1] = cur;
    }
    if (gh->currentTurn) { /* keep the turn with the same entity, wherever it moved */
        int idx = init_index(gh->currentTurn);
        if (idx >= 0) gh->currentTurnIndex = idx;
    }
    if (gh->initMarkers.count > 0) game_update_initiative_markers();
}

void game_initiative_list(Entity **entities, const double *values, int n) {
    if (!entities) return;
    for (int i = 0; i < n; i++)
        if (entities[i] && !game_is_in_initiative(entities[i])) initiative_push(entities[i], values[i]);
    sort_initiative();
    game_update_initiative_markers();
}

void game_update_initiative_markers(void) {
    int count = gh->init.count;
    for (int i = 0; i < count; i++) {
        Entity *e = gh->init.items[i].entity;
        for (int k = 0; k < gh->initMarkers.count; k++) {
            Marker *m = gh->initMarkers.items[k];
            if (m->attached != e) continue;
            m->stat = i; /* position in the turn order */
            m->debugValue = gh->init.items[i].value;
            m->hasColor = true;
            if (i == gh->currentTurnIndex)                   m->color = COLOR_GREEN;      /* current */
            else if (i == (gh->currentTurnIndex + 1) % count) m->color = rgb_hex(0xF9A801); /* next */
            else                                              m->color = COLOR_RED;
            break;
        }
    }
}

void game_next_turn(void) {
    if (gh->init.count == 0) return;
    /* the first :i next starts the round with the highest roller (the Java version skipped them) */
    if (gh->currentTurn) gh->currentTurnIndex = (gh->currentTurnIndex + 1) % gh->init.count;
    else gh->currentTurnIndex %= gh->init.count;
    gh->currentTurn = gh->init.items[gh->currentTurnIndex].entity;
    gh->currentTurn->turnUsed = 0; /* a new turn: its Major and Minor actions are free again (moves.h) */
    attack_marks_clear_of(gh->currentTurn->id); /* and the tiles it attacked last round are no longer marked */
    blocks_turn_started(gh->currentTurn->id);   /* its blocks end (blocks.h) */
    dodges_turn_started(gh->currentTurn->id);   /* its dodges end (dodges.h) */
    game_update_initiative_markers();
}

void game_remove_from_initiative(Entity *e) {
    int idx = init_index(e);
    if (!e || idx < 0) return;
    da_remove_at(&gh->init, idx);

    for (int k = 0; k < gh->initMarkers.count; k++) {
        Marker *m = gh->initMarkers.items[k];
        if (m->attached == e) {
            da_remove_at(&gh->initMarkers, k);
            marker_remove(m);
            break;
        }
    }

    if (e == gh->currentTurn) {
        if (gh->init.count == 0) {
            gh->currentTurn = NULL;
            gh->currentTurnIndex = 0;
        } else { /* the next one in order slid into the removed slot */
            gh->currentTurnIndex = idx % gh->init.count;
            gh->currentTurn = gh->init.items[gh->currentTurnIndex].entity;
            gh->currentTurn->turnUsed = 0; /* its turn starts */
            attack_marks_clear_of(gh->currentTurn->id);
            blocks_turn_started(gh->currentTurn->id);
            dodges_turn_started(gh->currentTurn->id);
        }
    } else if (gh->currentTurnIndex > 0 && init_index(gh->currentTurn) < gh->currentTurnIndex) {
        gh->currentTurnIndex--;
    }
    if (gh->init.count == 0) { /* the initiative ended */
        attack_marks_clear();
        blocks_clear();
        dodges_clear();
    }
    game_update_initiative_markers();
}

void game_toggle_show_initiative(void) {
    for (int k = 0; k < gh->initMarkers.count; k++)
        gh->initMarkers.items[k]->visible = !gh->initMarkers.items[k]->visible;
}

void game_clear_initiative(void) {
    for (int k = 0; k < gh->initMarkers.count; k++) marker_remove(gh->initMarkers.items[k]);
    da_clear(&gh->initMarkers);
    da_clear(&gh->init);
    gh->currentTurn = NULL;
    gh->currentTurnIndex = 0;
    attack_marks_clear(); /* marks, blocks and dodges last a round; without initiative there are none */
    blocks_clear();
    dodges_clear();
}

void game_restore_initiative(Entity **entities, const double *values, int n, int current, bool turn_started,
                             bool markers_visible) {
    game_clear_initiative();
    for (int i = 0; i < n; i++)
        if (entities[i] && !game_is_in_initiative(entities[i])) initiative_push(entities[i], values[i]);
    if (gh->init.count > 0) {
        gh->currentTurnIndex = (current >= 0 && current < gh->init.count) ? current : 0;
        gh->currentTurn = turn_started ? gh->init.items[gh->currentTurnIndex].entity : NULL;
    }
    game_update_initiative_markers();
    for (int k = 0; k < gh->initMarkers.count; k++) gh->initMarkers.items[k]->visible = markers_visible;
}
