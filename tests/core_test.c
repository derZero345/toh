/* Tests for the game core without a window: entity ids, op rules (ops_check) and
 * op effects (ops_apply). Run with `make test`. Stubs: tests/core_stubs.c. */
#include "app.h"

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
    uint32_t old = b->id;
    entities_delete_type(1);
    CHECK(entity_by_id(old) == NULL && entity_by_id(a->id) == a);
    Entity *c = add_wall((Hex){ 0, 1 });
    CHECK(c->id != old); /* ids are never reused */
}

int main(void) {
    test_entity_ids();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
