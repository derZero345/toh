# Bottom HUD: design

Date: 2026-11-26. Status: approved.

## Goal

A compact HUD at the bottom centre of the window, in the spirit of Baldur's Gate 3: one dark panel with rows of
elements (resources, a control, a hotbar) and a small floating tab above it for the character being shown.

- It replaces the HP panel in the top right corner. The HP bar, stamina bar and character name move into the HUD.
- Everything else is an empty, working framework with neutral placeholders. Real stats, abilities and items come
  later.
- The HUD will change a lot, so it is built to be changed: a config table decides what appears where, components
  only draw what they are given, and one adapter file is the only code that touches game state.

## Decisions

| Topic | Decision |
|---|---|
| Visibility | On at every start. `:hud` toggles it, `:hud on` / `:hud off` set it. Not saved. |
| Console | Hidden at start. `:` and Ctrl + Enter open it as before. |
| Hotkeys | 1–9, 0 (and the keypad) use hotbar slots 1–10 while the console is closed. |
| Config | A C table in `src/hud_config.c`, checked by the compiler, no runtime parser. |
| Old HP panel | Removed. Its logic (`hp_bar_layout`, `bar_fraction`, `BarAnim`) is reused. |
| Parties | Not part of this work. Who may see a character's HP is decided in one function so parties can extend it. |
| Network, saves | No change. The HUD is purely local. |

## Architecture

| File | Job | Touches game state? |
|---|---|---|
| `src/hud.h` | Public interface and the how-to (see "Docs in the code") | no |
| `src/hud.c` | Values by key, layout and layout cache, hit-testing, input, events, the component table | no |
| `src/hud_widgets.c` | The components: panel frame, target tab, bar, stat pill, slider, hotbar with its slots | no |
| `src/hud_theme.c` / `.h` | All tokens: colours, accent palette, radii, spacing, sizes, font sizes, shadow | no |
| `src/hud_icons.c` | Icon registry: key → texture. For now one key, `"placeholder"` | no |
| `src/hud_config.c` | The rows and their elements, in order | no |
| `src/hud_game.c` | The adapter: chooses the character, publishes values, handles events | **yes, the only one** |

`graphics.c` calls `hud_game_update()` and `hud_draw()` where it called `draw_hp_panels()`. `input.c` gives the
HUD the first look at mouse and number-key input. `console.c` gets `:hud`.

### Values by key

The adapter publishes values under string keys; config elements name the key they show. An element whose key has
no value this frame draws its empty state (dimmed frame, no text, pills show "—"). This covers the GM with nothing
selected, characters without stamina and all placeholders.

```c
hud_begin_frame();                          /* forgets last frame's values */
hud_set_target(name, extra, can_close);     /* the floating tab; not called = tab hidden */
hud_set_bar(key, &bar);                     /* spans, markers, text */
hud_set_pill(key, label, value_text);
hud_set_slider(key, label, value, min, max);
hud_set_slot(key, index, &slot);            /* state, icon key, count, cooldown */
```

Keys are short strings (`"hp"`, `"stamina"`, `"value"`, `"hotbar"`, `"pill.left"`, `"pill.right"`). At most
32 values per frame; a repeated key replaces the earlier value.

### Config

```c
typedef struct {
    HudElementType type;   /* HUD_BAR, HUD_PILL, HUD_SLIDER, HUD_HOTBAR */
    const char *key;       /* which published value it shows */
    HudAccent accent;      /* colour from the theme's accent palette */
    const char *label;     /* placeholder text where the element has one */
    int slots;             /* hotbar only: 1..10 */
    bool two_sided;        /* bar only: range -max..max (HP) */
    bool thin;             /* bar only: the thin variant (stamina) */
} HudElement;

typedef struct { const HudElement *elements; int count; } HudRow;
extern const HudRow HUD_ROWS[];
extern const int HUD_NROWS;
```

Adding, removing or reordering elements and rows means editing this table only. Limits: 8 rows, 8 elements per
row, one hotbar (it owns the number keys).

### Components

```c
typedef struct {
    bool flex;                                            /* shares the row's free width */
    float (*min_width)(const HudElement *, float scale);
    float (*height)(const HudElement *, float scale);
    void (*draw)(const HudElement *, Rect r, const HudValue *v, const HudUiState *ui, float scale);
    /* optional: NULL = the element ignores the mouse */
    bool (*mouse)(const HudElement *, Rect r, const HudValue *v, HudMouse m, HudUiState *ui, float scale);
} HudComponent;

extern const HudComponent *const HUD_COMPONENTS[HUD_ELEMENT_COUNT];  /* indexed by type */
```

