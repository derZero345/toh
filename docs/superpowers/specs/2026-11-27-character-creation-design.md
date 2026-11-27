# Character creation in the HUD: design

Date: 2026-11-27. Status: approved.

## Goal

Creating a character without typing `:c` and stats. The bottom bar (the HUD) walks you through it: name, size,
attributes, picture, then you place the character on the map. Characters also keep their Tales of Honor
attributes (STR, AGI, COG, SPI) from now on, and the stats that come from them follow them.

## When the bar offers creation

- **The bar's own button.** Instead of the HP and stamina rows, the panel shows one big **Create character**
  button when:
  - a player (in a session) owns no character;
  - the GM, or someone playing alone, has no character on the board (no entity of type character at all).
  The tab above the panel then reads `No character`.
- **The GM's "+" box.** The GM (and someone playing alone) always has a small square box at the bottom left of
  the bar, left of the panel, with a "+" icon and the same outline as the panel. Clicking it starts creation.
  Players never see it. It is hidden while creation is running, and while the HUD is hidden (`:hud`).
- **The tile menu, GM only.** Right clicking an empty tile with the Drag tool shows a new entry
  **Create character** in the ring, only for the GM (and someone playing alone; the radial rule `gm`). It starts
  creation with that tile already chosen: the placement step is skipped and the Summary's button says
  **Create** instead of **Place**.
- **A player waiting for the GM.** After a player's request is sent, the panel shows a line
  `Waiting for the GM: Mao` under the Create character button (one line per open request, at most 3, the
  existing limit for claims and new characters together). The line goes when the GM accepts (the character
  arrives and belongs to the player, as today) or declines. If the limit is reached, Create character is
  greyed out and says why when hovered (the existing message for the limit).
- What a player creates belongs to that player once the GM accepts it, as today. What the GM creates has no
  owner (an NPC) until the GM gives it away with `:c owner`. Party and owner are not part of the steps; the GM
  sets them with `:c` afterwards.

## The steps

All steps happen inside the bar: the panel replaces its rows with the step's rows. The tab above reads
`New character · <step>/5` (placing is not a numbered step). No separate window opens.

```
1 Name       [ Mao Clausewitz_______________ ]                   [Cancel] [Next >]
2 Size       ( Wee )( Tiny )( Small )(•Normal )( Big )( Huge )( Gigantic )
                                                         [Cancel] [< Back] [Next >]
3 Attributes STR [-] 1 [+]   AGI [-] 2 [+]   COG [-] 0 [+]   SPI [-] 0 [+]
             Max HP 15 · Stamina 10 · Speed 5 · Initiative 0
                                                         [Cancel] [< Back] [Next >]
4 Picture    (◉ preview)  [Choose picture…]              [Cancel] [< Back] [Next >]
5 Summary    ◉ Mao · Normal · STR 1 AGI 2 COG 0 SPI 0 · HP 15 ST 10 SPD 5 INI 0
                                                         [Cancel] [< Back] [Place]
```

- **Cancel** is on every step. It ends creation and throws the draft away; the bar goes back to what it showed
  before. The log says `Character creation cancelled`.
- **Back** goes one step back and keeps everything entered.
- **Next** is greyed out while the step is incomplete and says why when hovered:
  - Name: "Give the character a name".
  - Picture: "Choose a picture first".
  The other steps are always complete.
- **1 Name.** A text field, focused when the step opens. Clicking it focuses it; clicking anywhere else, or Esc,
  takes the focus away. While it has the focus, typed text goes into it and not to the camera keys (W A S D),
  tool shortcuts, the number keys or `:`. Left / Right, Home / End, Backspace / Delete edit it, Ctrl + V
  pastes (line breaks become spaces), Enter means Next. 1 to 32 bytes, the existing name rule
  (`savefmt_char_name_valid`); typing past 32 bytes does nothing.
- **2 Size.** One option per size class, Wee to Gigantic; Normal is chosen when creation starts. Hovering an
  option shows its tiles and bulk, e.g. `Huge: 3 tiles, a triangle`.
