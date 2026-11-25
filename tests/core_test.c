/* Tests for the game core without a window: entity ids, op rules (ops_check) and
 * op effects (ops_apply). Run with `make test`. Stubs: tests/core_stubs.c. */
#include "app.h"
#include "image.h"
#include "netmsg.h"
#include "ops.h"
#include "savefmt.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* The wire format (netmsg.h) numbers ops and stats like ops.h, so session.c can convert by value. */
_Static_assert((int)NOP_MOVE == (int)OP_MOVE && (int)NOP_SPAWN == (int)OP_SPAWN && (int)NOP_DELETE == (int)OP_DELETE &&
               (int)NOP_SET_STATS == (int)OP_SET_STATS && (int)NOP_SET_ICON == (int)OP_SET_ICON &&
               (int)NOP_SET_BACKGROUND == (int)OP_SET_BACKGROUND && (int)NOP_GRID_ORIENTATION == (int)OP_GRID_ORIENTATION &&
               (int)NOP_WAYPOINT_SET == (int)OP_WAYPOINT_SET && (int)NOP_WAYPOINT_DELETE == (int)OP_WAYPOINT_DELETE &&
               (int)NOP_INIT_ADD == (int)OP_INIT_ADD && (int)NOP_INIT_REMOVE == (int)OP_INIT_REMOVE &&
               (int)NOP_INIT_CLEAR == (int)OP_INIT_CLEAR && (int)NOP_INIT_SHOW == (int)OP_INIT_SHOW &&
               (int)NOP_INIT_NEXT == (int)OP_INIT_NEXT && (int)NOP_SET_OWNER == (int)OP_SET_OWNER && (int)NOP_ROLL == (int)OP_COUNT,
               "NetOpType must match OpType");
_Static_assert((int)NSTAT_SIZE == (int)STAT_SIZE && (int)NSTAT_MAXHEALTH == (int)STAT_MAXHEALTH &&
               (int)NSTAT_SPEED == (int)STAT_SPEED && (int)NSTAT_INITIATIVE == (int)STAT_INITIATIVE, "stat bits must match");
_Static_assert((int)OPR_INVALID <= (int)NETMSG_MAX_REASON, "OpResult must fit OP_REJECTED");
_Static_assert(PLAYER_NAME_MAX == SAVEFMT_OWNER_MAX && WAYPOINT_NAME_MAX == SAVEFMT_NAME_MAX, "name sizes");

extern char stub_last_log[1024];
extern Texture *stub_background;
extern int stub_bad_releases;
int stub_refs(Texture *t);

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
    Entity *e = entity_new_character(TEX_A, tile, SIZE_NORMAL, 10, speed, 0);
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

/* ---- ops_apply ------------------------------------------------------------------------- */

