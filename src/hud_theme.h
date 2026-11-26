/* HUD look: every size, colour and font size the HUD components use. Sizes are logical pixels at scale 1; the
 * HUD multiplies them by its scale (console font size / 16, shrunk in narrow windows). */
#ifndef TOH_HUD_THEME_H
#define TOH_HUD_THEME_H

#include "util.h"

typedef enum {
    HUD_ACCENT_RED, HUD_ACCENT_GREEN, HUD_ACCENT_BLUE, HUD_ACCENT_GOLD, HUD_ACCENT_VIOLET, HUD_ACCENT_GREY,
    HUD_ACCENT_COUNT
} HudAccent;

typedef struct {
    /* layout */
    float margin;   /* to the window edge, the open console and the toolbox */
    float padding;  /* inside the panel */
    float rowGap, elemGap;
    float radius;   /* panel and tab corners */
    float minScale; /* narrow windows: the HUD shrinks down to this fraction of its scale */
    /* text */
    float font, smallFont;
    /* bars */
    float barH, thinBarH, barMinW, barFrameLine, markerLine, tickOverhang;
    /* stat pills */
    float pillW, pillH, pillEdge, pillPad;
    /* slider */
    float sliderTrack, sliderKnob, sliderLabelW, sliderMinW;
    /* hotbar */
    float slot, slotGap, slotRadius, slotIconPad, slotTextPad;
    /* details: icon alpha of an unavailable slot, cooldown number size (times font), close cross inset (fraction of
     * the close box), close cross line width, extra slider height beyond the font */
    float unavailableAlpha, cooldownFontScale, closeInset, closeLine, sliderTextPad;
    /* target tab */
    float tabH, tabGap, tabPad, tabIcon, closeSize;
    /* shadow: layers spreading by shadowStep each */
    float shadowStep;
    int shadowLayers;
    /* colours */
    Color panel, border /* default; see hud_set_border */, shadow, text, textDim;
    Color barBg, barFrame, dyingBg, marker, emptyLine;
    Color slotBg, veil, hover, pressed;
    Color accent[HUD_ACCENT_COUNT], accentLight[HUD_ACCENT_COUNT];
} HudTheme;

extern const HudTheme HUD_THEME;

#endif
