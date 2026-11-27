/* The HUD adapter: chooses the character, publishes the values the config's keys name, and turns HUD events into
 * game actions. This is the only HUD file that touches game state (see hud.h). */
#include "hud_game.h"
#include "party.h"
#include "session.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

static unsigned frame = 1; /* counts updates; Entity.panelFrame 0 means never shown */

/* ---- which character ---- */

static bool mine(const Entity *e) {
    return e && e->type == ENTITY_CHARACTER && e->owner[0] && strcasecmp(e->owner, session_my_name()) == 0;
}

bool hud_can_see(const Entity *e) {
    if (!e || e->type != ENTITY_CHARACTER) return false;
    if (!session_is_client()) return true;
    return mine(e) || party_visible(e, session_my_name());
}

Entity *hud_pick_character(int *others, bool *from_selection) {
    Entity *best = NULL;
    int n = 0;
    for (int i = 0; i < gh->selectedEntities.count; i++) {
        bool dup = false;
        for (int j = 0; j < i && !dup; j++) dup = gh->selectedEntities.items[j] == gh->selectedEntities.items[i];
        Entity *e = dup ? NULL : entity_by_id(gh->selectedEntities.items[i]);
        if (!hud_can_see(e)) continue;
        n++;
        if (!best || e->id < best->id) best = e;
    }
    *others = best ? n - 1 : 0;
    *from_selection = best != NULL;
    if (best || !session_is_client()) return best;
    for (int i = 0; i < gh->entities.count; i++) {
        Entity *e = gh->entities.items[i];
        if (mine(e) && (!best || e->id < best->id)) best = e;
    }
    return best;
}

/* ---- publishing ---- */

static void publish_character(Entity *e, int others, bool from_selection, float dt, bool *moving) {
    char name[CHAR_NAME_MAX + 16], extra[HUD_TEXT_MAX] = "";
    entity_display_name(e, name, sizeof(name));
    bool dead = hp_stage(e->health, e->maxHealth) == HP_DEAD;
    bool tired = e->maxStamina > 0 && e->stamina <= 0;
    int n = others > 0 ? snprintf(extra, sizeof(extra), "+%d", others) : 0;
    if (dead || tired)
        snprintf(extra + n, sizeof(extra) - (size_t)n, "%s(%s)", n ? " " : "", dead ? "dead" : "exhausted");
    hud_set_target(name, extra, from_selection);

    /* bar animation: a character that wasn't shown in the previous frame starts without one */
    float hpf = bar_fraction(e->health, e->maxHealth), stf = bar_fraction(e->stamina, e->maxStamina);
    if (e->panelFrame != frame - 1) {
        bar_anim_reset(&e->hpAnim, hpf);
        bar_anim_reset(&e->staminaAnim, stf);
    }
    e->panelFrame = frame;
    if (bar_anim_step(&e->hpAnim, hpf, dt)) *moving = true;
    if (e->maxStamina <= 0) bar_anim_reset(&e->staminaAnim, stf); /* no bar: a new one appears at its value */
    else if (bar_anim_step(&e->staminaAnim, stf, dt)) *moving = true;

    HudBar hp = { 0 };
    bar_anim_spans(&e->hpAnim, &hp.solidLo, &hp.solidHi, &hp.lightLo, &hp.lightHi);
    HpBar layout = hp_bar_layout(e->health, e->maxHealth, 0, 1); /* in fractions of the frame */
    if (layout.lines) hp.markers[hp.nmarkers++] = (HudMarker){ layout.injured_x, HUD_MARK_LINE };
    if (layout.critical) hp.markers[hp.nmarkers++] = (HudMarker){ layout.critical_x, HUD_MARK_TICK };
    snprintf(hp.text, sizeof(hp.text), "%d/%d", e->health, e->maxHealth);
    hud_set_bar("hp", &hp);

    if (e->maxStamina > 0) {
        HudBar st = { 0 };
        bar_anim_spans(&e->staminaAnim, &st.solidLo, &st.solidHi, &st.lightLo, &st.lightHi);
        snprintf(st.text, sizeof(st.text), "%d/%d", e->stamina, e->maxStamina);
        hud_set_bar("stamina", &st);
    }
}

