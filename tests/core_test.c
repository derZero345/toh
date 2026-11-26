/* Tests for the game core without a window: entity ids, op rules (ops_check) and
 * op effects (ops_apply). Run with `make test`. Stubs: tests/core_stubs.c. */
#include "app.h"
#include "baranim.h"
#include "hud.h"
#include "hud_game.h"
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
               (int)NOP_INIT_NEXT == (int)OP_INIT_NEXT && (int)NOP_SET_OWNER == (int)OP_SET_OWNER && (int)NOP_SET_HEALTH == (int)OP_SET_HEALTH &&
               (int)NOP_SET_NAME == (int)OP_SET_NAME && (int)NOP_SET_STAMINA == (int)OP_SET_STAMINA &&
               (int)NOP_ROLL == (int)OP_COUNT,
               "NetOpType must match OpType");
_Static_assert((int)NSTAT_SIZE == (int)STAT_SIZE && (int)NSTAT_MAXHEALTH == (int)STAT_MAXHEALTH &&
               (int)NSTAT_SPEED == (int)STAT_SPEED && (int)NSTAT_INITIATIVE == (int)STAT_INITIATIVE, "stat bits must match");
_Static_assert((int)NSTAT_MAXSTAMINA == (int)STAT_MAXSTAMINA, "stamina bit");
_Static_assert(SIZE_MIN == SAVEFMT_MIN_SIZE && SIZE_MAX_CLASS == SAVEFMT_MAX_SIZE, "size limits must match");
_Static_assert((int)OPR_LAST <= (int)NETMSG_MAX_REASON, "OpResult must fit OP_REJECTED");
_Static_assert(PLAYER_NAME_MAX == SAVEFMT_OWNER_MAX && WAYPOINT_NAME_MAX == SAVEFMT_NAME_MAX, "name sizes");
_Static_assert(CHAR_NAME_MAX == SAVEFMT_CHAR_NAME_MAX, "character name sizes");

extern char stub_last_log[1024];
extern int stub_log_count;
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
    da_clear(&app.selectedEntities);
    da_push(&app.selectedEntities, a->id);
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
    CHECK(app.hasSelectedEntity && app.selectedEntities.count == 1);
    op_free(&del);

    /* someone else moves the selected entity: its highlights go */
    Op mv2 = { .type = OP_MOVE, .id = a->id, .tile = { 0, 1 }, .remote = true };
    ops_apply(&mv2);
    CHECK(!app.hasSelectedEntity && app.selectedEntities.count == 0);
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
    /* spending is left to the host: an earlier "recover" in the same command may not be applied here yet */
    a->maxStamina = 9;
    a->stamina = 0;
    Op spend = { .type = OP_SET_STAMINA, .healthMode = STAMINA_SPEND, .value = 3 };
    da_push(&spend.ids, a->id);
    CHECK_R(ops_submit(&spend), OPR_OK);
    CHECK(stub_sent_ops == sent + 2 && a->stamina == 0);
    op_free(&spend);
    stub_client = false;
    CHECK(strcmp(ops_result_text(OPR_ALREADY_OWNED), "That character already has an owner") == 0);
}

