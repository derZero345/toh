/* Tests for the game core without a window: entity ids, op rules (ops_check) and
 * op effects (ops_apply). Run with `make test`. Stubs: tests/core_stubs.c. */
#include "app.h"
#include "ops.h"
#include "savefmt.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

extern char stub_last_log[1024];
extern Texture *stub_background;

static int failures, checks;

#define CHECK(cond) do {                                                       \
        checks++;                                                              \
        if (!(cond)) {                                                         \
            failures++;                                                        \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                      \
    } while (0)

#define CHECK_R(expr, want) do {                                               \
        OpResult got_ = (expr);                                                \
        checks++;                                                              \
        if (got_ != (want)) {                                                  \
            failures++;                                                        \
            fprintf(stderr, "%s:%d: %s = \"%s\", expected \"%s\"\n", __FILE__, __LINE__, #expr, \
                    ops_result_text(got_), ops_result_text(want));             \
        }                                                                      \
    } while (0)

/* Textures are opaque and the stubs never touch them, so any distinct addresses do. */
static char tex_storage[2];
#define TEX_A ((Texture *)&tex_storage[0])
#define TEX_B ((Texture *)&tex_storage[1])

static App app;

static void reset_table(void) {
    gh = &app;
    entities_delete_type(2);
    game_clear_initiative();
    da_clear(&app.waypoints);
    app.hexSize = 40;
    app.isFlat = true;
}

static Entity *add_character(Hex tile, int speed, const char *owner) {
    Entity *e = entity_new_character(TEX_A, tile, SIZE_NORMAL, 10, 12, speed, 0);
    snprintf(e->owner, sizeof(e->owner), "%s", owner);
    entity_register(e);
    return e;
}

static Entity *add_wall(Hex tile) {
    Entity *e = entity_new_wall(TEX_B, tile);
    entity_register(e);
    return e;
}

/* ---- entity ids ------------------------------------------------------------------------ */

static void test_entity_ids(void) {
    reset_table();
    Entity *a = add_character((Hex){ 0, 0 }, 3, "");
    Entity *b = add_wall((Hex){ 0, 1 });
    CHECK(a->id != 0 && b->id != 0 && a->id != b->id);
    CHECK(entity_by_id(a->id) == a && entity_by_id(b->id) == b);
    CHECK(entity_by_id(0) == NULL);
    Entity *clone = entity_clone(a, false);
    CHECK(clone->id == 0 && strcmp(clone->owner, a->owner) == 0);
    entity_register(clone);
    CHECK(clone->id != 0 && clone->id != a->id && entity_by_id(clone->id) == clone);
    uint32_t old = b->id;
    entities_delete_type(1);
    CHECK(entity_by_id(old) == NULL && entity_by_id(a->id) == a);
    Entity *c = add_wall((Hex){ 0, 1 });
    CHECK(c->id != old); /* ids are never reused */
}

static void test_initiative_values(void) {
    reset_table();
    Entity *a = add_character((Hex){ 0, 0 }, 3, "");
    Entity *b = add_character((Hex){ 0, 4 }, 3, "");
    Entity *c = add_character((Hex){ 0, 8 }, 3, "");
    Entity *order[3] = { a, b, c };
    double values[3] = { 0.2, 0.9, 0.5 };
    game_initiative_list(order, values, 3);
    /* highest roll first */
    CHECK(gh->init.count == 3);
    CHECK(gh->init.items[0].entity == b && gh->init.items[1].entity == c && gh->init.items[2].entity == a);
    CHECK(gh->init.items[0].value == 0.9);
    /* entities already in the initiative keep their roll */
    double again[3] = { 0.99, 0.0, 0.0 };
    game_initiative_list(order, again, 3);
    CHECK(gh->init.count == 3 && gh->init.items[0].entity == b && gh->init.items[2].value == 0.2);
}

/* ---- ops_check ------------------------------------------------------------------------- */