A new element type is a new enum value, one new component and one entry in `HUD_COMPONENTS`; no existing
component changes. The panel frame and the target tab are fixed parts of the HUD drawn with the same widget
code, not config elements.

### Events

```c
typedef enum { HUD_EV_TARGET_CLOSED, HUD_EV_SLOT_USED, HUD_EV_SLIDER_CHANGED } HudEventType;
typedef struct { HudEventType type; const char *key; int slot; int value; } HudEvent;
void hud_set_event_handler(void (*fn)(const HudEvent *));
```

The HUD never changes game state itself; it reports what the user did and the adapter decides.

## Layout

All sizes are tokens times a scale. The scale is the console font size / 16, so Ctrl + `+` / `-` scales the HUD
as it scaled the old panel. Base values (scale 1) live in `hud_theme.c`; the ones the layout depends on:

| Token | Value |
|---|---|
| margin to window edge / console | 12 |
| panel padding / row gap / gap between elements | 10 / 6 / 8 |
| panel corner radius | 10 |
| hotbar slot size / gap | 44 / 6 |
| HP bar height / thin bar height | 20 / 12 |
| stat pill size | 64 × 20 |
| slider track / knob | 6 / 14 |
| target tab height / gap above the panel | 28 / 6 |

- Row width: the sum of its elements' minimum widths plus gaps. Panel width: the widest row plus padding (normally
  the hotbar: 10 × 44 + 9 × 6 + 2 × 10 = 514). Flex elements (bars, slider) share each row's remaining width
  equally. Row height: its tallest element.
- Horizontally the panel is centred in the window. If its left edge would be within the toolbox plus the margin,
  it moves right to that point.
- Vertically its bottom is one margin above the console when the console is open, else above the window edge.
- If the window is too narrow (window width minus toolbox minus two margins), the scale shrinks until it fits,
  down to half the normal scale; below that the HUD is cut off at the right window edge.
- The target tab is centred above the panel, one tab gap above it, as wide as its content.

### Layout cache

Rectangles for the panel, the tab, every row and every element (and every hotbar slot) are computed once and
kept. The HUD stores what they were computed from: window size, scale, console height, toolbox size, whether
the tab is shown and the tab's content width. When `hud_draw()` or a mouse call finds any of these changed it
recomputes; nobody else has to remember to invalidate it. Mouse hit-testing uses the cached rectangles.

## Look

The HUD has its own dark style for readability on any map; it does not follow the game's colour themes.

- Panel: background about `#121216` at 85 % opacity, corner radius 10, a 1 px border in a muted brass
  (about `#A08A5A` at 50 %), and a soft shadow of three offset translucent rounded rectangles.
- Text: off-white (about `#E8E4DA`), the game's plain and bold fonts.
- Accent palette (one per element): red, green, blue, gold, violet, grey.
- Bar: dark background, coloured fill, lighter trail for a recent loss, a white 1.5 px frame, text centred.
  Two-sided (HP): the element spans −Max to Max. The right half is the framed bar (0 to Max); the left half is
  a darker, unframed dying zone of the same width, where the fill grows leftwards from the frame's left edge
  while HP is negative. Markers: the injured line in the frame's middle, and while dying the critical tick in
  the middle of the dying zone, as on the old panel.
- Stat pill: a rounded capsule, dark inside, a short accent-coloured bar inside its left end, a small label and a value. Empty: "—".
- Slider: a thin track, filled in the accent colour up to the knob, a round knob, and a label left of it that
  reads `<label> <value>`.
- Hotbar slot states:
  - empty: faint outline, no icon;
  - unavailable: dimmed icon under a grey veil;
  - available: icon, thin accent border;
  - hover: brighter border, raised by 1 px;
  - pressed (mouse button held on it): brass border.
  - The hotkey number (1–9, 0) small in the top left corner; an optional count in the bottom right; an optional
    cooldown as a dark veil with the number centred. A slot on cooldown cannot be used.
- Target tab: `[icon] name +N (status) [✕]` on the panel background, rounded, with the same border.

### Starting config

1. Resources: stat pill `"pill.left"` (placeholder, grey) · two-sided HP bar `"hp"` (red) · stat pill
   `"pill.right"` (placeholder, grey).
2. Stamina: thin two-sided bar `"stamina"` (green). Two-sided because `:c stamina <n>` can set stamina below 0,
   which the old panel drew to the left like HP. Both bars are centred in their rows, so their 0 points line up.
3. Control: slider `"value"` (gold), label "Value", 0–100, starting at 50.
4. Hotbar `"hotbar"`: 10 slots, all with the placeholder icon. Slot 1 available, slot 2 available with count 3,
   slot 3 on cooldown 2, slot 4 unavailable, slots 5–10 empty.

