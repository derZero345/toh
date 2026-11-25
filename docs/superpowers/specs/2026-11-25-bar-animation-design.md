# Animated HP and stamina bars: design

Date: 2026-11-25. Status: approved.

Builds on the HP panel (`2026-11-25-hp-panel-design.md`) and the stamina bar (`2026-11-25-stamina-design.md`).

## Goal

When a character's HP or stamina changes, its bars on the HP panel move to the new value smoothly instead of
jumping. A loss also leaves a lighter trail that shows how much was lost.

## Behaviour

- **The text** (`current/max`), the injured and critical lines and the " (dead)" / " (exhausted)" labels show the
  new state at once. Only the coloured fills animate.
- **The slide:** the fill moves from where it is to the new value, fast at first and slowing down at the end
  (ease-out, not linear). It is about 95 % there after 0.25 s and done after about 0.45 s.
  - If the value changes again while the fill is moving, it turns towards the new value from where it is,
    without restarting or jumping.
- **The trail (losses only):** the part between the old and the new value is drawn in a lighter colour:
  - light red (235, 130, 130) on the HP bar, light green (150, 215, 150) on the stamina bar;
  - it stays for 0.3 s after the latest loss, then shrinks after the fill with the same ease-out;
  - a whole loss takes about 0.8 s.
- **Gains** only slide; there is no trail.
- **The negative part** (dying HP, the stamina failsafe) works the same way: the newly lost part, left of the
  frame, is drawn light first and the solid fill then grows over it.
- **Max changes** animate too, because the bars show current ÷ max (e.g. raising max HP of an injured character
  slides its fill shorter).
- **No animation when a panel appears.** A panel that was not shown in the previous frame starts at the current
  value: selecting a character, joining a table, loading a save, or the GM selecting a character again.
- **Both bars** use the same animation.

## How it is built

- **Animation module:** `src/baranim.c` / `src/baranim.h`, independent of SDL and the renderer:

  ```c
  typedef struct { float shown, trail, target, hold; } BarAnim; /* fractions of max, -1..1 */
  void bar_anim_reset(BarAnim *a, float target);               /* jump, no animation */
  bool bar_anim_step(BarAnim *a, float target, float dt);      /* dt in seconds; true while moving */
  /* the solid and the light interval, as fractions; an empty interval has lo == hi */
  void bar_anim_spans(const BarAnim *a, float *solid_lo, float *solid_hi, float *light_lo, float *light_hi);
  ```

  - Values are fractions of the frame width (current ÷ max, clamped to −1..1; 0 when max ≤ 0), so the same
    code serves both bars and max changes.
  - The slide is exponential easing: `shown += (target − shown) · (1 − e^(−dt/τ))` with τ = 0.083 s. It is
    independent of the frame rate, and changing the target mid-way continues smoothly from the current position.
    When it is within 0.004 of the target it snaps there and stops.
  - The trail: `trail` never goes below `shown` (a gain pulls it up with the fill). A loss (the target drops)
    sets `hold` to 0.3 s. While `hold` > 0 it counts down. After that the trail eases towards `shown` like the
    slide and snaps when close.
- **Drawing** (`bar_anim_spans`; `shown` ≤ `trail` always):
  - light colour over the interval between `shown` and `trail`;
  - solid colour over the rest of the fill: from 0 to `shown` if `shown` ≥ 0, from `trail` to 0 if `trail` ≤ 0,
    nothing if `shown` < 0 < `trail`;
  - x of a fraction f is `frame_x + f · frame_w`, as in `hp_bar_layout`.
- **State:** each `Entity` gets `BarAnim hpAnim, staminaAnim` and the number of the last frame its panel was
  drawn in. This is memory only: not saved, not sent over the network.
- **Time:** the panel code keeps the time of the previous frame. The step's `dt` is the time since then,
  capped at 0.1 s. If no bar was moving in the previous frame, `dt` is 0, so an animation starts at the frame
  that first shows the change, even after the window was idle for a long time.
- **Redrawing:** the main loop only draws when something happens. `gfx_draw_frame` reports whether a bar is
  still moving, and the main loop keeps drawing (one frame per 16 ms tick) until none is, then goes idle again.

## Out of scope

- Animating the numbers, the labels, or bars on the map tokens.
- A setting to turn the animation off.

## Testing

- Unit tests for `bar_anim_step` (in `tests/core_test.c`):
  - reset: no movement;
  - a loss: after 0.25 s `shown` is 90–99 % of the way and below the old value; the trail still holds the old
    value until 0.3 s, then shrinks; everything has stopped after 1.2 s;
  - a gain: no trail (`trail == shown` throughout);
  - a new target mid-way continues from the current `shown` (no jump);
  - the result does not depend on the frame rate: 60 steps of 1/60 s give the same `shown` as 6 steps of 1/6 s
    (within 0.01);
  - `bar_anim_spans`: shown 0.4 / trail 0.7 gives solid [0, 0.4] and light [0.4, 0.7]; shown −0.75 /
    trail −0.25 gives solid [−0.25, 0] and light [−0.75, −0.25]; shown −0.2 / trail 0.3 gives no solid part and
    light [−0.2, 0.3].
- e2e: the existing pixel checks wait until the animations have finished (they wait well over 1 s after the
  last change), so they stay as strict as now.
- A screenshot taken mid-animation (a few frames after `:c damage`), checked by eye: the light trail is visible.

## Docs

- `gmcontrols.md`: one sentence in the HP panel description saying the bars slide to new values and a loss leaves
  a short lighter trail.
- `playercontrols.md` and the README: no change, nothing new to do.
