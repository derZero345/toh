#include "baranim.h"

#include <math.h>

float bar_fraction(int value, int max) {
    if (max <= 0) return 0;
    float f = (float)value / (float)max;
    return f < -1 ? -1 : f > 1 ? 1 : f;
}

void bar_anim_reset(BarAnim *a, float target) {
    *a = (BarAnim){ .shown = target, .trail = target, .target = target };
}

/* moves v towards to by the frame-rate independent ease-out; snaps when close */
static float ease(float v, float to, float dt) {
    v += (to - v) * (1 - expf(-dt / BAR_ANIM_TAU));
    return fabsf(to - v) < BAR_ANIM_SNAP ? to : v;
}

bool bar_anim_step(BarAnim *a, float target, float dt) {
    if (target < a->target) a->hold = BAR_ANIM_HOLD; /* a loss (re)starts the trail's wait */
    a->target = target;
    a->shown = ease(a->shown, target, dt);
    if (a->trail < a->shown) a->trail = a->shown; /* gains have no trail */
    if (a->hold > 0) {
        a->hold -= dt;
        if (a->hold < 0) a->hold = 0;
    } else {
        a->trail = ease(a->trail, a->shown, dt);
    }
    return a->shown != a->target || a->trail != a->shown;
}

void bar_anim_spans(const BarAnim *a, float *solid_lo, float *solid_hi, float *light_lo, float *light_hi) {
    *light_lo = a->shown;
    *light_hi = a->trail;
    if (a->shown >= 0) *solid_lo = 0, *solid_hi = a->shown;
    else if (a->trail <= 0) *solid_lo = a->trail, *solid_hi = 0;
    else *solid_lo = *solid_hi = 0;
}