static const OpAuthor GM = { .gm = true };

static OpAuthor player(const char *name) {
    OpAuthor a = { .gm = false };
    snprintf(a.name, sizeof(a.name), "%s", name);
    return a;
}

/* the tile n steps from h in neighbour direction dir */
static Hex steps(Hex h, int dir, int n) {
    for (int i = 0; i < n; i++) h = hex_neighbor_raw(h, dir);
    return h;
}

static Op move_op(const Entity *e, Hex to) { return (Op){ .type = OP_MOVE, .id = e->id, .tile = to }; }

static void test_player_moves(void) {
    reset_table();
    Hex start = { 10, 10 };
    Entity *hero = add_character(start, 3, "Alice");
    Entity *orc = add_character(steps(start, 3, 4), 3, "");
    OpAuthor alice = player("Alice"), bob = player("Bob"), lower = player("alice"), nobody = player("");
    Op near = move_op(hero, steps(start, 0, 2));
    Op far = move_op(hero, steps(start, 0, 5));
    CHECK(hex_distance(start, near.tile) == 2 && hex_distance(start, far.tile) == 5);

    CHECK_R(ops_check(&near, &alice), OPR_OK);
    CHECK_R(ops_check(&near, &lower), OPR_OK); /* names compare case-insensitively */
    CHECK_R(ops_check(&near, &bob), OPR_NOT_YOURS);
    CHECK_R(ops_check(&far, &alice), OPR_OUT_OF_RANGE);
    CHECK_R(ops_check(&far, &GM), OPR_OK); /* the GM is not limited */

    Op orc_move = move_op(orc, steps(orc->tile, 0, 1));
    CHECK_R(ops_check(&orc_move, &alice), OPR_NOT_YOURS); /* unowned characters belong to the GM */
    CHECK_R(ops_check(&orc_move, &nobody), OPR_NOT_YOURS);
    CHECK_R(ops_check(&orc_move, &GM), OPR_OK);

    /* a wall blocks its tile */
    Hex blocked = steps(start, 0, 1);
    Entity *wall = add_wall(blocked);
    Op into_wall = move_op(hero, blocked);
    CHECK_R(ops_check(&into_wall, &alice), OPR_OUT_OF_RANGE);

    /* only characters move; unknown ids are rejected for everyone */
    Op wall_move = move_op(wall, start);
    CHECK_R(ops_check(&wall_move, &GM), OPR_NO_ENTITY);
    Op gone = { .type = OP_MOVE, .id = 9999, .tile = start };
    CHECK_R(ops_check(&gone, &GM), OPR_NO_ENTITY);
}

