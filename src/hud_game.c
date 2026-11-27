/* The HUD adapter: chooses the character, publishes the values the config's keys name, and turns HUD events into
 * game actions. This is the only HUD file that touches game state (see hud.h). */
#include "hud_game.h"
#include "charcreate.h"
#include "hud_create.h"
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
    char v[16], tip[HUD_TEXT_MAX];
    snprintf(v, sizeof(v), "%d", e->attr[ATTR_STR]);
    hud_set_pill("pill.left", "STR", v);
    snprintf(v, sizeof(v), "%d", e->attr[ATTR_AGI]);
    hud_set_pill("pill.right", "AGI", v);
    snprintf(tip, sizeof(tip), "COG %d · SPI %d", e->attr[ATTR_COG], e->attr[ATTR_SPI]);
    hud_set_target_tip(tip);

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

/* Placeholder content: replace with real game data here (see hud.h). */
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

/* the rows hud_game asked for last (NULL = HUD_ROWS); switched only on a change, since hud_use_config drops the
 * focus and the layout */
static const HudRow *shown_rows;
static int shown_n;
static int focused_step = -1; /* the step whose name field got the focus; -1 = none */

static void use_rows(const HudRow *r, int n) {
    if (r == shown_rows && n == shown_n) return;
    shown_rows = r;
    shown_n = n;
    hud_use_config(r, n);
}

static void reset_rows(void) {
    shown_rows = NULL;
    shown_n = 0;
    focused_step = -1;
    hud_use_config(NULL, 0);
}

/* nothing for the bar to show: a player owns no character; the GM (or alone) has none on the board */
static bool viewer_has_none(void) {
    for (int i = 0; i < gh->entities.count; i++) {
        const Entity *e = gh->entities.items[i];
        if (session_is_client() ? mine(e) : e->type == ENTITY_CHARACTER) return false;
    }
    return true;
}

bool hud_game_update(float dt) {
    frame++;
    hud_game_refresh_screen();
    if (gh->nthemes > 0) hud_set_border(gfx_theme()->secondary); /* like the console's border, also while fading */
    hud_begin_frame();
    if (!hud_visible()) return false;
    if (gh->console.active) hud_blur(); /* typing goes to the console now */
    const CharCreate *c = cc_state();
    const HudRow *r;
    int n;
    if (c->active && !c->placing) { /* the steps */
        hud_create_rows(&r, &n);
        use_rows(r, n);
        hud_create_publish();
        if ((int)c->step != focused_step) { /* a step opens: the name field gets the focus */
            focused_step = (int)c->step;
            if (c->step == CC_NAME && !gh->console.active) hud_focus("cc.name");
        }
        return false;
    }
    focused_step = -1;
    if (!c->active) hud_set_plus(!session_is_client()); /* the GM's "+"; hidden while creating (also placing) */
    if (!c->active && viewer_has_none()) { /* while placing the bar shows its normal rows */
        hud_create_empty_rows(session_is_client() ? session_pending_count() : 0, &r, &n);
        use_rows(r, n);
        hud_create_publish_empty();
        return false;
    }
    use_rows(NULL, 0);
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
    if (hud_create_event(ev)) return; /* the creation rows' "cc.*" keys */
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
    case HUD_EV_PLUS: /* the GM's "+": the rows switch to the steps in the next update */
        cc_start(NULL);
        break;
    case HUD_EV_BUTTON: case HUD_EV_TEXT: case HUD_EV_SUBMIT: case HUD_EV_STEP: case HUD_EV_CHOICE:
        break; /* only the creation rows have controls */
    }
}

void hud_game_init(const char *resource_dir) {
    hud_icons_init(resource_dir);
    hud_set_event_handler(on_event);
    reset_rows();
}

void hud_game_shutdown(void) {
    hud_set_event_handler(NULL);
    hud_icons_free();
    reset_rows();
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

bool hud_game_text(const char *utf8) { return hud_text_input(utf8); }

bool hud_game_text_key(SDL_Keycode key) {
    if (!hud_focused()) return false;
    switch (key) {
    case SDLK_LSHIFT: case SDLK_RSHIFT: case SDLK_LCTRL: case SDLK_RCTRL: case SDLK_LALT: case SDLK_RALT:
        return false; /* the game tracks them */
    case SDLK_RETURN: case SDLK_KP_ENTER:
        if (gh->isCtrlDown || gh->isAltDown) return false; /* console / log toggles */
        return hud_text_key(HUD_EDIT_ENTER);
    case SDLK_V:
        if (gh->isCtrlDown) { /* paste; line breaks become spaces */
            char *clip = SDL_GetClipboardText();
            if (clip) {
                for (char *p = clip; *p; p++)
                    if (*p == '\n' || *p == '\r') *p = ' ';
                hud_text_input(clip);
                SDL_free(clip);
            }
        }
        return true;
    case SDLK_LEFT: return hud_text_key(HUD_EDIT_LEFT);
    case SDLK_RIGHT: return hud_text_key(HUD_EDIT_RIGHT);
    case SDLK_HOME: return hud_text_key(HUD_EDIT_HOME);
    case SDLK_END: return hud_text_key(HUD_EDIT_END);
    case SDLK_BACKSPACE: return hud_text_key(HUD_EDIT_BACKSPACE);
    case SDLK_DELETE: return hud_text_key(HUD_EDIT_DELETE);
    case SDLK_ESCAPE: return hud_text_key(HUD_EDIT_ESCAPE);
    default:
        return true; /* every other key belongs to the field while it has the focus */
    }
}