- **3 Attributes.** STR, AGI, COG and SPI, each with − and + buttons, from −2 to 3 (the rulebook's range), all 0
  when creation starts. A button at the end of the range is greyed out. The line below updates at once with
  the derived stats (see **Derived stats**).
- **4 Picture.** **Choose picture…** opens the existing PNG file dialog. The chosen picture is cut to a circle
  as today and shown as a preview; choosing again replaces it. A picture that can't be loaded logs the existing
  "Could not load image" message and leaves the step incomplete. Cancelling the dialog changes nothing.
- **5 Summary.** One line with the picture, name, size, attributes and derived stats.
  - **Place** ends the steps and starts placing (below).
  - With a tile already chosen (from the tile menu), the button is **Create** and creates the character on
    that tile at once, without a room check, like the GM's other ways of putting characters anywhere.

## Placing on the map

- The bar goes back to its normal content. The character's picture follows the mouse over the map at the size
  it will have; the tiles it would cover are tinted green when it fits there and red when it doesn't
  (`character_fits` on the whole footprint, the existing bulk check: a tile holds 5 bulk, walls fill their tile).
- The log says `Click a tile to place Mao (Esc: back)`.
- **Left click** on a tile where it fits places it:
  - GM / alone: an `OP_SPAWN` with the draft, full HP and stamina, no owner. Creation ends.
  - Player: the existing request to the GM (`session_request_character`), with the attributes. The log says
    the existing "Asked the GM to create your character." Creation ends and the waiting line appears.
- A left click on a red tile does nothing but log `No room on that tile`.
- **Esc** or a **right click** goes back to the Summary step with everything kept.
- While placing, the map's other left and right click actions (selecting, tools, the tile menu) are off. The
  camera (wheel, W A S D, dragging with the Drag tool) still works.
- Keyboard-only play: the arrow-key cursor moves the picture, Enter places, Right Shift + Enter goes back.

## Attributes

- `Entity` gets four ints: STR, AGI, COG, SPI. They are 0 for characters in older saves and for characters made
  with `:c` without attributes.
- In the steps they go from −2 to 3. With `:c` they go from −10 to 10 (monsters, homebrew); outside that the
  log says "Attributes go from -10 to 10" and nothing changes.

### Derived stats

From the rulebook's stats table:

| Stat | From |
|---|---|
| Max HP | 12 + 3 × STR |
| Max stamina | 8 + AGI |
| Speed | AGI + 3, at least 2 |
| Initiative | COG |

SPI sets nothing yet (Max Vigor is not in the app).

One function computes them, used by the steps, `:c` and the host's op (`attr_derive` in `src/attrs.c`, below). With the step range
(−2 to 3) they are always valid: Max HP 6 to 21, max stamina 6 to 11, speed 2 to 6.

With `:c` and the wider range, max HP and max stamina are clamped at 0 from below (−10 STR would give −18):
max HP 0 is the existing "no HP" state, max stamina 0 the existing "no stamina" state.

### `:c` options

| Command | Action |
|---|---|
| `:c str <n>` | Set STR; max HP becomes 12 + 3 × STR |
| `:c agi <n>` | Set AGI; max stamina becomes 8 + AGI and speed AGI + 3 (at least 2) |
| `:c cog <n>` | Set COG; initiative becomes COG |
| `:c spi <n>` | Set SPI |

- They have no short forms (they already are short). They go into Tab completion and `:h c`.
- They work on the selected characters, or on the new one when creating with `:c` (`:c name Mao str 1 agi 2`).
- A derived stat given by hand in the same command wins: `:c str 1 maxhealth 40` sets STR 1 and max HP 40.
  A later attribute change recalculates it again (the attribute is the source; a hand-set value lasts until
  then).
- The existing rules for changing max HP and max stamina hold: a character at full HP (stamina) stays full.
- GM only, like the other stat options (`OP_SET_STATS` is GM only): a player's `:c str 2` on a selection says
  "Only the GM can do that". In a player's request for a new character (`:c` with nothing selected) the
  attributes are allowed, as the other stats are.
- The log shows the change per character, e.g. `Mao: STR 1 -> 2, max HP 15 -> 18`.
- The GM's yes/no question for a player's new character also shows the attributes:
  `... STR 1 AGI 2 COG 0 SPI 0`.

### The HUD

- The two stat pills beside the HP bar (`pill.left`, `pill.right`, placeholders until now) show STR (left) and
  AGI (right) of the character shown, e.g. `STR` `1`.
- Hovering the tab shows `COG 0 · SPI 0` as a tooltip below it.

## Data, saves and network

- `Entity`, `EntitySpec`, `NetSpec` and `PendingAction` get the four attributes.
- `OP_SET_STATS` gets four mask bits (`STAT_STR`, `STAT_AGI`, `STAT_COG`, `STAT_SPI`) and the values. The host
  applies an attribute bit together with its derived stats in `apply_stats`, so every game agrees; a derived
  bit set in the same op is applied after it (the hand-set value wins). `ops_check`'s structure check refuses
  attributes outside −10 to 10.
- Save format v9 stores the attributes with each character. v1 to v8 still load, with 0s.
- Protocol 11: the attributes are sent in the full table, in spawns, in `OP_SET_STATS` and in a player's
  create request (`NM_CREATE`).
  - The host refuses a request whose attributes are outside the step range (−2 to 3) as invalid. A player's
    `:c` request with such an attribute is refused before sending: "Attributes go from -2 to 3".
  - The derived stats in a request are not checked against the attributes: a player's `:c` request may still
    give stats by hand, as today. The GM sees everything in the yes/no question before accepting.
- New message `NM_CREATE_ANSWER` (host → player): the seq of the player's `NM_CREATE` and whether it was
  accepted. The host sends it when the GM answers, and when the request is refused (the limit, no room, invalid
  picture, no room any more on accepting). `Request` keeps the seq for this. The existing log lines stay as
  they are.

## Code structure

- **`src/charcreate.c` / `.h`**: the draft and the steps. No drawing, no SDL; tested on its own.
  - State: active, step, name, size, attributes, picture (the circle-cut image), preset tile (optional),
    placing.
  - `cc_start(const Hex *tile)`, `cc_next`, `cc_back`, `cc_cancel`; `cc_can_next(const char **why)`.
  - `cc_set_name`, `cc_set_size`, `cc_step_attr(i, ±1)`, `cc_set_picture`.
  - `cc_place(Hex tile)`: the `OP_SPAWN` (GM) or the request (player); `cc_place_preset()` for the tile-menu
    case.
- **`src/attrs.c` / `.h`**: the attribute names, ranges and `attr_derive` (used by `ops.c`, `console.c` and the
  steps, so `ops.c` does not depend on the creation UI).
- **`src/hud_create.c`**: one `HudRow[]` per step, publishing the draft's values while creation runs, and
  turning HUD events into `cc_*` calls. `hud_game.c` calls it while creation is active and switches configs
  with `hud_use_config` in `hud_game_update` (never in the event handler, which must not call it).
- **New HUD element types** (`hud_widgets.c`, the "add an element type" recipe in `hud.h`):
  - `HUD_BUTTON`: label, accent, disabled state with a reason shown on hover.
  - `HUD_TEXTFIELD`: text, caret, focus; gets text input and editing keys.
  - `HUD_STEPPER`: label, `[-] value [+]`, the ends greyed out.
  - `HUD_CHOICE`: a row of options, one selected, hover text per option.
  - `HUD_LABEL`: one line of text.
  - `HUD_IMAGE`: a picture (the preview).
  - With matching `HudValue` members, `hud_set_*` functions and events (`HUD_EV_BUTTON`, `HUD_EV_TEXT`,
    `HUD_EV_STEP`, `HUD_EV_CHOICE`). `hud_config_check` covers the new types.
- **The "+" box**: a new fixed HUD part `"plus"` (beside `"panel"`, `"tab"`, `"tab.close"`), shown when
  `hud_game.c` turns it on, drawn left of the panel with the panel's outline, sending `HUD_EV_PLUS`.
  `hud_part_rect("plus")` gives its rectangle for scripts.
- **Text input**: while the name field has focus, `input.c` hands SDL text and key events to the HUD before
  the camera, tool and console shortcuts (`hud_game_text`, `hud_game_key` grows to editing keys).
- **Placing**: a placement mode in `input.c` / `graphics.c`, built like the move-action target mode
  (`move_ui.c`): draws the picture and the green / red tiles, handles left click, right click and Esc.
- **Tile menu**: `radial_config.c` / `radial_game.c` add the GM-only **Create character** entry on empty tiles;
  choosing it calls `cc_start(&tile)`.
- **`:c`**: `cmd_character` parses `str`, `agi`, `cog`, `spi`; creating with them fills the derived stats unless
  given by hand.
- **Waiting line**: `session.c` on the client keeps its open create requests (seq and name, at most 3), adds
  one when a request is sent and removes it on the matching `NM_CREATE_ANSWER`. The list is cleared when the
  player leaves or loses the table. `hud_game.c` shows one line per entry.

## Test mode

Nothing to lift: creation has no turn, cost or cooldown. The room check while placing is a placement rule, and
the GM already places anywhere with the tile menu's Create character, `:c` on a tile and `:gm`. The Test mode
section of `gmcontrols.md` stays as it is.

## Tests

- `tests/core_test.c`:
  - `attr_derive` at both ends of the step range and of the `:c` range (speed never below 2, max HP and max
    stamina never below 0).
  - The steps: Next blocked without a name or picture (with the reasons), Back keeps the values, Cancel clears
    the draft, the attribute buttons stop at −2 and 3, a preset tile skips placing.
  - `:c str` / `agi` / `cog` / `spi`: the derived stats follow, a hand-set stat in the same command wins, full HP
    and stamina stay full, players are refused.
  - `hud_config_check` accepts every step config and refuses broken uses of the new types.
- `tests/save_test.c`, `fuzz_savefmt.c`: v9 round trip with attributes; a v8 save loads with 0s.
- `tests/netmsg_test.c`, `fuzz_netmsg.c`: spawns, `OP_SET_STATS`, `NM_CREATE` with attributes and
  `NM_CREATE_ANSWER` round trip; out-of-range attributes are refused.
- e2e (`tests/e2e_hud.sh`, with `@clickhud` and `autoFile` for the picture):
  - The GM with an empty board: Create character, through all steps, place; the character is on the tile with
    the derived stats.
  - Cancel on a middle step leaves the board as it was.
  - The "+" box shows for the GM with characters on the board.
  - The tile menu's Create character skips placing.
- e2e (`tests/e2e_net.sh`): a player creates a character through the bar, the GM accepts, the character
  belongs to the player and has the attributes; the waiting line comes and goes.

## Guides

In the same change as the code:

- `playercontrols.md` section 7 ("Get a character"): first **Create character** in the bar and its steps, then
  claiming, then `:c` as the typed alternative. Section 3 mentions the STR / AGI pills.
- `gmcontrols.md`:
  - section 4 "Create": the bar's button, the "+" box, the tile menu entry, the steps and placing;
  - section 4 "Edit": `:c str` / `agi` / `cog` / `spi`, the derived stats and the ranges;
  - the HUD part: the pills and the tab tooltip;
  - section 10 (the tile menu): the GM-only entry;
  - section 19: players can create through the bar but not change attributes afterwards.
- `README.md` Controls section, to match.

## Not in this change

- Abilities and ability points, Vigor, Evasion Threshold, Honor and Wealth.
- Owner and party in the steps (the GM uses `:c owner` and `:c party`).
- Editing an existing character through the bar.