static void test_turns(void) {
    reset_table();
    Entity *hero = add_character((Hex){ 10, 10 }, 3, "Alice");
    Entity *orc = add_character((Hex){ 20, 20 }, 3, "Bob");
    OpAuthor alice = player("Alice"), bob = player("Bob");
    Op next = { .type = OP_INIT_NEXT };

    /* no initiative: free movement, but there is no turn to pass */
    Op hero_move = move_op(hero, hex_neighbor_raw(hero->tile, 0));
    CHECK_R(ops_check(&hero_move, &alice), OPR_OK);
    CHECK_R(ops_check(&next, &alice), OPR_NOT_YOUR_TURN);

    Entity *order[2] = { hero, orc };
    double values[2] = { 0.9, 0.1 };
    game_initiative_list(order, values, 2);
    /* initiative is set up, but the first turn has not started */
    CHECK_R(ops_check(&hero_move, &alice), OPR_NOT_YOUR_TURN);
    CHECK_R(ops_check(&next, &alice), OPR_NOT_YOUR_TURN);
    CHECK_R(ops_check(&next, &GM), OPR_OK);

    game_next_turn();
    Entity *cur = gh->currentTurn;
    CHECK(cur == hero || cur == orc);
    Entity *other = cur == hero ? orc : hero;
    OpAuthor cur_owner = cur == hero ? alice : bob, other_owner = cur == hero ? bob : alice;
    Op cur_move = move_op(cur, hex_neighbor_raw(cur->tile, 0));
    Op other_move = move_op(other, hex_neighbor_raw(other->tile, 0));
    CHECK_R(ops_check(&cur_move, &cur_owner), OPR_OK);
    CHECK_R(ops_check(&other_move, &other_owner), OPR_NOT_YOUR_TURN);
    CHECK_R(ops_check(&other_move, &GM), OPR_OK);
    next.id = gh->currentTurn ? gh->currentTurn->id : 0;
    CHECK_R(ops_check(&next, &cur_owner), OPR_OK);
    CHECK_R(ops_check(&next, &other_owner), OPR_NOT_YOUR_TURN);

    /* an op built for an earlier turn is stale */
    Op stale = { .type = OP_INIT_NEXT, .id = 0 };
    CHECK_R(ops_check(&stale, &GM), OPR_STALE);
    CHECK_R(ops_check(&stale, &cur_owner), OPR_STALE);

    /* a player cannot pass the turn of an unowned character */
    Entity *npc = add_character((Hex){ 30, 30 }, 3, "");
    gh->currentTurn = npc;
    Op next_npc = { .type = OP_INIT_NEXT, .id = npc->id };
    CHECK_R(ops_check(&next_npc, &alice), OPR_NOT_YOUR_TURN);
    CHECK_R(ops_check(&next_npc, &GM), OPR_OK);
}

static void test_gm_only(void) {
    reset_table();
    Entity *hero = add_character((Hex){ 10, 10 }, 3, "Alice");
    OpAuthor alice = player("Alice");
    Op ops[] = {
        { .type = OP_SPAWN },
        { .type = OP_DELETE },
        { .type = OP_SET_STATS },
        { .type = OP_SET_ICON, .image = TEX_A },
        { .type = OP_SET_BACKGROUND },
        { .type = OP_GRID_ORIENTATION },
        { .type = OP_WAYPOINT_SET, .name = "camp" },
        { .type = OP_WAYPOINT_DELETE, .name = "camp" },
        { .type = OP_INIT_ADD },
        { .type = OP_INIT_REMOVE },
        { .type = OP_INIT_CLEAR },
        { .type = OP_INIT_SHOW },
        { .type = OP_SET_OWNER, .name = "Alice" },
    };
    for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
        Op *op = &ops[i];
        if (op->type == OP_DELETE || op->type == OP_SET_STATS || op->type == OP_SET_ICON ||
            op->type == OP_INIT_ADD || op->type == OP_INIT_REMOVE || op->type == OP_SET_OWNER)
            da_push(&op->ids, hero->id);
        if (op->type == OP_INIT_ADD) da_push(&op->values, 0.5);
        CHECK_R(ops_check(op, &alice), OPR_GM_ONLY);
        CHECK_R(ops_check(op, &GM), OPR_OK);
        op_free(op);
    }
}

