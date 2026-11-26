/* HUD look: every size, colour and font size the HUD components use, and the radial menu's (radial.h: the radial*
 * fields). Sizes are logical pixels at scale 1; the HUD multiplies them by its scale (console font size / 16, shrunk
 * in narrow windows), the radial menu by the same console font size / 16. */
#ifndef TOH_HUD_THEME_H
#define TOH_HUD_THEME_H

#include "util.h"

typedef enum {
    HUD_ACCENT_RED, HUD_ACCENT_GREEN, HUD_ACCENT_BLUE, HUD_ACCENT_GOLD, HUD_ACCENT_VIOLET, HUD_ACCENT_GREY,
    /* the action types of Tales of Honor (the character's action menu) */
    HUD_ACCENT_MINOR, HUD_ACCENT_INFO, HUD_ACCENT_MENTAL, HUD_ACCENT_MAJOR,
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
    /* radial menu (radial.h): radii of the ring roles, from the menu's centre */
    float radialCenterR;                    /* the centre button */
    float radialAloneIn, radialAloneOut;    /* the top level while no submenu is open */
    float radialInnerIn, radialInnerOut;    /* the parent level, shrunk, while a submenu is open */
    float radialOuterIn, radialOuterOut;    /* the submenu around it */
    float radialSegGap;                     /* between segments, constant width */
    float radialBand, radialBandHover;      /* the accent band on a segment's outer edge */
    float radialHoverPop;                   /* a hovered segment grows outwards by this */
    float radialIcon, radialIconFill;       /* icon size, at most this fraction of the ring's thickness */
    float radialMarker;                     /* the "opens a ring" triangle on submenu segments */
    float radialMargin;                     /* the menu keeps this far from the window edge */
    float radialTipPad, radialTipOffset;    /* tooltip padding and distance from the pointer */
    float radialDimAlpha;      /* icon and band of the parent ring's other segments while a submenu is open */
    float radialDisabledAlpha; /* icon and band of a disabled segment */
    float radialOpenScale;                  /* opening grows the ring from this fraction of its size */
    float radialOpenMs, radialMoveMs, radialCloseMs;
    /* colours */
    Color panel, border /* default; see hud_set_border */, shadow, text, textDim;
    Color barBg, barFrame, dyingBg, marker, emptyLine;
    Color slotBg, veil, hover, pressed;
    Color radialSeg, radialSegHover, radialSegPress, radialSegDim, radialSegDisabled, radialCenter, radialDisabledBand, radialWhy;
    Color accent[HUD_ACCENT_COUNT], accentLight[HUD_ACCENT_COUNT];
} HudTheme;

extern const HudTheme HUD_THEME;

#endif
