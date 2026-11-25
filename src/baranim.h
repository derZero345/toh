/* Smooth HP / stamina bar animation (no SDL). Values are fractions of the frame width, -1..1. */
#pragma once
#include <stdbool.h>

typedef struct {
    float shown;  /* where the solid fill ends */
    float trail;  /* where the light "just lost" part ends; never below shown */
    float target; /* the value the fill is moving to */
    float hold;   /* seconds the trail still waits before it shrinks */
} BarAnim;

#define BAR_ANIM_TAU 0.083f /* ease-out time constant: about 95 % after 0.25 s */
#define BAR_ANIM_HOLD 0.3f  /* the trail waits this long after a loss */
#define BAR_ANIM_SNAP 0.004f

float bar_fraction(int value, int max); /* value / max clamped to -1..1; 0 when max <= 0 */
void bar_anim_reset(BarAnim *a, float target);
bool bar_anim_step(BarAnim *a, float target, float dt); /* true while still moving */
void bar_anim_spans(const BarAnim *a, float *solid_lo, float *solid_hi, float *light_lo, float *light_hi);
