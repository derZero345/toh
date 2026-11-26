/* The HUD's rows, top to bottom, with their elements left to right. Edit this table to change the HUD; the keys
 * connect elements to the values hud_game.c publishes (see hud.h). */
#include "hud.h"

static const HudElement RESOURCES[] = {
    { .type = HUD_PILL, .key = "pill.left", .accent = HUD_ACCENT_GREY },
    { .type = HUD_BAR, .key = "hp", .accent = HUD_ACCENT_RED, .two_sided = true },
    { .type = HUD_PILL, .key = "pill.right", .accent = HUD_ACCENT_GREY },
};

/* two-sided: :c stamina can set it below 0. Centred like the HP bar, so their 0 points line up. */
static const HudElement STAMINA[] = {
    { .type = HUD_BAR, .key = "stamina", .accent = HUD_ACCENT_GREEN, .two_sided = true, .thin = true },
};

static const HudElement HOTBAR[] = {
    { .type = HUD_HOTBAR, .key = "hotbar", .accent = HUD_ACCENT_BLUE, .slots = 10 },
};

#define ROW(elements) { elements, (int)(sizeof(elements) / sizeof(elements[0])) }

const HudRow HUD_ROWS[] = { ROW(RESOURCES), ROW(STAMINA), ROW(HOTBAR) };
const int HUD_NROWS = (int)(sizeof(HUD_ROWS) / sizeof(HUD_ROWS[0]));
