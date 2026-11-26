/* The HUD's tokens (see hud_theme.h). Change the look here, not in the components. */
#include "hud_theme.h"

const HudTheme HUD_THEME = {
    .margin = 12, .padding = 10, .rowGap = 6, .elemGap = 8, .radius = 10, .minScale = 0.5f,
    .font = 13, .smallFont = 10,
    .barH = 20, .thinBarH = 16, .barMinW = 160, .barFrameLine = 1.5f, .markerLine = 2, .tickOverhang = 3,
    .pillW = 64, .pillH = 20, .pillEdge = 3, .pillPad = 5,
    .sliderTrack = 6, .sliderKnob = 14, .sliderLabelW = 72, .sliderMinW = 120,
    .slot = 44, .slotGap = 6, .slotRadius = 6, .slotIconPad = 7, .slotTextPad = 4,
    .unavailableAlpha = 0.35f, .cooldownFontScale = 1.3f, .closeInset = 0.28f, .closeLine = 2, .sliderTextPad = 4,
    .tabH = 28, .tabGap = 6, .tabPad = 8, .tabIcon = 18, .closeSize = 18,
    .shadowStep = 2, .shadowLayers = 3,
    .panel = { 0x12, 0x12, 0x16, 217 }, /* about 85 % */
    .border = { 0xA0, 0x8A, 0x5A, 128 }, /* muted brass */
    .shadow = { 0, 0, 0, 40 },
    .text = { 0xE8, 0xE4, 0xDA, 255 },
    .textDim = { 0xE8, 0xE4, 0xDA, 110 },
    .barBg = { 0x08, 0x08, 0x0A, 230 },
    .barFrame = { 0xE8, 0xE4, 0xDA, 255 },
    .dyingBg = { 0x05, 0x05, 0x06, 160 },
    .marker = { 255, 255, 255, 255 },
    .emptyLine = { 0xE8, 0xE4, 0xDA, 50 },
    .slotBg = { 0x1C, 0x1C, 0x22, 255 },
    .veil = { 0x30, 0x30, 0x34, 170 },
    .hover = { 0xF0, 0xE6, 0xC8, 255 },
    .pressed = { 0xC8, 0xA4, 0x5A, 255 },
    .accent = {
        [HUD_ACCENT_RED] = { 200, 30, 30, 255 },    [HUD_ACCENT_GREEN] = { 40, 160, 60, 255 },
        [HUD_ACCENT_BLUE] = { 60, 120, 210, 255 },  [HUD_ACCENT_GOLD] = { 200, 160, 70, 255 },
        [HUD_ACCENT_VIOLET] = { 150, 90, 200, 255 }, [HUD_ACCENT_GREY] = { 130, 130, 140, 255 },
    },
    .accentLight = {
        [HUD_ACCENT_RED] = { 235, 130, 130, 255 },   [HUD_ACCENT_GREEN] = { 150, 215, 150, 255 },
        [HUD_ACCENT_BLUE] = { 150, 185, 240, 255 },  [HUD_ACCENT_GOLD] = { 235, 210, 150, 255 },
        [HUD_ACCENT_VIOLET] = { 200, 165, 230, 255 }, [HUD_ACCENT_GREY] = { 190, 190, 198, 255 },
    },
};