static void test_apply_entities(void) {
    reset_table();
    Op spawn = { .type = OP_SPAWN };
    da_push(&spawn.specs, ((EntitySpec){ .type = ENTITY_CHARACTER, .image = TEX_A, .tile = { 5, 5 },
                                         .size = SIZE_NORMAL, .maxHealth = 20, .health = 7, .attack = 1,
                                         .speed = 4, .initiative = 2 }));
    da_push(&spawn.specs, ((EntitySpec){ .type = ENTITY_WALL, .image = TEX_B, .tile = { 5, 6 } }));
    int a0 = stub_refs(TEX_A), b0 = stub_refs(TEX_B);
    CHECK_R(ops_submit(&spawn), OPR_OK);
    CHECK(stub_refs(TEX_A) == a0 + 1 && stub_refs(TEX_B) == b0 + 1);
    CHECK(spawn.ids.count == 2 && gh->entities.count == 2);
    Entity *hero = entity_by_id(spawn.ids.items[0]);
    Entity *wall = entity_by_id(spawn.ids.items[1]);
    CHECK(hero && hero->type == ENTITY_CHARACTER && hero->image == TEX_A && hex_eq(hero->tile, (Hex){ 5, 5 }));
    CHECK(hero && hero->maxHealth == 20 && hero->health == 7 && hero->attack == 1 &&
          hero->speed == 4 && hero->initiative == 2);
    CHECK(wall && wall->type == ENTITY_WALL && wall->image == TEX_B && hex_eq(wall->tile, (Hex){ 5, 6 }));
    op_free(&spawn);
    if (!hero || !wall) return;

    Op mv = move_op(hero, (Hex){ 7, 5 });
    CHECK_R(ops_submit(&mv), OPR_OK);
    CHECK(hex_eq(hero->tile, (Hex){ 7, 5 }));

    /* only the masked stats change, speed is capped, walls are skipped */
    Op st = { .type = OP_SET_STATS, .statMask = STAT_SPEED | STAT_INITIATIVE, .speed = 5000, .initiative = 9, .maxHealth = 1 };
    da_push(&st.ids, hero->id);
    da_push(&st.ids, wall->id);
    CHECK_R(ops_submit(&st), OPR_OK);
    CHECK(hero->speed == MAX_SPEED && hero->initiative == 9 && hero->maxHealth == 20);
    op_free(&st);

    Op icon = { .type = OP_SET_ICON, .image = TEX_B };
    da_push(&icon.ids, hero->id);
    a0 = stub_refs(TEX_A);
    b0 = stub_refs(TEX_B);
    CHECK_R(ops_submit(&icon), OPR_OK);
    CHECK(hero->image == TEX_B);
    CHECK(stub_refs(TEX_A) == a0 - 1 && stub_refs(TEX_B) == b0 + 1);
    op_free(&icon);

    Op own = { .type = OP_SET_OWNER, .name = "Alice" };
    da_push(&own.ids, hero->id);
    da_push(&own.ids, wall->id);
    CHECK_R(ops_submit(&own), OPR_OK);
    CHECK(strcmp(hero->owner, "Alice") == 0 && wall->owner[0] == '\0');
    own.name[0] = '\0';
    CHECK_R(ops_submit(&own), OPR_OK);
    CHECK(hero->owner[0] == '\0');
    op_free(&own);

    Op del = { .type = OP_DELETE };
    da_push(&del.ids, wall->id);
    uint32_t wall_id = wall->id;
    b0 = stub_refs(TEX_B);
    CHECK_R(ops_submit(&del), OPR_OK);
    CHECK(gh->entities.count == 1 && entity_by_id(wall_id) == NULL);
    CHECK(stub_refs(TEX_B) == b0 - 1);
    op_free(&del);

    /* the same id twice: the second lookup finds nothing */
    Op twice = { .type = OP_DELETE };
    da_push(&twice.ids, hero->id);
    da_push(&twice.ids, hero->id);
    CHECK_R(ops_submit(&twice), OPR_OK);
    CHECK(gh->entities.count == 0);
    op_free(&twice);

    /* rejected ops change nothing and tell the user why */
    Op bad = { .type = OP_MOVE, .id = 4242, .tile = { 0, 0 } };
    CHECK_R(ops_submit(&bad), OPR_NO_ENTITY);
    CHECK(strcmp(stub_last_log, "That entity no longer exists") == 0);
}

static void test_apply_world(void) {
    reset_table();
    int a0 = stub_refs(TEX_A);
    Op bg = { .type = OP_SET_BACKGROUND, .image = TEX_A };
    CHECK_R(ops_submit(&bg), OPR_OK);
    CHECK(stub_background == TEX_A && stub_refs(TEX_A) == a0 + 1);
    bg.image = NULL;
    CHECK_R(ops_submit(&bg), OPR_OK);
    CHECK(stub_background == NULL && stub_refs(TEX_A) == a0);

    bool flat = gh->isFlat;
    Op grid = { .type = OP_GRID_ORIENTATION };
    CHECK_R(ops_submit(&grid), OPR_OK);
    CHECK(gh->isFlat != flat);

    Op set = { .type = OP_WAYPOINT_SET, .tile = { 1, 2 }, .name = "Camp" };
    CHECK_R(ops_submit(&set), OPR_OK);
    CHECK(gh->waypoints.count == 1 && strcmp(gh->waypoints.items[0].name, "Camp") == 0);
    Op move = { .type = OP_WAYPOINT_SET, .tile = { 3, 4 }, .name = "camp" };
    CHECK_R(ops_submit(&move), OPR_OK); /* same waypoint: keeps its name, moves */
    CHECK(gh->waypoints.count == 1 && strcmp(gh->waypoints.items[0].name, "Camp") == 0);
    CHECK(hex_eq(gh->waypoints.items[0].tile, (Hex){ 3, 4 }));
    Op del = { .type = OP_WAYPOINT_DELETE, .name = "CAMP" };
    CHECK_R(ops_submit(&del), OPR_OK);
    CHECK(gh->waypoints.count == 0);
}