static void test_create_helpers(void) {
    CHECK(strcmp(ops_result_text(OPR_TILE_TAKEN), "No room on that tile") == 0);
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

/* Tales of Honor footprints: Wee to Big one tile, Huge a triangle of 3, Gigantic a tile and its 6 neighbours */
static void test_size_footprints(void) {
    const int want[] = { 1, 1, 1, 1, 1, 3, 7 };
    for (int size = SIZE_MIN; size <= SIZE_MAX_CLASS; size++) {
        Hex t[MAX_OCCUPIED];
        int n = character_occupied_tiles((Hex){ 4, 4 }, size, t);
        CHECK(n == want[size - SIZE_MIN] && hex_eq(t[0], (Hex){ 4, 4 }));
        for (int i = 1; i < n; i++) CHECK(hex_distance(t[i], (Hex){ 4, 4 }) == 1);
    }
}

/* Tales of Honor: a tile holds 5 bulk (Wee 0, Tiny 1, Small 2, Normal 3, Big 4; Huge 3 per tile; Gigantic 5 in
 * the centre, 3 on the edges); walls fill their tile */
static void test_tile_sharing(void) {
    reset_table();
    Hex t = { 0, 0 };
    Entity *a = add_character(t, 3, ""); /* Normal: 3 */
    CHECK(character_fits(t, SIZE_SMALL, NULL) && character_fits(t, SIZE_WEE, NULL));
    CHECK(!character_fits(t, SIZE_NORMAL, NULL) && !character_fits(t, SIZE_BIG, NULL));
    CHECK(character_fits(t, SIZE_BIG, a)); /* a itself is not counted */
    add_wall((Hex){ 0, 2 });
    CHECK(!character_fits((Hex){ 0, 2 }, SIZE_WEE, NULL));

    Hex g = { 10, 10 };
    Entity *big = add_character(g, 3, "");
    big->size = SIZE_GIGANTIC;
    Hex edge = hex_neighbor_raw(g, 0);
    CHECK(character_fits(g, SIZE_WEE, NULL) && !character_fits(g, SIZE_TINY, NULL));
    CHECK(character_fits(edge, SIZE_SMALL, NULL) && !character_fits(edge, SIZE_NORMAL, NULL));
    big->size = SIZE_HUGE; /* a triangle, 3 on each tile */
    CHECK(character_fits(g, SIZE_SMALL, NULL) && !character_fits(g, SIZE_NORMAL, NULL));
    CHECK(!character_fits(hex_neighbor_raw(g, 1), SIZE_NORMAL, NULL) && character_fits(hex_neighbor_raw(g, 3), SIZE_BIG, NULL));

    /* moving: a Small character may step onto a's tile and walk through it, a Normal one may not */
    Hex left = hex_neighbor_raw(t, 3), right = hex_neighbor_raw(t, 0);
    Entity *s = add_character(left, 2, "");
    s->size = SIZE_SMALL;
    TileSet r = { 0 };
    astar_range(s->tile, 2, s, &r);
    CHECK(ts_has(&r, t) && ts_has(&r, right));
    ts_free(&r);
    HexList path = { 0 };
    CHECK(astar_path_within(left, right, 2, s, &path) && path.count == 3 && hex_eq(path.items[1], t));
    s->size = SIZE_NORMAL;
    astar_range(s->tile, 2, s, &r);
    CHECK(!ts_has(&r, t));
    ts_free(&r);
    CHECK(!astar_path_within(left, right, 2, s, &path));
    da_free(&path);

    /* drawing: the two on one tile get different places; alone, a character has the tile to itself */
    s->size = SIZE_SMALL;
    s->tile = t;
    entity_layout();
    CHECK(a->slots == 2 && s->slots == 2 && a->slot != s->slot && big->slots == 1);
    Entity *wee = add_character(hex_neighbor_raw(g, 1), 3, ""); /* on the Huge one's edge */
    wee->size = SIZE_WEE;
    entity_layout();
    CHECK(wee->slots == 2 && wee->slot == 1);
}

/* The console line holds the question, then the GM's answer. */
static void test_confirm_answer(void) {
    const char *q = "Nina: new character [2|7] size 1 HP 20 speed 6 init 1. Allow? (y/n)";
    char line[256];
    snprintf(line, sizeof(line), "%sy", q);
    CHECK(confirm_answer(q, line) == ANSWER_YES); /* a name starting with N is not the answer */
    snprintf(line, sizeof(line), "%s n", q);
    CHECK(confirm_answer(q, line) == ANSWER_NO);
    snprintf(line, sizeof(line), "%sNo", q);
    CHECK(confirm_answer(q, line) == ANSWER_NO);
    snprintf(line, sizeof(line), "%sYES ", q);
    CHECK(confirm_answer(q, line) == ANSWER_YES);
    CHECK(confirm_answer(q, q) == ANSWER_YES); /* just Enter */
    snprintf(line, sizeof(line), "%s#", q);
    CHECK(confirm_answer(q, line) == ANSWER_UNCLEAR); /* asked again */
    snprintf(line, sizeof(line), "%snope", q);
    CHECK(confirm_answer(q, line) == ANSWER_UNCLEAR);
    const char *b = "Bob wants to claim character #3. Allow? (y/n)";
    CHECK(confirm_answer(b, "y") == ANSWER_YES); /* the GM cleared the line first */
    CHECK(confirm_answer(b, " no ") == ANSWER_NO);
    CHECK(confirm_answer(b, "") == ANSWER_YES);
    CHECK(confirm_answer(b, "Bob wants to claim") == ANSWER_UNCLEAR); /* an edited question */
}

/* ---- HP rules (Tales of Honor) -------------------------------------------------------------- */

static void test_hp_rules(void) {
    CHECK(hp_clamp(25, 20) == 20 && hp_clamp(-25, 20) == -20 && hp_clamp(-3, 20) == -3);
    CHECK(hp_clamp(5, 0) == 0 && hp_clamp(5, -4) == 0); /* no max HP: nothing to track */
    CHECK(hp_clamp(-3000000000LL, 20) == -20);
    CHECK(hp_stage(20, 20) == HP_HEALTHY && hp_stage(10, 20) == HP_HEALTHY && hp_stage(9, 20) == HP_INJURED);
    CHECK(hp_stage(1, 20) == HP_INJURED && hp_stage(0, 20) == HP_DYING && hp_stage(-10, 20) == HP_DYING);
    CHECK(hp_stage(-11, 20) == HP_CRITICAL && hp_stage(-19, 20) == HP_CRITICAL && hp_stage(-20, 20) == HP_DEAD);
    CHECK(hp_stage(5, 11) == HP_INJURED && hp_stage(6, 11) == HP_HEALTHY);   /* odd max: half is 5.5 */
    CHECK(hp_stage(-5, 11) == HP_DYING && hp_stage(-6, 11) == HP_CRITICAL);
    CHECK(hp_stage(0, 0) == HP_HEALTHY);
    CHECK(strcmp(hp_stage_name(HP_CRITICAL), "Critical") == 0 && strcmp(hp_stage_name(HP_DEAD), "Dead") == 0);

    CHECK(hp_after_max_change(10, 10, 20) == 20); /* full HP stays full */
    CHECK(hp_after_max_change(0, 0, 20) == 20);   /* a new character without max HP */
    CHECK(hp_after_max_change(5, 10, 20) == 5);
    CHECK(hp_after_max_change(15, 20, 10) == 10);
    CHECK(hp_after_max_change(-15, 20, 10) == -10);

    HpBar b = hp_bar_layout(15, 20, 100, 80);
    CHECK(b.fill_x == 100 && b.fill_w == 60 && b.lines && b.injured_x == 140 && !b.critical);
    b = hp_bar_layout(-5, 20, 100, 80); /* dying: grows to the left out of the frame */
    CHECK(b.fill_x == 80 && b.fill_w == 20 && b.lines && b.critical && b.critical_x == 60);
    b = hp_bar_layout(-20, 20, 100, 80);
    CHECK(b.fill_x == 20 && b.fill_w == 80);
    b = hp_bar_layout(0, 20, 100, 80);
    CHECK(b.fill_w == 0 && b.critical);
    b = hp_bar_layout(3, 0, 100, 80);
    CHECK(b.fill_w == 0 && !b.lines && !b.critical);

    reset_table();
    Entity *e = add_character((Hex){ 0, 0 }, 3, "");
    char name[CHAR_NAME_MAX + 16], want[32];
    entity_display_name(e, name, sizeof(name));
    snprintf(want, sizeof(want), "Character #%u", e->id);
    CHECK(strcmp(name, want) == 0);
    snprintf(e->name, sizeof(e->name), "Mao");
    entity_display_name(e, name, sizeof(name));
    CHECK(strcmp(name, "Mao") == 0);
}

static void test_split_args(void) {
    char buf[128], *argv[8];
    int n = split_args(":c name \"Mao Clausewitz\" h 20", buf, sizeof(buf), argv, 8);
    CHECK(n == 5 && strcmp(argv[1], "name") == 0 && strcmp(argv[2], "Mao Clausewitz") == 0 &&
          strcmp(argv[4], "20") == 0);
    n = split_args("  a\tb  ", buf, sizeof(buf), argv, 8);
    CHECK(n == 2 && strcmp(argv[0], "a") == 0 && strcmp(argv[1], "b") == 0);
    n = split_args(":c name \"Open quote", buf, sizeof(buf), argv, 8); /* runs to the end */
    CHECK(n == 3 && strcmp(argv[2], "Open quote") == 0);
    n = split_args("x\"y z\"w", buf, sizeof(buf), argv, 8);
    CHECK(n == 1 && strcmp(argv[0], "xy zw") == 0);
    CHECK(split_args("a b c", buf, sizeof(buf), argv, 2) == 2);
    n = split_args("abcdefghij", buf, 4, argv, 8); /* cut to the buffer */
    CHECK(n == 1 && strcmp(argv[0], "abc") == 0);
    CHECK(split_args("   ", buf, sizeof(buf), argv, 8) == 0);
}

static void test_health_and_names(void) {
    reset_table();
    Entity *hero = add_character((Hex){ 0, 0 }, 3, "Alice"); /* 10/10 HP */
    Entity *orc = add_character((Hex){ 0, 4 }, 3, "");
    OpAuthor alice = player("Alice"), bob = player("Bob");

    Op dmg = { .type = OP_SET_HEALTH, .healthMode = HEALTH_CHANGE, .value = -6 };
    da_push(&dmg.ids, hero->id);
    CHECK_R(ops_check(&dmg, &alice), OPR_OK); /* owners may change their HP */
    CHECK_R(ops_check(&dmg, &bob), OPR_NOT_YOURS);
    CHECK_R(ops_check(&dmg, &GM), OPR_OK);
    ops_apply(&dmg);
    CHECK(hero->health == 4);
    CHECK(strstr(stub_last_log, "Character #") && strstr(stub_last_log, ": 10/10 -> 4/10 (Injured)"));
    ops_apply(&dmg); /* changes add up */
    ops_apply(&dmg);
    CHECK(hero->health == -8 && hp_stage(hero->health, hero->maxHealth) == HP_CRITICAL);
    stub_last_log[0] = '\0';
    dmg.remote = true; /* someone else's change: no log line here */
    ops_apply(&dmg);
    CHECK(hero->health == -10 && stub_last_log[0] == '\0'); /* clamped to -max HP */

    Op set = { .type = OP_SET_HEALTH, .healthMode = HEALTH_SET, .value = 99 };
    da_push(&set.ids, hero->id);
    ops_apply(&set);
    CHECK(hero->health == 10);
    set.healthMode = 2;
    CHECK_R(ops_check(&set, &GM), OPR_INVALID);
    set.healthMode = HEALTH_SET;
    set.value = SAVEFMT_MAX_HP + 1;
    CHECK_R(ops_check(&set, &GM), OPR_INVALID);

    Op orc_hp = { .type = OP_SET_HEALTH, .healthMode = HEALTH_SET, .value = 1 };
    da_push(&orc_hp.ids, orc->id);
    CHECK_R(ops_check(&orc_hp, &alice), OPR_NOT_YOURS); /* unowned characters are the GM's */

    Op nm = { .type = OP_SET_NAME };
    snprintf(nm.charName, sizeof(nm.charName), "Mao Clausewitz");
    da_push(&nm.ids, hero->id);
    CHECK_R(ops_check(&nm, &alice), OPR_OK);
    CHECK_R(ops_check(&nm, &bob), OPR_NOT_YOURS);
    ops_apply(&nm);
    CHECK(strcmp(hero->name, "Mao Clausewitz") == 0);
    Op bad_nm = { .type = OP_SET_NAME };
    da_push(&bad_nm.ids, hero->id);
    CHECK_R(ops_check(&bad_nm, &GM), OPR_INVALID); /* "" is not a name */

    /* the other stats stay GM-only; max HP keeps full HP full */
    Op stats = { .type = OP_SET_STATS, .statMask = STAT_MAXHEALTH, .maxHealth = 20 };
    da_push(&stats.ids, hero->id);
    CHECK_R(ops_check(&stats, &alice), OPR_GM_ONLY);
    ops_apply(&stats);
    CHECK(hero->maxHealth == 20 && hero->health == 20);
    dmg.remote = false;
    ops_apply(&dmg); /* 14/20 */
    stats.maxHealth = 10;
    ops_apply(&stats);
    CHECK(hero->maxHealth == 10 && hero->health == 10);

    /* names travel with spawned copies (paste) */
    Op sp = { .type = OP_SPAWN };
    EntitySpec s = op_spec_from_entity(hero);
    CHECK(strcmp(s.name, "Mao Clausewitz") == 0);
    da_push(&sp.specs, s);
    CHECK_R(ops_check(&sp, &GM), OPR_OK);
    ops_apply(&sp);
    Entity *copy = sp.ids.count == 1 ? entity_by_id(sp.ids.items[0]) : NULL;
    CHECK(copy && strcmp(copy->name, "Mao Clausewitz") == 0);
    Op bad_sp = { .type = OP_SPAWN };
    snprintf(s.name, sizeof(s.name), "bad\x01");
    da_push(&bad_sp.specs, s);
    CHECK_R(ops_check(&bad_sp, &GM), OPR_INVALID);

    op_free(&dmg);
    op_free(&set);
    op_free(&orc_hp);
    op_free(&nm);
    op_free(&bad_nm);
    op_free(&stats);
    op_free(&sp);
    op_free(&bad_sp);
}

static void test_stamina(void) {
    reset_table();
    Entity *hero = add_character((Hex){ 0, 0 }, 3, "Alice");
    Entity *orc = add_character((Hex){ 0, 4 }, 3, "");
    CHECK(hero->stamina == 0 && hero->maxStamina == 0); /* no stamina stat by default */
    OpAuthor alice = player("Alice"), bob = player("Bob");

    Op ms = { .type = OP_SET_STATS, .statMask = STAT_MAXSTAMINA, .maxStamina = 9 };
    da_push(&ms.ids, hero->id);
    da_push(&ms.ids, orc->id);
    CHECK_R(ops_check(&ms, &alice), OPR_GM_ONLY);
    ops_apply(&ms); /* 0/0 counts as full: both start full */
    CHECK(hero->maxStamina == 9 && hero->stamina == 9 && orc->stamina == 9);

    Op spend = { .type = OP_SET_STAMINA, .healthMode = STAMINA_SPEND, .value = 4 };
    da_push(&spend.ids, hero->id);
    CHECK_R(ops_check(&spend, &alice), OPR_OK);
    CHECK_R(ops_check(&spend, &bob), OPR_NOT_YOURS);
    ops_apply(&spend);
    CHECK(hero->stamina == 5 && strstr(stub_last_log, ": stamina 9/9 -> 5/9") && !strstr(stub_last_log, "exhausted"));
    ops_apply(&spend); /* 1 left */
    CHECK_R(ops_check(&spend, &alice), OPR_NO_STAMINA); /* 1 - 4 < 0 */
    CHECK_R(ops_check(&spend, &GM), OPR_NO_STAMINA);    /* nobody spends below 0 */
    spend.value = 1;
    ops_apply(&spend);
    CHECK(hero->stamina == 0 && strstr(stub_last_log, "(exhausted)"));

    Op both = { .type = OP_SET_STAMINA, .healthMode = STAMINA_SPEND, .value = 2 };
    da_push(&both.ids, orc->id);
    da_push(&both.ids, hero->id);
    CHECK_R(ops_check(&both, &GM), OPR_NO_STAMINA); /* one of them can't: all refused */

    Op rec = { .type = OP_SET_STAMINA, .healthMode = HEALTH_CHANGE, .value = 20 };
    da_push(&rec.ids, hero->id);
    ops_apply(&rec);
    CHECK(hero->stamina == 9); /* up to max */
    Op set = { .type = OP_SET_STAMINA, .healthMode = HEALTH_SET, .value = -30 };
    da_push(&set.ids, hero->id);
    CHECK_R(ops_check(&set, &GM), OPR_OK); /* the failsafe: the GM may set a negative value */
    ops_apply(&set);
    CHECK(hero->stamina == -9);
    set.healthMode = 3;
    CHECK_R(ops_check(&set, &GM), OPR_INVALID);
    set.healthMode = HEALTH_SET;
    set.value = SAVEFMT_MAX_HP + 1;
    CHECK_R(ops_check(&set, &GM), OPR_INVALID);

    /* lowering max stamina keeps full stamina full and clamps the rest */
    ms.maxStamina = 5;
    ops_apply(&ms);
    CHECK(hero->stamina == -5 && orc->stamina == 5);

    /* max stamina stays within what the network can carry (spec: 0 to 1,000,000) */
    Op big = { .type = OP_SET_STATS, .statMask = STAT_MAXSTAMINA, .maxStamina = SAVEFMT_MAX_HP + 1 };
    da_push(&big.ids, hero->id);
    CHECK_R(ops_check(&big, &GM), OPR_INVALID);
    big.maxStamina = -1;
    CHECK_R(ops_check(&big, &GM), OPR_INVALID);
    op_free(&big);
    Op sp = { .type = OP_SPAWN };
    EntitySpec bs = op_spec_from_entity(hero);
    bs.maxStamina = SAVEFMT_MAX_HP + 1;
    da_push(&sp.specs, bs);
    CHECK_R(ops_check(&sp, &GM), OPR_INVALID);
    op_free(&sp);

    /* copies keep stamina */
    EntitySpec s = op_spec_from_entity(orc);
    CHECK(s.stamina == 5 && s.maxStamina == 5);

    op_free(&ms); op_free(&spend); op_free(&both); op_free(&rec); op_free(&set);
}

static bool near(float a, float b, float eps) { return fabsf(a - b) <= eps; }

static void test_bar_anim(void) {
    BarAnim a;
    bar_anim_reset(&a, 0.8f);
    CHECK(!bar_anim_step(&a, 0.8f, 0.1f) && a.shown == 0.8f && a.trail == 0.8f); /* at rest */

    /* a loss: fast ease-out, the trail holds 0.3 s, then everything stops */
    float t = 0;
    while (t < 0.25f - 1e-4f) { bar_anim_step(&a, 0.4f, 1.0f / 60); t += 1.0f / 60; }
    float done = (0.8f - a.shown) / 0.4f;
    CHECK(done > 0.9f && done < 0.99f);
    CHECK(a.trail == 0.8f);
    while (t < 0.29f) { bar_anim_step(&a, 0.4f, 1.0f / 60); t += 1.0f / 60; }
    CHECK(a.trail == 0.8f);
    bar_anim_step(&a, 0.4f, 0.1f);
    CHECK(a.trail < 0.8f && a.trail > a.shown);
    bool moving = true;
    for (int i = 0; i < 60 && moving; i++) moving = bar_anim_step(&a, 0.4f, 1.0f / 60); /* 1 s more */
    CHECK(!moving && a.shown == 0.4f && a.trail == 0.4f);

    /* a gain: no trail */
    bool trail_ok = true;
    for (int i = 0; i < 40; i++) {
        bar_anim_step(&a, 0.9f, 1.0f / 60);
        trail_ok = trail_ok && a.trail == a.shown;
    }
    CHECK(trail_ok && a.shown == 0.9f);

    /* a new target mid-way continues from where the fill is */
    bar_anim_reset(&a, 1.0f);
    for (int i = 0; i < 6; i++) bar_anim_step(&a, 0.0f, 1.0f / 60);
    float mid = a.shown;
    bar_anim_step(&a, 1.0f, 1.0f / 60);
    CHECK(mid > 0.05f && mid < 0.95f && a.shown > mid && a.shown - mid < 0.2f);

    /* independent of the frame rate */
    BarAnim fast, slow;
    bar_anim_reset(&fast, 1.0f);
    bar_anim_reset(&slow, 1.0f);
    for (int i = 0; i < 60; i++) bar_anim_step(&fast, 0.0f, 1.0f / 60);
    for (int i = 0; i < 6; i++) bar_anim_step(&slow, 0.0f, 1.0f / 6);
    CHECK(near(fast.shown, slow.shown, 0.01f));

    /* spans */
    float sl, sh, ll, lh;
    a = (BarAnim){ .shown = 0.4f, .trail = 0.7f };
    bar_anim_spans(&a, &sl, &sh, &ll, &lh);
    CHECK(sl == 0 && sh == 0.4f && ll == 0.4f && lh == 0.7f);
    a = (BarAnim){ .shown = -0.75f, .trail = -0.25f };
    bar_anim_spans(&a, &sl, &sh, &ll, &lh);
    CHECK(sl == -0.25f && sh == 0 && ll == -0.75f && lh == -0.25f);
    a = (BarAnim){ .shown = -0.2f, .trail = 0.3f };
    bar_anim_spans(&a, &sl, &sh, &ll, &lh);
    CHECK(sl == sh && ll == -0.2f && lh == 0.3f);

    /* the fraction a bar shows */
    CHECK(bar_fraction(10, 20) == 0.5f && bar_fraction(-30, 20) == -1.0f && bar_fraction(5, 0) == 0.0f);
}

static void test_hud_icons(void) {
    CHECK(hud_icon("placeholder") == NULL); /* not loaded yet */
    hud_icons_init("resources");
    Texture *t = hud_icon("placeholder");
    CHECK(t != NULL && stub_refs(t) == 1);
    CHECK(hud_icon("nope") == NULL);
    hud_icons_free();
    CHECK(hud_icon("placeholder") == NULL && stub_refs(t) == 0);
}

typedef struct { float x, y, w, h; Color c; } StubFill;
extern StubFill stub_fill_ring[32];
extern int stub_fills;
static bool fill_seen(int since, float x, float w, Color c) {
    for (int i = since; i < stub_fills; i++) {
        StubFill *f = &stub_fill_ring[i % 32];
        if (near(f->x, x, 0.01f) && near(f->w, w, 0.01f) && f->c.r == c.r && f->c.g == c.g && f->c.b == c.b &&
            f->c.a == c.a) return true;
    }
    return false;
}

static void test_hud_widgets(void) {
    const HudTheme *t = &HUD_THEME;
    const HudComponent *hc = HUD_COMPONENTS[HUD_HOTBAR], *sc = HUD_COMPONENTS[HUD_SLIDER];
    const HudComponent *bc = HUD_COMPONENTS[HUD_BAR], *pc = HUD_COMPONENTS[HUD_PILL];
    HudElement hb = { .type = HUD_HOTBAR, .key = "hb", .slots = 10 };
    HudElement thin = { .type = HUD_BAR, .key = "b", .thin = true }, thick = { .type = HUD_BAR, .key = "c" };
    HudElement pill = { .type = HUD_PILL, .key = "p" };
    HudElement sl = { .type = HUD_SLIDER, .key = "s", .label = "Value" };

    /* sizes come from the tokens and scale linearly */
    float hw = 10 * t->slot + 9 * t->slotGap;
    CHECK(near(hc->min_width(&hb, 1), hw, 0.01f) && near(hc->min_width(&hb, 2), 2 * hw, 0.01f));
    CHECK(near(hc->height(&hb, 1), t->slot, 0.01f));
    CHECK(near(bc->height(&thin, 1), t->thinBarH, 0.01f) && near(bc->height(&thick, 1), t->barH, 0.01f));
    CHECK(near(pc->min_width(&pill, 1), t->pillW, 0.01f) && near(pc->height(&pill, 1), t->pillH, 0.01f));
    CHECK(near(sc->height(&sl, 1), fmaxf(t->sliderKnob, t->font + t->sliderTextPad), 0.01f));
    CHECK(bc->flex && sc->flex && !pc->flex && !hc->flex);
    CHECK(!bc->mouse && !pc->mouse && sc->mouse && hc->mouse);

    /* hotbar slots: geometry and hit-testing */
    HudRect r = { 100, 50, hw, t->slot };
    HudRect s3 = hud_slot_rect(r, 2, 1), s4 = hud_slot_rect(r, 3, 1);
    CHECK(near(s3.x, 100 + 2 * (t->slot + t->slotGap), 0.01f) && near(s3.w, t->slot, 0.01f));
    CHECK(hud_slot_at(&hb, r, s3.x + 1, s3.y + 1, 1) == 2);
    CHECK(hud_slot_at(&hb, r, s3.x + t->slot + 1, s3.y + 1, 1) == -1); /* in the gap */
    CHECK(hud_slot_at(&hb, r, 99, 60, 1) == -1);
    HudSlot ok = { HUD_SLOT_AVAILABLE, "placeholder", 0, 0 }, cd = ok, off = ok, empty = { 0 };
    cd.cooldown = 2;
    off.state = HUD_SLOT_UNAVAILABLE;
    CHECK(hud_slot_usable(&ok) && !hud_slot_usable(&cd) && !hud_slot_usable(&off) && !hud_slot_usable(&empty));

    /* hotbar mouse: press and release on the same usable slot uses it */
    HudValue hv = { .type = HUD_HOTBAR };
    hv.slots[2] = ok;
    hv.slots[3] = off;
    HudUiState ui = { -1, -1, -1, -1, false, false };
    HudEvent ev = { 0 };
    ui.press = 7;
    CHECK(!hc->mouse(&hb, r, &hv, (HudMouse){ HUD_MOUSE_DOWN, s3.x + 5, s3.y + 5 }, &ui, 7, 1, &ev));
    CHECK(ui.pressSub == 2);
    CHECK(hc->mouse(&hb, r, &hv, (HudMouse){ HUD_MOUSE_UP, s3.x + 6, s3.y + 6 }, &ui, 7, 1, &ev));
    CHECK(ev.type == HUD_EV_SLOT_USED && ev.slot == 2);
    hc->mouse(&hb, r, &hv, (HudMouse){ HUD_MOUSE_DOWN, s3.x + 5, s3.y + 5 }, &ui, 7, 1, &ev);
    CHECK(!hc->mouse(&hb, r, &hv, (HudMouse){ HUD_MOUSE_UP, s4.x + 5, s4.y + 5 }, &ui, 7, 1, &ev)); /* elsewhere */
    hc->mouse(&hb, r, &hv, (HudMouse){ HUD_MOUSE_DOWN, s4.x + 5, s4.y + 5 }, &ui, 7, 1, &ev);
    CHECK(!hc->mouse(&hb, r, &hv, (HudMouse){ HUD_MOUSE_UP, s4.x + 5, s4.y + 5 }, &ui, 7, 1, &ev)); /* unavailable */
    ui.press = -1;
    hc->mouse(&hb, r, &hv, (HudMouse){ HUD_MOUSE_MOVE, s4.x + 5, s4.y + 5 }, &ui, 7, 1, &ev);
    CHECK(ui.hoverSub == 3);
    ui.press = 7;
    hc->mouse(&hb, r, &hv, (HudMouse){ HUD_MOUSE_MOVE, s3.x + 5, s3.y + 5 }, &ui, 7, 1, &ev);
    CHECK(ui.hoverSub == 2); /* follows the pointer while pressed too */
    ui.press = -1;

    /* slider: follows the pointer within its range while pressed, one event per new value */
    HudRect sr = { 0, 0, sc->min_width(&sl, 1) + 100, sc->height(&sl, 1) };
    HudValue sv = { .type = HUD_SLIDER, .slider = { "Value", 50, 0, 100 } };
    ui.press = 3;
    CHECK(sc->mouse(&sl, sr, &sv, (HudMouse){ HUD_MOUSE_DOWN, sr.w + 50, 5 }, &ui, 3, 1, &ev));
    CHECK(ev.type == HUD_EV_SLIDER_CHANGED && ev.value == 100 && sv.slider.value == 100);
    CHECK(!sc->mouse(&sl, sr, &sv, (HudMouse){ HUD_MOUSE_MOVE, sr.w + 80, 5 }, &ui, 3, 1, &ev)); /* still 100 */
    CHECK(sc->mouse(&sl, sr, &sv, (HudMouse){ HUD_MOUSE_MOVE, -500, 5 }, &ui, 3, 1, &ev) && ev.value == 0);
    ui.press = -1;
    sv.slider.value = 50;
    CHECK(!sc->mouse(&sl, sr, &sv, (HudMouse){ HUD_MOUSE_DOWN, sr.x + 5, 5 }, &ui, 3, 1, &ev) && sv.slider.value == 50);
    CHECK(!sc->mouse(&sl, sr, &sv, (HudMouse){ HUD_MOUSE_MOVE, sr.w, 5 }, &ui, 3, 1, &ev)); /* only hovering */
    CHECK(!sc->mouse(&sl, sr, NULL, (HudMouse){ HUD_MOUSE_DOWN, sr.w, 5 }, &ui, 3, 1, &ev)); /* no value */

    /* tab width: padding, icon, text (stub: 0.5 em per byte), close button */
    HudTarget tg = { .shown = true, .name = "Mao" };
    float w0 = 3 * t->tabPad + t->tabIcon + 0.5f * t->font * 3;
    CHECK(near(hud_tab_width(&tg, 1), w0, 0.01f));
    tg.canClose = true;
    CHECK(near(hud_tab_width(&tg, 1), w0 + t->closeSize + t->tabPad, 0.01f));
    snprintf(tg.extra, sizeof(tg.extra), "+2"); /* "Mao +2": 3 more bytes */
    CHECK(near(hud_tab_width(&tg, 2), 2 * (w0 + t->closeSize + t->tabPad + 0.5f * t->font * 3), 0.01f));

    /* drawing runs with and without values (the stubs draw nothing) */
    HudValue bv = { .type = HUD_BAR };
    bv.bar = (HudBar){ .solidLo = -0.3f, .solidHi = 0, .lightLo = -0.3f, .lightHi = 0.2f, .nmarkers = 2,
                       .markers = { { 0.5f, HUD_MARK_LINE }, { -0.5f, HUD_MARK_TICK } }, .text = "-3/10" };
    HudElement two = { .type = HUD_BAR, .key = "hp", .two_sided = true };
    bc->draw(&two, r, &bv, &ui, 0, 1);
    bc->draw(&thick, r, &bv, &ui, 0, 1);
    bc->draw(&thin, r, NULL, &ui, 0, 1);
    HudValue pv = { .type = HUD_PILL, .pill = { "STR", "3" } };
    pc->draw(&pill, r, &pv, &ui, 1, 1);
    pc->draw(&pill, r, NULL, &ui, 1, 1);
    sc->draw(&sl, sr, &sv, &ui, 3, 1);
    sc->draw(&sl, sr, NULL, &ui, 3, 1);
    hc->draw(&hb, r, &hv, &ui, 7, 1);
    hc->draw(&hb, r, NULL, &ui, 7, 1);
    hud_draw_panel(r, 1);
    hud_draw_tab(r, r, &tg, &ui, 1);

    /* drawing maths: a two-sided bar's frame is the right half; the zone below 0 reaches left of it */
    HudRect br = { 100, 50, 400, 20 };
    HudValue lv = { .type = HUD_BAR };
    lv.bar.solidLo = -0.3f;
    int since = stub_fills;
    bc->draw(&two, br, &lv, &ui, 0, 1);
    CHECK(fill_seen(since, 240, 60, t->accent[HUD_ACCENT_RED]));
    since = stub_fills;
    bc->draw(&two, br, NULL, &ui, 0, 1);
    CHECK(!fill_seen(since, 240, 60, t->accent[HUD_ACCENT_RED]) && stub_fills > since);
}

static void test_hud_values(void) {
    char err[128];
    CHECK(hud_config_check(HUD_ROWS, HUD_NROWS, err, sizeof(err)) == 0);

    static const HudElement twice[] = { { .type = HUD_PILL, .key = "a" }, { .type = HUD_PILL, .key = "a" } };
    static const HudElement no_key[] = { { .type = HUD_PILL } };
    static const HudElement big_hotbar[] = { { .type = HUD_HOTBAR, .key = "h", .slots = 11 } };
    static const HudElement hotbar1[] = { { .type = HUD_HOTBAR, .key = "h1", .slots = 10 } };
    static const HudElement hotbar2[] = { { .type = HUD_HOTBAR, .key = "h2", .slots = 2 } };
    static const HudElement bad_type[] = { { .type = HUD_ELEMENT_COUNT, .key = "x" } };
    static const HudElement bad_accent[] = { { .type = HUD_PILL, .key = "x", .accent = HUD_ACCENT_COUNT } };
    static const HudElement other_a[] = { { .type = HUD_BAR, .key = "a" } };
    const HudRow r_twice[] = { { twice, 2 } }, r_no_key[] = { { no_key, 1 } }, r_big[] = { { big_hotbar, 1 } };
    const HudRow r_two_hotbars[] = { { hotbar1, 1 }, { hotbar2, 1 } }, r_bad_type[] = { { bad_type, 1 } };
    const HudRow r_empty_row[] = { { hotbar1, 0 } }, r_across[] = { { twice, 1 }, { other_a, 1 } };
    const HudRow r_bad_accent[] = { { bad_accent, 1 } };
    CHECK(hud_config_check(r_twice, 1, err, sizeof(err)) != 0 && strstr(err, "twice"));
    CHECK(hud_config_check(r_across, 2, err, sizeof(err)) != 0 && strstr(err, "twice")); /* across rows */
    CHECK(hud_config_check(r_no_key, 1, err, sizeof(err)) != 0);
    CHECK(hud_config_check(r_big, 1, err, sizeof(err)) != 0 && strstr(err, "slots"));
    CHECK(hud_config_check(r_two_hotbars, 2, err, sizeof(err)) != 0 && strstr(err, "hotbars"));
    CHECK(hud_config_check(r_bad_type, 1, err, sizeof(err)) != 0 && strstr(err, "type"));
    CHECK(hud_config_check(r_bad_accent, 1, err, sizeof(err)) != 0 && strstr(err, "accent"));
    CHECK(hud_config_check(r_empty_row, 1, err, sizeof(err)) != 0);
    CHECK(hud_config_check(HUD_ROWS, HUD_MAX_ROWS + 1, err, sizeof(err)) != 0 && strstr(err, "rows"));

    hud_begin_frame();
    CHECK(hud_value("hp") == NULL && !hud_target()->shown);
    HudBar b = { .solidHi = 0.5f, .text = "5/10" };
    hud_set_bar("hp", &b);
    hud_set_pill("pill.left", "STR", "3");
    hud_set_slider("value", "Value", 40, 0, 100);
    HudSlot sl = { HUD_SLOT_AVAILABLE, "placeholder", 2, 0 };
    hud_set_slot("hotbar", 4, &sl);
    hud_set_target("Mao", "+1", true);
    const HudValue *v = hud_value("hp");
    CHECK(v && v->type == HUD_BAR && v->bar.solidHi == 0.5f && !strcmp(v->bar.text, "5/10"));
    CHECK(!strcmp(hud_value("pill.left")->pill.label, "STR") && !strcmp(hud_value("pill.left")->pill.value, "3"));
    CHECK(hud_value("value")->slider.value == 40 && hud_value("value")->slider.max == 100);
    v = hud_value("hotbar");
    CHECK(v && v->type == HUD_HOTBAR && v->slots[4].count == 2 && v->slots[0].state == HUD_SLOT_EMPTY);
    HudSlot sl0 = sl;
    sl0.count = 0;
    hud_set_slot("hotbar", 0, &sl0);
    CHECK(hud_value("hotbar")->slots[4].count == 2 && hud_value("hotbar")->slots[0].state == HUD_SLOT_AVAILABLE);
    hud_set_slot("hotbar", 10, &sl); /* out of range: ignored */
    b.solidHi = 0.25f;
    hud_set_bar("hp", &b); /* replaces the earlier value */
    CHECK(hud_value("hp")->bar.solidHi == 0.25f);
    hud_set_pill("hp", "", "x"); /* another type under the same key replaces it too */
    CHECK(hud_value("hp")->type == HUD_PILL);
    const HudTarget *tg = hud_target();
    CHECK(tg->shown && !strcmp(tg->name, "Mao") && !strcmp(tg->extra, "+1") && tg->canClose);

    /* at most HUD_MAX_VALUES values (4 are there already) */
    for (int i = 0; i < HUD_MAX_VALUES; i++) {
        char k[16];
        snprintf(k, sizeof(k), "k%d", i);
        hud_set_pill(k, "", "x");
    }
    CHECK(hud_value("k0") != NULL && hud_value("k27") != NULL && hud_value("k28") == NULL);
    hud_begin_frame();
    CHECK(hud_value("pill.left") == NULL && hud_value("k0") == NULL && !hud_target()->shown);
}

static void test_hud_layout(void) {
    const HudTheme *t = &HUD_THEME;
    hud_use_config(NULL, 0);
    hud_set_visible(true);
    hud_begin_frame();
    HudScreen s = { 1000, 700, 700, 32, 1 };
    hud_set_screen(&s);
    HudRect panel, hb, hp, st, s3, tab, close;
    float pw = 10 * t->slot + 9 * t->slotGap + 2 * t->padding; /* the hotbar is the widest row */
    float ph = 2 * t->padding + t->barH + t->thinBarH + t->slot + 2 * t->rowGap;
    CHECK(hud_part_rect("panel", &panel));
    CHECK(near(panel.w, pw, 0.01f) && near(panel.h, ph, 0.01f) && near(panel.x, (1000 - pw) / 2, 0.01f));
    CHECK(near(panel.y + panel.h, 700 - t->margin, 0.01f));
    CHECK(hud_part_rect("hotbar", &hb) && near(hb.x, panel.x + t->padding, 0.01f));
    CHECK(near(hb.y + hb.h, panel.y + panel.h - t->padding, 0.01f));
    /* the HP bar takes the resources row's free width; the stamina bar the whole row */
    CHECK(hud_part_rect("hp", &hp) && near(hp.w, pw - 2 * t->padding - 2 * t->pillW - 2 * t->elemGap, 0.01f));
    CHECK(hud_part_rect("stamina", &st) && near(st.w, pw - 2 * t->padding, 0.01f));
    CHECK(near(hp.x + hp.w / 2, st.x + st.w / 2, 0.01f)); /* the 0 points line up */
    CHECK(hud_part_rect("hotbar.3", &s3) && near(s3.x, hb.x + 2 * (t->slot + t->slotGap), 0.01f));
    CHECK(!hud_part_rect("hotbar.11", &s3) && !hud_part_rect("hotbar.0", &s3) && !hud_part_rect("nope", &s3));
    CHECK(!hud_part_rect("tab", &tab)); /* no target */
    CHECK(hud_contains(panel.x + 1, panel.y + 1) && !hud_contains(panel.x - 1, panel.y + 1));

    /* the cache: same inputs, no new layout; a change recomputes */
    int n = hud_layout_count();
    CHECK(hud_part_rect("panel", &panel) && hud_layout_count() == n);
    s.bottom = 600; /* the console opened */
    hud_set_screen(&s);
    CHECK(hud_part_rect("panel", &panel) && hud_layout_count() == n + 1);
    CHECK(near(panel.y + panel.h, 600 - t->margin, 0.01f));

    /* the tab: centred above the panel, the close button at its right end */
    hud_set_target("Mao", "", true);
    CHECK(hud_part_rect("tab", &tab) && hud_layout_count() == n + 2);
    CHECK(near(tab.x + tab.w / 2, panel.x + panel.w / 2, 0.01f) && near(tab.y + tab.h, panel.y - t->tabGap, 0.01f));
    CHECK(near(tab.w, hud_tab_width(hud_target(), 1), 0.01f));
    CHECK(hud_part_rect("tab.close", &close) && near(close.x + close.w, tab.x + tab.w - t->tabPad, 0.01f));
    CHECK(hud_contains(tab.x + 1, tab.y + 1));
    hud_set_target("Mao", "", false);
    CHECK(hud_part_rect("tab", &tab) && !hud_part_rect("tab.close", &close));

    /* clear of the toolbox: centred it would start at 33, but toolbox + margin is 44 */
    s = (HudScreen){ 580, 700, 700, 32, 1 };
    hud_set_screen(&s);
    CHECK(hud_part_rect("panel", &panel) && near(panel.x, 32 + t->margin, 0.01f) && hud_layout_scale() == 1);
    /* narrow: shrinks to fit, but not below minScale */
    s.winW = 400;
    hud_set_screen(&s);
    float want = (400 - 32 - 2 * t->margin) / pw;
    CHECK(hud_part_rect("panel", &panel) && near(hud_layout_scale(), want, 0.001f));
    CHECK(near(panel.w, pw * want, 0.05f));
    s.winW = 200;
    hud_set_screen(&s);
    CHECK(hud_part_rect("panel", &panel) && near(hud_layout_scale(), t->minScale, 0.001f));
    /* the scale follows the screen (Ctrl + / -) */
    s = (HudScreen){ 2000, 1000, 1000, 32, 1.5f };
    hud_set_screen(&s);
    CHECK(hud_part_rect("panel", &panel) && near(panel.w, pw * 1.5f, 0.01f));
    /* hidden: no parts */
    hud_set_visible(false);
    CHECK(!hud_part_rect("panel", &panel) && !hud_contains(1000, 990));
    hud_set_visible(true);
    hud_begin_frame();
}

static HudEvent hud_events[8];
static int hud_nevents;
static void hud_record(const HudEvent *ev) {
    if (hud_nevents < 8) hud_events[hud_nevents] = *ev;
    hud_nevents++;
}

/* The game's config has no slider; the input tests add one ("value") above the hotbar, the last row. */
static void use_config_with_slider(void) {
    static const HudElement slider[] = {
        { .type = HUD_SLIDER, .key = "value", .accent = HUD_ACCENT_GOLD, .label = "Value" },
    };
    static HudRow rows[HUD_MAX_ROWS];
    int n = 0;
    for (int i = 0; i < HUD_NROWS - 1; i++) rows[n++] = HUD_ROWS[i];
    rows[n++] = (HudRow){ slider, 1 };
    rows[n++] = HUD_ROWS[HUD_NROWS - 1];
    hud_use_config(rows, n);
}

static void test_hud_input(void) {
    use_config_with_slider();
    hud_set_visible(true);
    hud_set_event_handler(hud_record);
    HudScreen s = { 1000, 700, 700, 32, 1 };
    hud_set_screen(&s);
    hud_begin_frame();
    HudSlot ok = { HUD_SLOT_AVAILABLE, "placeholder", 0, 0 }, off = { HUD_SLOT_UNAVAILABLE, "placeholder", 0, 0 };
    hud_set_slot("hotbar", 0, &ok);
    hud_set_slot("hotbar", 1, &off);
    hud_set_slider("value", "Value", 50, 0, 100);
    hud_set_target("Mao", "", true);
    HudRect panel, s1, s2, close, slider, pill;
    CHECK(hud_part_rect("panel", &panel) && hud_part_rect("hotbar.1", &s1) && hud_part_rect("hotbar.2", &s2));
    CHECK(hud_part_rect("tab.close", &close) && hud_part_rect("value", &slider) && hud_part_rect("pill.left", &pill));
    float ox = panel.x - 5, oy = panel.y - 50; /* outside the panel and left of the tab */
    hud_nevents = 0;

    /* a click on a usable slot uses it; the map never sees the click */
    CHECK(hud_mouse_down(s1.x + 5, s1.y + 5) && hud_mouse_up(s1.x + 5, s1.y + 5));
    CHECK(hud_nevents == 1 && hud_events[0].type == HUD_EV_SLOT_USED && hud_events[0].slot == 0);
    CHECK(!strcmp(hud_events[0].key, "hotbar"));
    /* an unavailable slot and a pill take the click without an event */
    CHECK(hud_mouse_down(s2.x + 5, s2.y + 5) && hud_mouse_up(s2.x + 5, s2.y + 5) && hud_nevents == 1);
    CHECK(hud_mouse_down(pill.x + 5, pill.y + 5) && hud_mouse_up(pill.x + 5, pill.y + 5) && hud_nevents == 1);
    /* outside: not taken */
    CHECK(!hud_mouse_down(ox, oy) && !hud_mouse_up(ox, oy) && !hud_mouse_move(ox, oy));
    /* hovering over the HUD is not a drag: the map keeps getting the motion */
    CHECK(!hud_mouse_move(s1.x + 5, s1.y + 5));

    /* the slider follows a drag, also outside the HUD, until the button is released */
    CHECK(hud_mouse_down(slider.x + slider.w - 1, slider.y + slider.h / 2));
    CHECK(hud_nevents == 2 && hud_events[1].type == HUD_EV_SLIDER_CHANGED && hud_events[1].value == 100);
    CHECK(!strcmp(hud_events[1].key, "value") && hud_value("value")->slider.value == 100);
    CHECK(hud_mouse_move(0, 0) && hud_nevents == 3 && hud_events[2].value == 0);
    CHECK(hud_mouse_up(0, 0) && !hud_mouse_move(0, 0) && hud_nevents == 3);

    /* the tab's close button; pressing it and releasing elsewhere does nothing */
    CHECK(hud_mouse_down(close.x + 2, close.y + 2) && hud_mouse_up(close.x + 2, close.y + 2));
    CHECK(hud_nevents == 4 && hud_events[3].type == HUD_EV_TARGET_CLOSED && hud_events[3].key == NULL);
    CHECK(hud_mouse_down(close.x + 2, close.y + 2) && hud_mouse_up(ox, oy) && hud_nevents == 4);

    /* number keys: only usable slots that exist */
    CHECK(hud_key(0) && hud_nevents == 5 && hud_events[4].slot == 0);
    CHECK(!hud_key(1) && !hud_key(2) && !hud_key(10) && !hud_key(-1) && hud_nevents == 5);

    /* hidden: nothing is taken */
    hud_set_visible(false);
    CHECK(!hud_mouse_down(s1.x + 5, s1.y + 5) && !hud_mouse_up(s1.x + 5, s1.y + 5) && !hud_key(0));
    CHECK(hud_nevents == 5);
    hud_set_visible(true);
    hud_mouse_move(s1.x + 5, s1.y + 5); /* hover, then draw with the stubs */
    hud_draw();
    hud_set_event_handler(NULL);
    hud_begin_frame();
}

static void test_hud_review(void) {
    const HudTheme *t = &HUD_THEME;
    use_config_with_slider();
    hud_set_visible(true);
    hud_set_event_handler(hud_record);
    HudScreen sc = { 1000, 700, 700, 32, 1 };
    hud_set_screen(&sc);
    hud_begin_frame();
    HudSlot ok = { HUD_SLOT_AVAILABLE, "placeholder", 0, 0 };
    hud_set_slot("hotbar", 0, &ok);
    hud_set_slider("value", "Value", 50, 0, 100);
    hud_set_target("Mao", "", true);
    HudRect panel, slider, close, tab, r;
    CHECK(hud_part_rect("panel", &panel) && hud_part_rect("value", &slider) && hud_part_rect("tab.close", &close));
    CHECK(hud_part_rect("tab", &tab));
    hud_nevents = 0;

    /* 1: a press on the slider label, then motion: no jump */
    CHECK(hud_mouse_down(slider.x + 3, slider.y + slider.h / 2));
    CHECK(hud_mouse_move(slider.x + slider.w - 1, slider.y + slider.h / 2));
    CHECK(hud_mouse_up(slider.x + slider.w - 1, slider.y + slider.h / 2));
    CHECK(hud_nevents == 0 && hud_value("value")->slider.value == 50);

    /* 2: a lost mouse-up does not capture the next press */
    CHECK(hud_mouse_down(panel.x + 1, panel.y + 1));
    CHECK(!hud_mouse_down(panel.x - 5, panel.y - 80) && !hud_mouse_move(panel.x - 5, panel.y - 80) &&
          !hud_mouse_up(panel.x - 5, panel.y - 80));
    CHECK(hud_mouse_down(tab.x + 1, tab.y + 1) && hud_mouse_up(close.x + 2, close.y + 2) && hud_nevents == 0);
    hud_cancel_press();
    CHECK(!hud_mouse_up(0, 0));

    /* press on the tab, release over the panel: nothing */
    CHECK(hud_mouse_down(close.x + 2, close.y + 2) && hud_mouse_up(panel.x + 1, panel.y + 1) && hud_nevents == 0);

    /* 4: the close button's pressed look follows the pointer */
    CHECK(hud_mouse_down(close.x + 2, close.y + 2));
    CHECK(hud_mouse_move(0, 0) && hud_mouse_up(0, 0));

    /* 6: slot part names are strict */
    CHECK(!hud_part_rect("hotbar.+3", &r) && !hud_part_rect("hotbar. 3", &r) && !hud_part_rect("hotbar.03", &r));
    CHECK(hud_part_rect("hotbar.10", &r));

    /* 8: NULL texts */
    hud_set_pill("pill.left", NULL, NULL);
    hud_set_slider("value", NULL, 1, 0, 2);
    hud_set_target(NULL, NULL, false);
    CHECK(hud_value("pill.left")->pill.label[0] == 0 && hud_target()->shown);

    /* 9: a long name in a narrow window keeps the tab right of the toolbox */
    hud_begin_frame();
    char longname[HUD_TEXT_MAX];
    memset(longname, 'W', sizeof(longname) - 1);
    longname[sizeof(longname) - 1] = 0;
    hud_set_target(longname, "", true);
    sc = (HudScreen){ 300, 700, 700, 32, 1 };
    hud_set_screen(&sc);
    CHECK(hud_part_rect("tab", &tab) && tab.x >= 32 + t->margin - 0.01f);
    sc = (HudScreen){ 1000, 700, 700, 32, 1 };
    hud_set_screen(&sc);

    /* 11: per-frame cycle keeps the layout */
    hud_begin_frame();
    hud_set_target("Mao", "", true);
    CHECK(hud_part_rect("panel", &panel));
    int n = hud_layout_count();
    hud_begin_frame();
    hud_set_target("Mao", "", true);
    CHECK(hud_part_rect("panel", &panel) && hud_layout_count() == n);

    /* 3 and 7: an invalid config leaves the HUD without rows */
    static const HudElement two[] = { { .type = HUD_HOTBAR, .key = "a", .slots = 2 },
                                      { .type = HUD_HOTBAR, .key = "b", .slots = 2 } };
    static const HudRow bad[] = { { two, 2 } };
    hud_use_config(bad, 1);
    CHECK(!hud_part_rect("panel", &r) && !hud_mouse_down(500, 600) && !hud_contains(500, 600) && !hud_key(0));
    hud_draw();
    hud_use_config(NULL, 0);
    CHECK(hud_part_rect("panel", &r));
    char err[160];
    static const HudElement res1[] = { { .type = HUD_PILL, .key = "tab" } };
    static const HudElement res2[] = { { .type = HUD_HOTBAR, .key = "bar", .slots = 3 }, { .type = HUD_PILL, .key = "bar.2" } };
    static const HudElement res3[] = { { .type = HUD_HOTBAR, .key = "bar", .slots = 3 }, { .type = HUD_PILL, .key = "bar.x" } };
    HudRow r1 = { res1, 1 }, r2 = { res2, 2 }, r3 = { res3, 2 };
    CHECK(hud_config_check(&r1, 1, err, sizeof(err)) == -1 && strstr(err, "reserved"));
    CHECK(hud_config_check(&r2, 1, err, sizeof(err)) == -1 && strstr(err, "reserved"));
    CHECK(hud_config_check(&r3, 1, err, sizeof(err)) == 0);

    /* 11: a row without flex elements is centred; two flex elements share the free width */
    static const HudElement lone[] = { { .type = HUD_PILL, .key = "p" } };
    static const HudElement flex2[] = { { .type = HUD_SLIDER, .key = "s1" }, { .type = HUD_SLIDER, .key = "s2" } };
    static const HudElement hb[] = { { .type = HUD_HOTBAR, .key = "hb", .slots = 6 } };
    static const HudRow cfg2[] = { { lone, 1 }, { flex2, 2 }, { hb, 1 } };
    hud_use_config(cfg2, 3);
    HudRect p, a, b, h;
    CHECK(hud_part_rect("panel", &p) && hud_part_rect("p", &a) && hud_part_rect("s1", &a) && hud_part_rect("s2", &b));
    CHECK(near(a.w, b.w, 0.01f) && near(a.w + b.w + t->elemGap, p.w - 2 * t->padding, 0.01f));
    CHECK(hud_part_rect("p", &a) && near(a.x + a.w / 2, p.x + p.w / 2, 0.01f) && a.w < p.w - 2 * t->padding);
    CHECK(hud_part_rect("hb", &h) && near(h.x + h.w / 2, p.x + p.w / 2, 0.01f));
    hud_use_config(NULL, 0);
    hud_set_event_handler(NULL);
    hud_begin_frame();
}

static void test_hud_game(void) {
    reset_table();
    app.winW = 1000;
    app.winH = 700;
    app.console.fontSize = 16;
    app.console.active = false;
    app.toolbox.size = 32;
    stub_client = false;
    hud_use_config(NULL, 0);
    hud_set_visible(true);
    hud_game_init("resources");
    da_clear(&app.selectedEntities);
    Entity *a = add_character((Hex){ 2, 2 }, 3, "bob"); /* oldest */
    Entity *b = add_character((Hex){ 2, 4 }, 3, "alice");
    Entity *c = add_character((Hex){ 2, 6 }, 3, "");
    int others;
    bool sel;

    /* the GM with nothing selected: no character, but the placeholders */
    CHECK(hud_pick_character(&others, &sel) == NULL);
    hud_game_update(0);
    CHECK(!hud_target()->shown && hud_value("hp") == NULL && hud_value("value") == NULL && hud_value("hotbar"));
    CHECK(hud_value("pill.left") == NULL);

    /* selected: the oldest visible one (duplicates count once), "+N" for the others */
    da_push(&app.selectedEntities, c->id);
    da_push(&app.selectedEntities, a->id);
    da_push(&app.selectedEntities, a->id);
    CHECK(hud_pick_character(&others, &sel) == a && others == 1 && sel);
    a->maxHealth = 20;
    a->health = -5;
    a->maxStamina = 9;
    a->stamina = 0;
    snprintf(a->name, sizeof(a->name), "Mao");
    hud_game_update(0);
    const HudTarget *tg = hud_target();
    CHECK(tg->shown && !strcmp(tg->name, "Mao") && !strcmp(tg->extra, "+1 (exhausted)") && tg->canClose);
    const HudValue *hp = hud_value("hp");
    CHECK(hp && !strcmp(hp->bar.text, "-5/20") && near(hp->bar.solidLo, -0.25f, 0.001f) &&
          near(hp->bar.solidHi, 0, 0.001f));
    CHECK(hp->bar.nmarkers == 2 && hp->bar.markers[0].style == HUD_MARK_LINE && near(hp->bar.markers[0].at, 0.5f, 0.001f));
    CHECK(hp->bar.markers[1].style == HUD_MARK_TICK && near(hp->bar.markers[1].at, -0.5f, 0.001f));
    CHECK(hud_value("stamina") && !strcmp(hud_value("stamina")->bar.text, "0/9"));
    a->health = -20;
    hud_game_update(0);
    CHECK(!strcmp(hud_target()->extra, "+1 (dead)"));
    /* no stamina stat: no stamina value */
    a->health = -5;
    a->maxStamina = 0;
    hud_game_update(0);
    CHECK(hud_value("stamina") == NULL && !strcmp(hud_target()->extra, "+1"));

    /* a change animates; the update reports it until the bar is still */
    a->health = 10;
    CHECK(hud_game_update(0.016f));
    bool moving = true;
    for (int i = 0; i < 200 && moving; i++) moving = hud_game_update(0.016f);
    CHECK(!moving && near(hud_value("hp")->bar.solidHi, 0.5f, 0.001f));

    /* the close button clears the selection */
    HudRect close;
    CHECK(hud_part_rect("tab.close", &close));
    Vec2 p = { close.x + 2, close.y + 2 };
    CHECK(hud_game_mouse_down(p) && hud_game_mouse_up(p) && app.selectedEntities.count == 0);
    CHECK(hud_game_contains(p) && !hud_game_contains((Vec2){ 5, 5 }));

    /* players see their own characters only; with nothing selected the oldest own one, without close button */
    stub_client = true; /* the stub's name is "alice" */
    CHECK(!hud_can_see(a) && hud_can_see(b) && !hud_can_see(c));
    CHECK(hud_pick_character(&others, &sel) == b && !sel && others == 0);
    hud_game_update(0);
    CHECK(hud_target()->shown && !hud_target()->canClose);
    da_push(&app.selectedEntities, a->id); /* bob's: not hers, so she still sees her own */
    CHECK(hud_pick_character(&others, &sel) == b && !sel);
    stub_client = false;
    CHECK(hud_can_see(a) && hud_can_see(c));
    da_clear(&app.selectedEntities);

    /* number keys: only with the console closed and without Ctrl / Alt; debug mode logs */
    hud_game_update(0);
    app.debugMode = true;
    int logs = stub_log_count;
    CHECK(hud_game_key(SDLK_1) && stub_log_count == logs + 1 && strstr(stub_last_log, "slot 1"));
    CHECK(hud_game_key(SDLK_KP_2));
    CHECK(!hud_game_key(SDLK_3) && !hud_game_key(SDLK_4) && !hud_game_key(SDLK_0)); /* cooldown, unavailable, empty */
    app.console.active = true;
    CHECK(!hud_game_key(SDLK_1));
    app.console.active = false;
    app.isCtrlDown = true;
    CHECK(!hud_game_key(SDLK_1));
    app.isCtrlDown = false;
    app.debugMode = false;

    /* hidden: nothing published, nothing moving */
    hud_set_visible(false);
    CHECK(!hud_game_update(0.016f) && hud_value("hotbar") == NULL);
    hud_set_visible(true);
    hud_game_shutdown();
    reset_table();
}

int main(void) {
    test_split_args();
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
    test_size_footprints();
    test_tile_sharing();
    test_confirm_answer();
    test_hp_rules();
    test_health_and_names();
    test_stamina();
    test_bar_anim();
    test_hud_icons();
    test_hud_widgets();
    test_hud_values();
    test_hud_layout();
    test_hud_input();
    test_hud_review();
    test_hud_game();
    CHECK(stub_bad_releases == 0);
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