/* Placeholder content: replace with real game data here (see hud.h). The stat pills stay unpublished ("—"). */
static void publish_placeholders(void) {
    static const HudSlot SLOTS[] = {
        { HUD_SLOT_AVAILABLE, "placeholder", 0, 0 },
        { HUD_SLOT_AVAILABLE, "placeholder", 3, 0 },   /* with a count */
        { HUD_SLOT_AVAILABLE, "placeholder", 0, 2 },   /* on cooldown */
        { HUD_SLOT_UNAVAILABLE, "placeholder", 0, 0 },
    };
    for (int i = 0; i < (int)(sizeof(SLOTS) / sizeof(SLOTS[0])); i++) hud_set_slot("hotbar", i, &SLOTS[i]);
}

static HudScreen hud_game_screen(void) {
    Rect con = console_panel_rect(); /* y = window height while the console is closed */
    float scale = gh->console.fontSize > 0 ? (float)gh->console.fontSize / 16.0f : 1;
    return (HudScreen){ (float)gh->winW, (float)gh->winH, (float)con.y, (float)gh->toolbox.size, scale };
}

void hud_game_refresh_screen(void) {
    HudScreen s = hud_game_screen();
    hud_set_screen(&s);
}

bool hud_game_update(float dt) {
    frame++;
    hud_game_refresh_screen();
    if (gh->nthemes > 0) hud_set_border(gfx_theme()->secondary); /* like the console's border, also while fading */
    hud_begin_frame();
    if (!hud_visible()) return false;
    bool moving = false;
    int others;
    bool from_selection;
    Entity *e = hud_pick_character(&others, &from_selection);
    if (e) publish_character(e, others, from_selection, dt, &moving);
    publish_placeholders();
    return moving;
}

/* ---- events ---- */

static void on_event(const HudEvent *ev) {
    switch (ev->type) {
    case HUD_EV_TARGET_CLOSED: /* like a click on empty map */
        entities_clear_highlights();
        da_clear(&gh->selectedTiles);
        break;
    case HUD_EV_SLOT_USED: /* placeholder slots do nothing yet */
        if (gh->debugMode) console_log("HUD: slot %d used", ev->slot + 1);
        break;
    case HUD_EV_SLIDER_CHANGED: /* the config has no slider */
        break;
    case HUD_EV_BUTTON: case HUD_EV_TEXT: case HUD_EV_SUBMIT: case HUD_EV_STEP: case HUD_EV_CHOICE:
    case HUD_EV_PLUS: /* the config has no controls and no "+" box */
        break;
    }
}

void hud_game_init(const char *resource_dir) {
    hud_icons_init(resource_dir);
    hud_set_event_handler(on_event);
}

void hud_game_shutdown(void) {
    hud_set_event_handler(NULL);
    hud_icons_free();
}

/* ---- input ---- */

bool hud_game_mouse_down(Vec2 p) {
    hud_game_refresh_screen();
    return hud_mouse_down((float)p.x, (float)p.y);
}

bool hud_game_mouse_move(Vec2 p) {
    hud_game_refresh_screen();
    return hud_mouse_move((float)p.x, (float)p.y);
}

bool hud_game_mouse_up(Vec2 p) {
    hud_game_refresh_screen();
    return hud_mouse_up((float)p.x, (float)p.y);
}

bool hud_game_contains(Vec2 p) {
    hud_game_refresh_screen();
    return hud_contains((float)p.x, (float)p.y);
}

bool hud_game_key(SDL_Keycode key) {
    if (gh->console.active || gh->isCtrlDown || gh->isAltDown) return false;
    int slot = -1;
    if (key >= SDLK_1 && key <= SDLK_9) slot = (int)(key - SDLK_1);
    else if (key == SDLK_0) slot = 9;
    else if (key >= SDLK_KP_1 && key <= SDLK_KP_9) slot = (int)(key - SDLK_KP_1);
    else if (key == SDLK_KP_0) slot = 9;
    return slot >= 0 && hud_key(slot);
}
