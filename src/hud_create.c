/* The creation steps in the HUD (see hud_create.h). Edit the tables to change a step's layout. */
#include "hud_create.h"
#include "charcreate.h"
#include "session.h"

#include <stdio.h>
#include <string.h>

#define BTN 84
#define HINT { .type = HUD_LABEL, .key = "cc.hint", .width = 40 } /* flex, empty: pushes the buttons right */
#define CANCEL { .type = HUD_BUTTON, .key = "cc.cancel", .accent = HUD_ACCENT_GREY, .label = "Cancel", .width = BTN }
#define BACK { .type = HUD_BUTTON, .key = "cc.back", .accent = HUD_ACCENT_GREY, .label = "< Back", .width = BTN }
#define NEXT { .type = HUD_BUTTON, .key = "cc.next", .accent = HUD_ACCENT_GREEN, .label = "Next >", .width = BTN }
#define PREVIEW { .type = HUD_IMAGE, .key = "cc.preview", .width = 44 }

static const HudElement BUTTONS_FIRST[] = { HINT, CANCEL, NEXT };
static const HudElement BUTTONS[] = { HINT, CANCEL, BACK, NEXT };

static const HudElement NAME[] = {
    { .type = HUD_LABEL, .key = "cc.title", .label = "Name", .width = 60 },
    { .type = HUD_TEXTFIELD, .key = "cc.name", .accent = HUD_ACCENT_GOLD, .label = "Name of the character",
      .width = 300, .max_len = CHAR_NAME_MAX - 1 },
};
static const HudElement SIZE[] = { { .type = HUD_CHOICE, .key = "cc.size", .accent = HUD_ACCENT_BLUE, .width = 476 } };
static const char *const STEPPER_KEYS[ATTR_COUNT] = { "cc.str", "cc.agi", "cc.cog", "cc.spi" };
static const HudElement ATTRS[] = {
    { .type = HUD_STEPPER, .key = "cc.str", .accent = HUD_ACCENT_RED, .width = 112 },
    { .type = HUD_STEPPER, .key = "cc.agi", .accent = HUD_ACCENT_GREEN, .width = 112 },
    { .type = HUD_STEPPER, .key = "cc.cog", .accent = HUD_ACCENT_BLUE, .width = 112 },
    { .type = HUD_STEPPER, .key = "cc.spi", .accent = HUD_ACCENT_VIOLET, .width = 112 },
};
static const HudElement DERIVED[] = { { .type = HUD_LABEL, .key = "cc.derived", .width = 360 } };
static const HudElement PICTURE[] = {
    PREVIEW,
    { .type = HUD_BUTTON, .key = "cc.picture", .accent = HUD_ACCENT_BLUE, .label = "Choose picture…", .width = 160 },
    { .type = HUD_LABEL, .key = "cc.picname", .width = 120 },
};
/* the picture with "name · size" beside it, the attributes and stats below: as wide as the normal bar, so a
 * 32-byte name and every stat fit */
static const HudElement SUMMARY[] = { PREVIEW, { .type = HUD_LABEL, .key = "cc.summary", .width = 442 } };
static const HudElement STATS[] = { { .type = HUD_LABEL, .key = "cc.stats", .width = 494 } };

#define ROW(e) { (e), (int)(sizeof(e) / sizeof((e)[0])) }
static const HudRow ROWS_NAME[] = { ROW(NAME), ROW(BUTTONS_FIRST) };
static const HudRow ROWS_SIZE[] = { ROW(SIZE), ROW(BUTTONS) };
static const HudRow ROWS_ATTRS[] = { ROW(ATTRS), ROW(DERIVED), ROW(BUTTONS) };
static const HudRow ROWS_PICTURE[] = { ROW(PICTURE), ROW(BUTTONS) };
static const HudRow ROWS_SUMMARY[] = { ROW(SUMMARY), ROW(STATS), ROW(BUTTONS) };
#define STEP(r) { (r), (int)(sizeof(r) / sizeof((r)[0])) }
static const struct { const HudRow *rows; int n; } STEPS[CC_STEPS] = {
    [CC_NAME] = STEP(ROWS_NAME),       [CC_SIZE] = STEP(ROWS_SIZE),       [CC_ATTRS] = STEP(ROWS_ATTRS),
    [CC_PICTURE] = STEP(ROWS_PICTURE), [CC_SUMMARY] = STEP(ROWS_SUMMARY),
};

#define WAIT_LINES 3
static const HudElement START[] = {
    { .type = HUD_BUTTON, .key = "cc.start", .accent = HUD_ACCENT_GREEN, .label = "+ Create character", .width = 280 },
};
static const HudElement WAIT1[] = { { .type = HUD_LABEL, .key = "cc.wait.1", .width = 280 } };
static const HudElement WAIT2[] = { { .type = HUD_LABEL, .key = "cc.wait.2", .width = 280 } };
static const HudElement WAIT3[] = { { .type = HUD_LABEL, .key = "cc.wait.3", .width = 280 } };
static const HudRow EMPTY[1 + WAIT_LINES] = { ROW(START), ROW(WAIT1), ROW(WAIT2), ROW(WAIT3) };
_Static_assert(WAIT_LINES == SESSION_REQUESTS_MAX, "one waiting line per open request");