static void test_structure(void) {
    reset_table();
    Entity *hero = add_character((Hex){ 10, 10 }, 3, "");

    Op del = { .type = OP_DELETE };
    da_push(&del.ids, hero->id);
    da_push(&del.ids, 12345);
    CHECK_R(ops_check(&del, &GM), OPR_NO_ENTITY);
    op_free(&del);

    Op add = { .type = OP_INIT_ADD };
    da_push(&add.ids, hero->id);
    CHECK_R(ops_check(&add, &GM), OPR_INVALID); /* no roll for it */
    da_push(&add.values, NAN);
    CHECK_R(ops_check(&add, &GM), OPR_INVALID);
    add.values.items[0] = 0.5;
    CHECK_R(ops_check(&add, &GM), OPR_OK);
    op_free(&add);

    Op own = { .type = OP_SET_OWNER, .name = "two words" };
    da_push(&own.ids, hero->id);
    CHECK_R(ops_check(&own, &GM), OPR_INVALID);
    snprintf(own.name, sizeof(own.name), "Alice");
    CHECK_R(ops_check(&own, &GM), OPR_OK);
    own.name[0] = '\0';
    CHECK_R(ops_check(&own, &GM), OPR_OK); /* "" removes the owner */
    op_free(&own);

    Op wp = { .type = OP_WAYPOINT_SET };
    CHECK_R(ops_check(&wp, &GM), OPR_INVALID); /* no name */
    memset(wp.name, 'x', sizeof(wp.name));
    CHECK_R(ops_check(&wp, &GM), OPR_INVALID); /* not terminated */

    Op spawn = { .type = OP_SPAWN };
    da_push(&spawn.specs, ((EntitySpec){ .type = ENTITY_WALL, .image = NULL }));
    CHECK_R(ops_check(&spawn, &GM), OPR_INVALID); /* no image */
    op_free(&spawn);

    Op icon = { .type = OP_SET_ICON };
    da_push(&icon.ids, hero->id);
    CHECK_R(ops_check(&icon, &GM), OPR_INVALID); /* no image */
    op_free(&icon);

    Op bogus = { .type = OP_COUNT };
    CHECK_R(ops_check(&bogus, &GM), OPR_INVALID);
    Op neg = { .type = (OpType)-1 }, huge = { .type = (OpType)1000 };
    CHECK_R(ops_check(&neg, &GM), OPR_INVALID);
    CHECK_R(ops_check(&huge, &GM), OPR_INVALID);

    Op inf = { .type = OP_INIT_ADD };
    da_push(&inf.ids, hero->id);
    da_push(&inf.values, INFINITY);
    CHECK_R(ops_check(&inf, &GM), OPR_INVALID);
    op_free(&inf);

    Op badtype = { .type = OP_SPAWN };
    da_push(&badtype.specs, ((EntitySpec){ .type = (EntityType)7, .image = TEX_A }));
    CHECK_R(ops_check(&badtype, &GM), OPR_INVALID);
    op_free(&badtype);

    /* untrusted input: players cannot probe ids, names must be terminated, lists are capped */
    OpAuthor alice = player("Alice");
    Op pdel = { .type = OP_DELETE };
    da_push(&pdel.ids, 777777);
    CHECK_R(ops_check(&pdel, &alice), OPR_GM_ONLY);
    op_free(&pdel);

    OpAuthor loose = { .gm = true };
    memset(loose.name, 'x', sizeof(loose.name));
    Op any = { .type = OP_INIT_CLEAR };
    CHECK_R(ops_check(&any, &loose), OPR_INVALID);

    Op big = { .type = OP_DELETE };
    big.ids.count = SAVEFMT_MAX_ENTITIES + 1;
    CHECK_R(ops_check(&big, &GM), OPR_INVALID);
    big.ids.count = 0;
    op_free(&big);

    Op oob_move = move_op(hero, (Hex){ SAVEFMT_MAX_COORD + 1, 0 });
    CHECK_R(ops_check(&oob_move, &GM), OPR_INVALID);
    Op oob_wp = { .type = OP_WAYPOINT_SET, .name = "x", .tile = { 0, -SAVEFMT_MAX_COORD - 1 } };
    CHECK_R(ops_check(&oob_wp, &GM), OPR_INVALID);
    Op oob_spawn = { .type = OP_SPAWN };
    da_push(&oob_spawn.specs, ((EntitySpec){ .type = ENTITY_WALL, .image = TEX_A, .tile = { INT_MAX, 0 } }));
    CHECK_R(ops_check(&oob_spawn, &GM), OPR_INVALID);
    op_free(&oob_spawn);
}

int main(void) {
    test_entity_ids();
    test_initiative_values();
    test_player_moves();
    test_turns();
    test_gm_only();
    test_structure();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