static void test_apply_initiative(void) {
    reset_table();
    Entity *a = add_character((Hex){ 0, 0 }, 3, "");
    Entity *b = add_character((Hex){ 0, 4 }, 3, "");
    Entity *c = add_character((Hex){ 0, 8 }, 3, "");
    Op add = { .type = OP_INIT_ADD };
    Entity *es[3] = { a, b, c };
    double vals[3] = { 0.2, 0.9, 0.5 };
    for (int i = 0; i < 3; i++) {
        da_push(&add.ids, es[i]->id);
        da_push(&add.values, vals[i]);
    }
    CHECK_R(ops_submit(&add), OPR_OK);
    CHECK(gh->init.count == 3 && gh->init.items[0].entity == b && gh->init.items[2].entity == a);
    op_free(&add);

    Op next = { .type = OP_INIT_NEXT };
    CHECK_R(ops_submit(&next), OPR_OK);
    CHECK(gh->currentTurn == b); /* order is b, c, a; the first next starts with the highest roller */

    /* an op that names the current turn advances it */
    Op next2 = { .type = OP_INIT_NEXT, .id = gh->currentTurn->id };
    CHECK_R(ops_submit(&next2), OPR_OK);
    CHECK(gh->currentTurn == c);
    Op stale = { .type = OP_INIT_NEXT, .id = b->id }; /* b's turn is over */
    CHECK_R(ops_submit(&stale), OPR_STALE);
    Op next3 = { .type = OP_INIT_NEXT, .id = gh->currentTurn->id };
    CHECK_R(ops_submit(&next3), OPR_OK);
    CHECK(gh->currentTurn == a);
    Op next4 = { .type = OP_INIT_NEXT, .id = gh->currentTurn->id };
    CHECK_R(ops_submit(&next4), OPR_OK);
    CHECK(gh->currentTurn == b); /* full circle: b is current again */

    /* removing the current one passes the turn to the next in order, not the one after */
    Op rm = { .type = OP_INIT_REMOVE };
    da_push(&rm.ids, b->id);
    CHECK_R(ops_submit(&rm), OPR_OK);
    CHECK(gh->init.count == 2 && !game_is_in_initiative(b) && gh->currentTurn == c);
    op_free(&rm);
    /* removing the last one in order while it is current wraps around */
    Op next5 = { .type = OP_INIT_NEXT, .id = gh->currentTurn->id };
    CHECK_R(ops_submit(&next5), OPR_OK);
    CHECK(gh->currentTurn == a);
    Op rm2 = { .type = OP_INIT_REMOVE };
    da_push(&rm2.ids, a->id);
    CHECK_R(ops_submit(&rm2), OPR_OK);
    CHECK(gh->init.count == 1 && gh->currentTurn == c && gh->currentTurnIndex == 0);
    op_free(&rm2);
    /* removing one before the current keeps the current */
    Op add2 = { .type = OP_INIT_ADD };
    da_push(&add2.ids, a->id);
    da_push(&add2.values, 0.95); /* order a, c; c is current */
    da_push(&add2.ids, b->id);
    da_push(&add2.values, 0.05); /* order a, c, b */
    CHECK_R(ops_submit(&add2), OPR_OK);
    op_free(&add2);
    CHECK(gh->currentTurn == c && gh->currentTurnIndex == 1); /* the index follows c to its new place */

    /* deleting an entity also takes it out of the initiative */
    Op del = { .type = OP_DELETE };
    da_push(&del.ids, c->id);
    CHECK_R(ops_submit(&del), OPR_OK);
    CHECK(gh->init.count == 2 && gh->init.items[0].entity == a);
    CHECK(gh->currentTurn == b); /* c was current; removing it passes the turn on */
    op_free(&del);

    Op show = { .type = OP_INIT_SHOW };
    bool visible = gh->initMarkers.items[0]->visible;
    CHECK_R(ops_submit(&show), OPR_OK);
    CHECK(gh->initMarkers.items[0]->visible != visible);

    Op clear = { .type = OP_INIT_CLEAR };
    CHECK_R(ops_submit(&clear), OPR_OK);
    CHECK(gh->init.count == 0 && gh->currentTurn == NULL);
}

extern bool stub_client;
extern int stub_sent_ops;