/* sizes Wee..Gigantic (SIZE_MIN + index); the hints are the tiles and bulk (character_bulk) */
#define NSIZES (SIZE_MAX_CLASS - SIZE_MIN + 1)
static const char *const SIZE_NAMES[NSIZES] = { "Wee", "Tiny", "Small", "Normal", "Big", "Huge", "Gigantic" };
static const char *const SIZE_HINTS[NSIZES] = {
    "Wee: 1 tile, bulk 0", "Tiny: 1 tile, bulk 1", "Small: 1 tile, bulk 2", "Normal: 1 tile, bulk 3",
    "Big: 1 tile, bulk 4", "Huge: 3 tiles in a triangle, bulk 3", "Gigantic: 7 tiles, bulk 5 centre, 3 around",
};

void hud_create_rows(const HudRow **rows, int *n) {
    CcStep s = cc_state()->step;
    *rows = STEPS[s].rows;
    *n = STEPS[s].n;
}

void hud_create_empty_rows(int waiting, const HudRow **rows, int *n) {
    if (waiting < 0) waiting = 0;
    if (waiting > WAIT_LINES) waiting = WAIT_LINES;
    *rows = EMPTY;
    *n = 1 + waiting;
}

void hud_create_publish(void) {
    const CharCreate *c = cc_state();
    char extra[24], line[HUD_LABEL_MAX];
    snprintf(extra, sizeof(extra), "· %d/%d", (int)c->step + 1, (int)CC_STEPS);
    hud_set_target("New character", extra, false);

    const char *why;
    bool ok = cc_can_next(&why);
    hud_set_button("cc.next", c->step < CC_SUMMARY ? "Next >" : c->hasTile ? "Create" : "Place", ok, why);
    hud_set_button("cc.cancel", "Cancel", true, NULL);
    hud_set_button("cc.back", "< Back", true, NULL);

    AttrStats d = attr_derive(c->attr);
    switch (c->step) {
    case CC_NAME:
        hud_set_label("cc.title", "Name");
        hud_set_textfield("cc.name", c->name);
        break;
    case CC_SIZE:
        hud_set_choice("cc.size", SIZE_NAMES, SIZE_HINTS, NSIZES, c->size - SIZE_MIN);
        break;
    case CC_ATTRS:
        for (int a = 0; a < ATTR_COUNT; a++)
            hud_set_stepper(STEPPER_KEYS[a], ATTR_NAMES[a], c->attr[a], ATTR_CREATE_MIN, ATTR_CREATE_MAX);
        snprintf(line, sizeof(line), "Max HP %d · Stamina %d · Speed %d · Initiative %d", d.maxHealth, d.maxStamina,
                 d.speed, d.initiative);
        hud_set_label("cc.derived", line);
        break;
    case CC_PICTURE:
        hud_set_button("cc.picture", "Choose picture…", true, NULL);
        hud_set_image("cc.preview", c->picture);
        hud_set_label("cc.picname", c->picture ? "Picture chosen" : "No picture yet");
        break;
    case CC_SUMMARY:
        hud_set_image("cc.preview", c->picture);
        snprintf(line, sizeof(line), "%s · %s", c->name, SIZE_NAMES[c->size - SIZE_MIN]);
        hud_set_label("cc.summary", line);
        snprintf(line, sizeof(line), "STR %d AGI %d COG %d SPI %d · HP %d · Stamina %d · Speed %d · Initiative %d",
                 c->attr[ATTR_STR], c->attr[ATTR_AGI], c->attr[ATTR_COG], c->attr[ATTR_SPI], d.maxHealth,
                 d.maxStamina, d.speed, d.initiative);
        hud_set_label("cc.stats", line);
        break;
    case CC_STEPS:
        break;
    }
}

void hud_create_publish_empty(void) {
    hud_set_target("No character", "", false);
    int n = session_is_client() ? session_pending_count() : 0;
    bool full = n >= SESSION_REQUESTS_MAX;
    hud_set_button("cc.start", "+ Create character", !full, full ? "Wait for the GM to answer your requests" : NULL);
    for (int i = 0; i < n && i < WAIT_LINES; i++) {
        char key[16], line[HUD_LABEL_MAX];
        const char *name = session_pending_name(i);
        snprintf(key, sizeof(key), "cc.wait.%d", i + 1);
        snprintf(line, sizeof(line), "Waiting for the GM: %s", name[0] ? name : "new character");
        hud_set_label(key, line);
    }
}

bool hud_create_event(const HudEvent *ev) {
    if (!ev->key || strncmp(ev->key, "cc.", 3) != 0) return false;
    const char *k = ev->key + 3;
    switch (ev->type) {
    case HUD_EV_BUTTON:
        if (!strcmp(k, "start")) cc_start(NULL);
        else if (!strcmp(k, "cancel")) cc_cancel();
        else if (!strcmp(k, "back")) cc_back();
        else if (!strcmp(k, "next")) cc_next();
        else if (!strcmp(k, "picture")) cc_choose_picture();
        return true;
    case HUD_EV_TEXT:
        if (!strcmp(k, "name")) cc_set_name(ev->text);
        return true;
    case HUD_EV_SUBMIT:
        if (!strcmp(k, "name")) cc_next();
        return true;
    case HUD_EV_CHOICE:
        if (!strcmp(k, "size")) cc_set_size(SIZE_MIN + ev->value);
        return true;
    case HUD_EV_STEP:
        for (int a = 0; a < ATTR_COUNT; a++)
            if (!strcmp(ev->key, STEPPER_KEYS[a])) cc_step_attr(a, ev->value);
        return true;
    default:
        return false;
    }
}