The placeholder icon is a simple neutral glyph in `resources/images/hud/placeholder.png`.

## The adapter (`hud_game.c`)

### Which character

`hud_can_see(e)`: playing without a session or as the GM, any character; as a player, only characters they own
(owner equals their name, ignoring case). Parties will extend this function later.

1. Selected characters the viewer can see: if there are any, the HUD shows the oldest (lowest id). The tab shows
   `+N` for the others and has ✕.
2. Otherwise a player sees their own oldest character, without `+N` and without ✕ (nothing to deselect).
3. Otherwise (the GM with nothing selected) there is no character: the tab is hidden and the bars are empty.

### What it publishes each drawn frame

- Tab: `entity_display_name`, `+N`, and the status `dead` (HP stage dead) or `exhausted` (max stamina > 0 and
  stamina ≤ 0).
- `"hp"`: spans from the character's `hpAnim`, markers from `hp_bar_layout`, text `current/max`. Max HP 0: empty
  frame with `current/0` and no markers.
- `"stamina"`: spans from `staminaAnim`, text `current/max`; not published when max stamina is 0.
- The animation rules stay as they are: a character that was not shown in the previous frame starts without
  animation, `dt` is capped at 0.1 s, and `gfx_draw_frame` keeps returning true while a bar moves.
- Placeholders: the slider's current value and the fixed slot states above. The stat pills are not published.

### Events

| Event | Adapter |
|---|---|
| Target closed (✕) | Clears the selected entities and tiles, like a click on empty map. |
| Slot used | Nothing yet. |
| Slider changed | Stores the value; it is published from the next frame on. |

In debug mode (`:d`) slot and slider events also log `HUD: slot 3 used` / `HUD: Value 42`.

## Input

Only while the HUD is visible:

- Left mouse button down over the panel or tab: consumed (no tile selection, no tool, no entity drag).
  On a slot: pressed state; releasing over the same slot uses it. On ✕: target closed. On the slider: it jumps to
  the pointer and follows it while the button is held, also outside the HUD, sending an event for each new value.
- Right button and wheel over the HUD: consumed, nothing happens.
- Mouse motion: hover state for slots and ✕. Motion is only kept from the map while a press that started on the
  HUD is held (slider drag); otherwise the map still sees it, so a camera drag can cross the HUD.
- Keys 1–9, 0 and keypad 1–9, 0 while the console is closed: use that slot. Empty, unavailable and cooling-down
  slots do nothing.

## Console

- `:hud` toggles the HUD, `:hud on` / `:hud off` set it. Help text: `:hud [on|off] # Show or hide the HUD at the bottom of the window`. Tab completion knows the command and `on` / `off`.
- The console starts hidden.

## Out of scope

- Parties and seeing other players' characters (next spec).
- Real stats, abilities, items, icons and slider meaning.
- Remembering `:hud` between runs.
- Caching the panel's drawn geometry (`DrCache`): only worth it if profiling ever shows the HUD.
- HP on map tokens.

## Testing

- `core_test` (no window; `dr_*` and texture calls stubbed):
  - the config is valid: every type has a component, keys are unique, rows and elements within limits, one
    hotbar with 1–10 slots;
  - layout: element rectangles in a known window, the panel above an open console, clear of the toolbox,
    shrunk in a narrow window, the cache recomputing after a size or console change;
  - hit-testing: the element at a point, nothing outside the panel;
  - the character choice: selected before own, oldest first, `+N`, a player never sees another player's
    character, the GM with nothing selected gets none;
  - events: ✕ clears the selection, a slot is used only when available, number keys only with the console
    closed, a slider drag sends values within its range.
- e2e:
  - the HP checks in `e2e_net.sh` move from the top-right panel to the HUD's HP bar;
  - scripts whose map clicks would land on the HUD run `:hud` first;
  - new: clicking ✕ clears the selection; a click on the HUD selects no tile.
- By eye, from screenshots: every slot state, the empty state, a dying character, a narrow window.

## Docs

- `gmcontrols.md`: the HUD (what it shows, which character), `:hud`, the console hidden at start, keys 1–0,
  ✕, and that the slider and hotbar have no effect yet. The old HP panel text goes.
- `playercontrols.md`: the HUD showing your character, ✕, `:hud`, opening the console (short).
- `README.md` Controls section: kept consistent.
- `hud.h` starts with a short how-to: add a stat (publish a value in `hud_game.c`, add a pill to a row in
  `hud_config.c`), add or reorder a row element, add a hotbar slot, add a new element type, swap icons, and
  where real content will go.