static void test_host_ids(void) {
    reset_table();
    /* spawn ops broadcast by the host create the entities with the host's ids */
    Op op = { .type = OP_SPAWN, .keepIds = true };
    da_push(&op.specs, ((EntitySpec){ .type = ENTITY_CHARACTER, .image = TEX_A, .tile = { 1, 1 }, .speed = 3 }));
    da_push(&op.specs, ((EntitySpec){ .type = ENTITY_WALL, .image = TEX_B, .tile = { 2, 2 } }));
    da_push(&op.ids, 500u);
    da_push(&op.ids, 77u);
    ops_apply(&op);
    CHECK(op.ids.count == 2 && op.ids.items[0] == 500 && op.ids.items[1] == 77);
    CHECK(entity_by_id(500) && entity_by_id(500)->type == ENTITY_CHARACTER && entity_by_id(77));
    op_free(&op);
    /* later local ids do not collide with them */
    Entity *c = add_character((Hex){ 3, 3 }, 3, "");
    CHECK(c->id > 500);

    /* the snapshot's entities get the host's ids in order */
    uint32_t ids[3] = { 9, 10, 11 }, dup[3] = { 9, 9, 11 };
    CHECK(!entities_set_ids(ids, 2));  /* wrong count */
    CHECK(!entities_set_ids(dup, 3));  /* duplicate */
    CHECK(entities_set_ids(ids, 3));
    CHECK(app.entities.items[0]->id == 9 && app.entities.items[2]->id == 11 && entity_by_id(500) == NULL);
}

static void test_remote_view_state(void) {
    reset_table();
    Entity *a = add_character((Hex){ 0, 0 }, 3, "bob");
    Entity *b = add_character((Hex){ 5, 5 }, 3, "");
    /* a is selected locally and shows a path */
    app.hasSelectedEntity = true;
    da_clear(&app.selectedEntityTiles);
    da_push(&app.selectedEntityTiles, a->tile);
    da_clear(&app.path);
    da_push(&app.path, ((Hex){ 0, 1 }));

    /* someone else moves b: the local path and selection stay */
    Op mv = { .type = OP_MOVE, .id = b->id, .tile = { 5, 6 }, .remote = true };
    ops_apply(&mv);
    CHECK(b->tile.col == 6 && app.path.count == 1 && app.hasSelectedEntity);

    /* someone else deletes b: still nothing to clear */
    Op del = { .type = OP_DELETE };
    da_push(&del.ids, b->id);
    del.remote = true;
    ops_apply(&del);
    CHECK(app.hasSelectedEntity && app.selectedEntityTiles.count == 1);
    op_free(&del);

    /* someone else moves the selected entity: its highlights go */
    Op mv2 = { .type = OP_MOVE, .id = a->id, .tile = { 0, 1 }, .remote = true };
    ops_apply(&mv2);
    CHECK(!app.hasSelectedEntity && app.selectedEntityTiles.count == 0);
}

static void test_client_submit(void) {
    reset_table();
    Entity *a = add_character((Hex){ 0, 0 }, 3, "Alice");
    Entity *b = add_character((Hex){ 4, 4 }, 3, "");
    stub_client = true;
    int sent = stub_sent_ops;
    /* a client checks locally, sends, and does not apply yet */
    Op ok = { .type = OP_MOVE, .id = a->id, .tile = { 0, 1 } };
    CHECK_R(ops_submit(&ok), OPR_OK);
    CHECK(stub_sent_ops == sent + 1 && a->tile.col == 0);
    /* refused ops are not sent */
    Op other = { .type = OP_MOVE, .id = b->id, .tile = { 4, 5 } };
    CHECK_R(ops_submit(&other), OPR_NOT_YOURS);
    CHECK(stub_sent_ops == sent + 1);
    stub_client = false;
    CHECK(strcmp(ops_result_text(OPR_ALREADY_OWNED), "That character already has an owner") == 0);
}

static void test_create_helpers(void) {
    CHECK(strcmp(ops_result_text(OPR_TILE_TAKEN), "That tile is taken") == 0);
    Image src = { .w = 1000, .h = 600, .px = calloc(1000 * 600, 4) };
    for (int i = 0; i < 1000 * 600; i++) src.px[i * 4 + 3] = 255;
    Image s = image_scale_to_fit(&src, 512);
    CHECK(s.w == 512 && s.h == 307 && s.px && s.px[3] == 255 && s.px[(512 * 307 - 1) * 4 + 3] == 255);
    image_free(&s);
    Image small = image_scale_to_fit(&(Image){ .w = 10, .h = 20, .px = src.px }, 512);
    CHECK(small.w == 10 && small.h == 20 && memcmp(small.px, src.px, 10 * 20 * 4) == 0);
    image_free(&small);
    image_free(&src);
}

int main(void) {
    test_entity_ids();
    test_initiative_values();
    test_player_moves();
    test_turns();
    test_gm_only();
    test_structure();
    test_apply_entities();
    test_apply_world();
    test_apply_initiative();
    test_host_ids();
    test_remote_view_state();
    test_client_submit();
    test_create_helpers();
    CHECK(stub_bad_releases == 0);
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
