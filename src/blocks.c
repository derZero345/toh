/* Blocks (see blocks.h). */
#include "blocks.h"

#include <stdio.h>
#include <string.h>

/* Only placeholders so the framework can be tried out; real blocks (weapons' parries) replace them. */
const BlockActionDef BLOCK_ACTIONS[BLOCK_COUNT] = {
    [BLOCK_TEST] = { "block.test", "Test block", SLOT_MAJOR, 0, 0, 2 },
    [BLOCK_TEST_RANGE] = { "block.test_range", "Test block (range 1)", SLOT_MAJOR, 0, 1, 2 },
};

#define MAX_RANGE 60 /* keeps the range's tile set small whatever a row says */

int block_action_by_id(const char *id) {
    for (int i = 0; id && i < BLOCK_COUNT; i++)
        if (strcmp(BLOCK_ACTIONS[i].id, id) == 0) return i;
    return -1;
}

static bool valid(BlockAction a) { return (int)a >= 0 && a < BLOCK_COUNT; }

static int range_of(BlockAction a) {
    int r = BLOCK_ACTIONS[a].range;
    return r < 0 ? 0 : r > MAX_RANGE ? MAX_RANGE : r;
}

/* ---- tiles ---- */

bool block_in_range(const Entity *e, BlockAction a, Hex tile) {
    if (!e || !valid(a)) return false;
    if (entity_occupies(e, tile)) return true;
    return range_of(a) > 0 && hex_distance(attack_origin(e, tile), tile) <= range_of(a);
}

void block_range(const Entity *e, BlockAction a, TileSet *out) {
    int r = valid(a) ? range_of(a) : 0;
    ts_reset(out, e->tile, r + 1); /* see attack_range */
    for (int row = e->tile.row - r - 1; row <= e->tile.row + r + 1; row++)
        for (int col = e->tile.col - r - 1; col <= e->tile.col + r + 1; col++) {
            Hex h = { row, col };
            if (block_in_range(e, a, h)) ts_add(out, h);
        }
}

bool block_locked(const Entity *e, Hex tile) {
    if (!e || entity_occupies(e, tile)) return false;
    for (int i = 0; i < gh->blockLocks.count; i++) {
        const BlockLock *l = &gh->blockLocks.items[i];
        if (l->blocker == e->id && hex_eq(l->tile, tile)) return true;
    }
    return false;
}

/* ---- checks ---- */

OpResult block_check(const Entity *e, BlockAction a) {
    if (!e || e->type != ENTITY_CHARACTER) return OPR_NO_ENTITY;
    if (!valid(a)) return OPR_INVALID;
    OpResult r = action_turn_check(e, BLOCK_ACTIONS[a].slot);
    if (r != OPR_OK) return r;
    /* test mode: stamina never blocks (moves.h); a character without a stamina stat pays nothing */
    if (!gh->testMode && e->maxStamina > 0 && e->stamina < BLOCK_ACTIONS[a].stamina) return OPR_NO_STAMINA;
    return OPR_OK;
}

OpResult block_check_at(const Entity *e, BlockAction a, Hex tile, int dirs) {
    OpResult r = block_check(e, a);
    if (r != OPR_OK) return r;
    if (dirs == 0 || (dirs & ~BLOCK_DIRS)) return OPR_INVALID;
    if (!block_in_range(e, a, tile)) return OPR_OUT_OF_RANGE;
    /* the lock holds in test mode too (the user's choice) */
    if (block_locked(e, tile)) return OPR_BLOCK_LOCKED;
    return OPR_OK;
}

/* ---- using it ---- */

static void remove_blocks_of(uint32_t id) {
    for (int i = gh->blocks.count - 1; i >= 0; i--)
        if (gh->blocks.items[i].blocker == id) da_remove_at(&gh->blocks, i);
}

static void remove_locks_of(uint32_t id) {
    for (int i = gh->blockLocks.count - 1; i >= 0; i--)
        if (gh->blockLocks.items[i].blocker == id) da_remove_at(&gh->blockLocks, i);
}

