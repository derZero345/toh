/* Entities (entities package) and markers (fx/Marker). */
#include "app.h"

#include <stdio.h>

/* ---- markers ------------------------------------------------------------------ */

Marker *marker_new(Vec2 location, int purpose, bool debug) {
    Marker *m = calloc(1, sizeof(*m));
    if (!m) abort();
    m->coords = location;
    m->displayCoords = location;
    m->purpose = purpose;
    m->isDebug = debug;
    m->visible = true;
    return m;
}

Marker *marker_new_stat(double stat, Vec2 coords, int purpose, bool debug) {
    Marker *m = marker_new(coords, purpose, debug);
    m->stat = stat;
    return m;
}

void marker_text(const Marker *m, char *buf, size_t n) {
    if (m->purpose == MARKER_STAT && gh->debugMode && m->attached) {
        snprintf(buf, n, "%d [%.3f]%s", (int)m->stat, m->debugValue, m->suffix);
        return;
    }
    switch (m->purpose) {
    case MARKER_COORDINATES:
        snprintf(buf, n, "[%d|%d]", (int)m->displayCoords.x, (int)m->displayCoords.y);
        break;
    case MARKER_STAT:
    case MARKER_DICE:
    case MARKER_DICERESULT: {
        char num[64];
        fmt_java_double(num, sizeof(num), m->stat);
        snprintf(buf, n, "%s %s", num, m->suffix);
        break;
    }
    default:
        buf[0] = '\0';
    }
}

void marker_update(Marker *m) {
    if (!m->attached) return;
    Hex tiles[MAX_OCCUPIED];
    entity_occupied_tiles(m->attached, tiles);
    Vec2 c = hex_center(tiles[0]);
    m->coords = (Vec2){ (int)c.x, (int)c.y - gh->hexSize };
}

void marker_add(Marker *m) { da_push(&gh->markers, m); }

void marker_remove(Marker *m) {
    if (!m) return;
    da_remove_value(&gh->markers, m);
    free(m);
}

/* ---- entities ------------------------------------------------------------------ */

static Entity *entity_alloc(EntityType type, Texture *image, Hex tile) {
    Entity *e = calloc(1, sizeof(*e));
    if (!e) abort();
    e->type = type;
    e->image = image;
    vkr_texture_retain(image);
    e->tile = tile;
    e->location = hex_center(tile);
    /* Debug marker showing the grid coordinates of the entity. */
    e->debugMarker = marker_new((Vec2){ tile.row, tile.col }, MARKER_COORDINATES, true);
    marker_add(e->debugMarker);
    return e;
}

Entity *entity_new_character(Texture *image, Hex tile, int size, int maxhealth, int ac, int speed, int initiative) {
    Entity *e = entity_alloc(ENTITY_CHARACTER, image, tile);
    e->size = size;
    e->maxHealth = maxhealth;
    e->health = maxhealth;
    e->attack = 0;
    e->ac = ac;
    e->speed = speed > MAX_SPEED ? MAX_SPEED : speed;
    e->initiative = initiative;
    return e;
}

Entity *entity_new_wall(Texture *image, Hex tile) {
    return entity_alloc(ENTITY_WALL, image, tile);
}

Entity *entity_clone(const Entity *src, bool with_marker) {
    Entity *e = malloc(sizeof(*e));
    if (!e) abort();
    *e = *src;
    e->id = 0; /* not on the table until entity_register */
    vkr_texture_retain(e->image);
    e->debugMarker = NULL;
    if (with_marker) {
        e->debugMarker = marker_new((Vec2){ e->tile.row, e->tile.col }, MARKER_COORDINATES, true);
        marker_add(e->debugMarker);
    }
    return e;
}

void entity_free(Entity *e) {
    if (!e) return;
    if (e->debugMarker) marker_remove(e->debugMarker);
    vkr_texture_release(e->image);
    free(e);
}

void entity_set_image(Entity *e, Texture *image) {
    vkr_texture_retain(image);
    vkr_texture_release(e->image);
    e->image = image;
}

