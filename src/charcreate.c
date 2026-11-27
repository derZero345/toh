/* The creation draft and its steps (see charcreate.h). */
#include "charcreate.h"
#include "move_ui.h"
#include "netmsg.h"
#include "ops.h"
#include "savefmt.h"
#include "session.h"

#include <stdio.h>
#include <string.h>

static CharCreate C;

const CharCreate *cc_state(void) { return &C; }
bool cc_placing(void) { return C.active && C.placing; }

static void reset(void) {
    if (C.picture) vkr_texture_release(C.picture);
    image_free(&C.small);
    unsigned gen = C.gen;
    memset(&C, 0, sizeof(C));
    C.gen = gen;
}

void cc_start(const Hex *tile) {
    reset();
    move_ui_cancel(); /* choosing a move's tile would take the placing click */
    C.gen++;
    C.active = true;
    C.step = CC_NAME;
    C.size = SIZE_NORMAL;
    if (tile) {
        C.hasTile = true;
        C.tile = *tile;
    }
}

void cc_cancel(void) {
    if (!C.active) return;
    reset();
    console_log("Character creation cancelled");
}

void cc_shutdown(void) { reset(); }

bool cc_can_next(const char **why) {
    *why = NULL;
    if (!C.active) return false;
    if (C.step == CC_NAME && !savefmt_char_name_valid(C.name)) *why = "Give the character a name";
    else if (C.step == CC_PICTURE && !C.picture) *why = "Choose a picture first";
    return *why == NULL;
}

/* players: the request's copy of the picture, made from the texture's pixels if the dialog didn't make one (the
 * table was offline then) */
static bool request_picture(void) {
    if (C.small.px) return true;
    const uint8_t *px = C.picture ? vkr_texture_pixels(C.picture) : NULL;
    if (!px) return false;
    Image full = { vkr_texture_width(C.picture), vkr_texture_height(C.picture), (uint8_t *)px }; /* only read */
    C.small = image_scale_to_fit(&full, NET_CREATE_MAX_DIM);
    return true;
}

/* creates the character on h: the GM spawns it, a player asks the GM. False (the reason was logged): a player's
 * request was not sent, the draft stays. */
static bool create(Hex h) {
    AttrStats d = attr_derive(C.attr);
    if (session_is_client()) {
        if (!request_picture()) {
            console_log("Could not encode the picture");
            return false;
        }
        if (!session_request_character(&C.small, h, C.size, d.maxHealth, d.speed, d.initiative, d.maxStamina,
                                       C.name, C.attr))
            return false;
    } else {
        EntitySpec s = { .type = ENTITY_CHARACTER, .image = C.picture, .tile = h, .size = C.size,
                         .maxHealth = d.maxHealth, .health = d.maxHealth, .speed = d.speed,
                         .initiative = d.initiative, .maxStamina = d.maxStamina, .stamina = d.maxStamina };
        snprintf(s.name, sizeof(s.name), "%s", C.name);
        memcpy(s.attr, C.attr, sizeof(s.attr));
        Op op = { .type = OP_SPAWN };
        da_push(&op.specs, s);
        OpResult r = ops_submit(&op); /* logs why it was refused */
        op_free(&op);
        if (r != OPR_OK) return false; /* keep the draft */
    }
    reset();
    return true;
}

void cc_next(void) {
    const char *why;
    if (!cc_can_next(&why) || C.placing) return;
    if (C.step < CC_SUMMARY) {
        C.step++;
    } else if (C.hasTile) {
        create(C.tile);
    } else {
        move_ui_cancel(); /* also when the move was chosen during the steps */
        C.placing = true;
        console_log("Click a tile to place %s (Esc: back)", C.name);
    }
}

void cc_back(void) {
    if (!C.active) return;
    if (C.placing) C.placing = false;
    else if (C.step > CC_NAME) C.step--;
}

void cc_set_name(const char *name) {
    if (C.active) snprintf(C.name, sizeof(C.name), "%s", name ? name : "");
}

void cc_set_size(int size) {
    if (C.active && size >= SIZE_MIN && size <= SIZE_MAX_CLASS) C.size = size;
}

void cc_step_attr(int a, int delta) {
    if (!C.active || a < 0 || a >= ATTR_COUNT) return;
    int v = C.attr[a] + delta;
    C.attr[a] = v < ATTR_CREATE_MIN ? ATTR_CREATE_MIN : v > ATTR_CREATE_MAX ? ATTR_CREATE_MAX : v;
}

void cc_choose_picture(void) {
    if (C.active) gfx_request_file(ACTION_CREATE_PICTURE, NULL, 0, 0, 0, 0, 0, NULL, NULL, NULL);
}

void cc_set_picture(Texture *t, Image small) {
    if (!C.active) {
        image_free(&small);
        return;
    }
    if (t) vkr_texture_retain(t);
    if (C.picture) vkr_texture_release(C.picture);
    C.picture = t;
    image_free(&C.small);
    C.small = small;
}

bool cc_place(Hex h) {
    if (!cc_placing()) return false;
    if (!character_fits(h, C.size, NULL)) {
        console_log("No room on that tile");
        return true;
    }
    create(h);
    return true;
}