void block_dirs_text(int dirs, char *buf, size_t cap) {
    static const AttackDir order[] = { DIR_UP, DIR_LEFT, DIR_RIGHT };
    size_t n = 0;
    buf[0] = '\0';
    for (int k = 0; k < 3; k++)
        if (dirs & DIR_BIT(order[k])) {
            int w = snprintf(buf + n, cap - n, "%s%s", n ? ", " : "", ATTACK_DIR_NAMES[order[k]]);
            if (w < 0 || (size_t)w >= cap - n) return;
            n += (size_t)w;
        }
}

void block_apply(Entity *e, BlockAction a, Hex tile, int dirs) {
    const BlockActionDef *def = &BLOCK_ACTIONS[a];
    action_use_slot(e, def->slot);
    int before;
    bool spent = action_spend_stamina(e, def->stamina, &before);

    if (gh->init.count == 0) { /* no rounds: the newest block replaces the old, and ends its locks */
        remove_blocks_of(e->id);
        remove_locks_of(e->id);
    }
    Hex tiles[MAX_OCCUPIED];
    int n = 1;
    tiles[0] = tile;
    if (def->range == 0) n = entity_occupied_tiles(e, tiles); /* its own tiles: all of them */
    for (int i = 0; i < n; i++) {
        TileBlock b = { .tile = tiles[i], .dirs = dirs, .value = def->value, .blocker = e->id,
                        .occupied = entity_occupies(e, tiles[i]) };
        da_push(&gh->blocks, b);
    }

    char name[CHAR_NAME_MAX + 16], where[48], d[32], stamina[48] = "";
    entity_display_name(e, name, sizeof(name));
    if (n == 1) snprintf(where, sizeof(where), "[%d|%d]", tiles[0].row, tiles[0].col);
    else snprintf(where, sizeof(where), "its %d tiles", n);
    block_dirs_text(dirs, d, sizeof(d));
    if (spent) snprintf(stamina, sizeof(stamina), ", stamina %d/%d", e->stamina, e->maxStamina);
    console_log("%s: %s on %s: %s and neutral, BV %d%s", name, def->name, where, d, def->value, stamina);
    action_end_if_exhausted(e, before, spent);
}

bool block_on_tile(Hex tile, int *dirs, int *value) {
    bool any = false;
    *dirs = 0;
    *value = 0;
    for (int i = 0; i < gh->blocks.count; i++) {
        const TileBlock *b = &gh->blocks.items[i];
        if (!hex_eq(b->tile, tile)) continue;
        *dirs |= b->dirs;
        if (!any || b->value > *value) *value = b->value; /* a tile has a single BV: the highest */
        any = true;
    }
    return any;
}

void block_failed(Hex tile) {
    for (int i = 0; i < gh->blocks.count; i++) {
        const TileBlock *b = &gh->blocks.items[i];
        if (!hex_eq(b->tile, tile) || b->occupied) continue;
        bool found = false;
        for (int k = 0; k < gh->blockLocks.count; k++) {
            BlockLock *l = &gh->blockLocks.items[k];
            if (l->blocker == b->blocker && hex_eq(l->tile, tile)) {
                l->armed = false; /* again: until the end of its next turn */
                found = true;
            }
        }
        if (!found) {
            BlockLock l = { .tile = tile, .blocker = b->blocker, .armed = false };
            da_push(&gh->blockLocks, l);
        }
    }
}

/* ---- lifetime ---- */

void blocks_turn_started(uint32_t id) {
    remove_blocks_of(id);
    for (int i = gh->blockLocks.count - 1; i >= 0; i--) {
        BlockLock *l = &gh->blockLocks.items[i];
        if (l->blocker != id) continue;
        if (l->armed) da_remove_at(&gh->blockLocks, i); /* its next turn is over */
        else l->armed = true;                           /* this is its next turn */
    }
}

void blocks_clear_of(uint32_t id) {
    remove_blocks_of(id);
    remove_locks_of(id);
}

void blocks_clear(void) {
    da_clear(&gh->blocks);
    da_clear(&gh->blockLocks);
}