int character_occupied_tiles(Hex h, int size, Hex out[MAX_OCCUPIED]) {
    int n = 0;
    out[n++] = h;
    switch (size) {
    case SIZE_LARGE: /* centre + 2 adjacent tiles */
        out[n++] = hex_neighbor_raw(h, 1);
        out[n++] = hex_neighbor_raw(h, 2);
        break;
    case SIZE_HUGE: /* centre + all adjacent tiles */
        for (int i = 0; i < 6; i++) out[n++] = hex_neighbor_raw(h, i);
        break;
    case SIZE_GARGANTUAN: { /* centre + all adjacent + 5 outer tiles */
        for (int i = 0; i < 6; i++) out[n++] = hex_neighbor_raw(h, i);
        Hex n1 = hex_neighbor_raw(h, 1), n2 = hex_neighbor_raw(h, 2);
        out[n++] = hex_neighbor_raw(n1, 0);
        out[n++] = hex_neighbor_raw(n1, 1);
        out[n++] = hex_neighbor_raw(n1, 2);
        out[n++] = hex_neighbor_raw(n2, 2);
        out[n++] = hex_neighbor_raw(n2, 3);
        break;
    }
    default: /* tiny, small, normal: only the centre tile */
        break;
    }
    return n;
}

int entity_occupied_tiles(const Entity *e, Hex out[MAX_OCCUPIED]) {
    if (e->type == ENTITY_WALL) {
        out[0] = e->tile;
        return 1;
    }
    return character_occupied_tiles(e->tile, e->size, out);
}

bool entity_occupies(const Entity *e, Hex h) {
    Hex t[MAX_OCCUPIED];
    int n = entity_occupied_tiles(e, t);
    for (int i = 0; i < n; i++)
        if (hex_eq(t[i], h)) return true;
    return false;
}

double entity_draw_size(const Entity *e) {
    if (e->type != ENTITY_CHARACTER) return SQRT3;
    switch (e->size) {
    case SIZE_TINY:       return 1;
    case SIZE_SMALL:      return 1.5;
    case SIZE_NORMAL:     return SQRT3;
    case SIZE_LARGE:      return 2;
    case SIZE_HUGE:       return 4;
    case SIZE_GARGANTUAN: return SQRT3 * 5 / 2 + 1;
    default:              return SQRT3;
    }
}

Entity *entity_at(Hex h) {
    for (int i = 0; i < gh->entities.count; i++)
        if (hex_eq(gh->entities.items[i]->tile, h)) return gh->entities.items[i];
    return NULL;
}

void entity_register(Entity *e) {
    e->id = ++gh->nextEntityId;
    da_push(&gh->entities, e);
}

void entity_register_as(Entity *e, uint32_t id) {
    e->id = id;
    if (id > gh->nextEntityId) gh->nextEntityId = id;
    da_push(&gh->entities, e);
}

bool entities_set_ids(const uint32_t *ids, uint32_t n) {
    if (n != (uint32_t)gh->entities.count) return false;
    for (uint32_t i = 0; i < n; i++) {
        if (ids[i] == 0) return false;
        for (uint32_t k = 0; k < i; k++)
            if (ids[k] == ids[i]) return false;
    }
    for (uint32_t i = 0; i < n; i++) {
        gh->entities.items[i]->id = ids[i];
        if (ids[i] > gh->nextEntityId) gh->nextEntityId = ids[i];
    }
    return true;
}

Entity *entity_by_id(uint32_t id) {
    for (int i = 0; i < gh->entities.count; i++)
        if (gh->entities.items[i]->id == id) return gh->entities.items[i];
    return NULL;
}

void entity_delete(Entity *e) {
    if (game_is_in_initiative(e)) game_remove_from_initiative(e);
    da_remove_value(&gh->entities, e);
    entity_free(e);
}

void entities_clear_highlights(void) {
    da_clear(&gh->selectedEntityTiles);
    ts_free(&gh->entityRangeTiles);
    da_clear(&gh->entityPreviewTiles);
    da_clear(&gh->path);
    gh->hasSelectedEntity = false;
}

void entities_delete_type(int type) {
    bool deleted = false;
    for (int i = gh->entities.count - 1; i >= 0; i--) {
        Entity *e = gh->entities.items[i];
        bool match = (type == 0) ? e->type == ENTITY_CHARACTER
                   : (type == 1) ? e->type == ENTITY_WALL
                   : true;
        if (!match) continue;
        entity_delete(e);
        deleted = true;
    }
    if (deleted) entities_clear_highlights(); /* drop highlights that referred to the deleted entities */
}
