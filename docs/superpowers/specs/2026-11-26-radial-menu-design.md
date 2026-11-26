# Radial tile menu: design

Date: 2026-11-26. Status: built.

## Goal

A ring-shaped context menu that opens around a hex on a right click, in the spirit of strategy-game radial menus.
Only the framework is built: placeholder entries (actions, two submenus, one nested submenu, a disabled entry, a
GM-only entry) let it be tried out. The real entries come later and only change data.

## Decisions

| Topic | Decision |
|---|---|
| Opening | A right click (press and release, at most 5 px apart) on a tile **with the Drag tool**. Right drag still pans. The Measure, Line, Area and Cone tools keep their own right click. |
| Position | Centred on the hex's centre; follows the hex when the view scrolls or zooms; moved inside the window near the edges. |
| Closing | Esc, a left click outside the menu, a right click on the menu or its hex. A right click on another hex closes it and opens it there (on release). |
| Submenus | The current ring shrinks into an inner ring, the submenu's ring grows around it, the opener stays highlighted (accent fill), the rest of the inner ring is dimmed. The centre becomes "back"; clicking the highlighted entry also goes back; clicking another submenu entry in the inner ring swaps the outer ring. Only the current level and its parent are shown, at any depth (max 6 levels). |
| Animation | Ease-out, 180 ms open, 200 ms between levels, 120 ms close. Rings animate from wherever they are, so clicking mid-animation works; hit-testing uses where the rings are going. |
| States | Normal, hover (lighter fill, wider band, grows 4 px outwards), pressed, active (opened the submenu), disabled (semi-transparent fill, faded icon and grey band, not clickable, tooltip says why). |
| Keyboard | Not supported: keyboard-only play can't open the menu. |
| Network, saves | No change. The menu is purely local. |

## Architecture

Same split as the HUD (hud.h):

| File | Job | Touches game state? |
|---|---|---|
| `src/radial.h` | Interface and the how-to (actions, submenus, conditions, menus per hex type, entry kinds) | no |
| `src/radial.c` | Controller: config check, rules, levels, ring animation, hit-testing, input, events, the entry-kind table | no |
| `src/radial_widgets.c` | Components: RadialRing (every level), RingSegment, CenterButton, tooltip | no |
| `src/radial_config.c` | Data: the menus as trees of entries | no |
| `src/hud_theme.c` / `.h` | Tokens, shared with the HUD (`radial*` fields) | no |
| `src/hud_icons.c` | Icon registry, shared with the HUD | no |
| `src/radial_game.c` / `.h` | Adapter: which menu a hex gets, the rules, what actions do (placeholders: a log line) | **yes, the only one** |

- **Entries** have an id, icon key, label (tooltip), accent, kind (`RADIAL_ACTION`, `RADIAL_SUBMENU`), children,
  and optional `visible_if` / `enabled_if` rule keys (`"!"` negates).
- **Rules** are registered by the adapter as `{ key, fn(target), unmet reason, met reason }`. Visibility is decided
  when a ring opens; enabled is rechecked every drawn frame. A submenu with no visible entries is disabled.
- **Menus per hex type**: `RADIAL_MENUS` holds several menus by id; the adapter picks one per hex (`"character"`
  with a character on the hex, else `"tile"`). Each menu has an optional centre action; without one the centre
  closes the menu.
- **Entry kinds** are a table (`RADIAL_KINDS`): whether it draws the "opens a ring" marker and what activating it
  does, so a new kind needs no change in the components.
- **Interface**: `radial_open(menu, hex, x, y)`, `radial_close()`, events `RADIAL_EV_ACTION` (id, menu, hex) and
  `RADIAL_EV_CLOSED`. `input.c` detects the right click and forwards mouse input; the menu never reads game state.

## Testing

- `make test`: config check (every error), segment geometry, open / action / close events, disabled entries,
  nested submenus, back by centre and by the highlighted entry, switching, clicks mid-animation, outside / gap
  clicks, rules for players and the GM, menu choice per hex, edge clamping, and that every sector is filled with
  convex parts.
- `tests/e2e_radial.sh` (in `make e2e`): right click opens, submenu ring and highlighted opener are drawn, Esc
  closes, a right drag doesn't open it, a right click on another tile moves it, a right click on it closes it.
