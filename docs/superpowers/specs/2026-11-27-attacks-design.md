# Attacks: design

Date: 2026-11-27. Status: approved.

## Goal

The framework for Tales of Honor attacks (rulebook: Base Action Types -> Attacks, Block, Dodge): an attack targets a
**tile** (not a character) within range, in one of four **directions** (up, left, right, neutral), and succeeds,
scrapes or misses. Only the framework is built. One neutral placeholder attack lets it be tried out; real attacks
(weapons), blocks and dodges come later and plug into the tables and hooks below.

## Decisions (with the user)

| Topic | Decision |
|---|---|
| Scope | Framework only, no rulebook attacks. One placeholder row, "Test attack": Major, 0 stamina, range 1, AV 0, damage 1d4 / 1 / 0. |
| Resolution | At once, when the attack is used (no declaration / resolution phases). Damage is rolled by the host and taken off **every character on the tile** (attacks hit tiles: allies too). |
| Choosing | Pick the attack in the character's action menu (Major action -> Attack), the tiles in range light up, click one, a direction menu opens on it (Up at the top, Right, Neutral, Left), choosing an entry attacks. |
| Directions | Neutral = the target tile. Up = the target's neighbours farther from the attacker. Left / right = the target's neighbour at the same distance from the attacker, on the attacker's left / right as seen from the attacker (for an adjacent target: the neighbour that also borders the attacker). |
| Blocks, dodges | Not built. `attack_resolve` asks two hooks (a block on the tile, a dodge by the character) that return nothing yet, so every attack succeeds. The rulebook's block and dodge contests are pure, tested functions ready for them. How a block and a dodge on the same attack combine is decided when they exist. |
| Tile marks | Every attack leaves a mark on its tile: an arrow in the attack's direction (a ring for neutral), seen by everyone. Marks go when the attacker's next turn starts, when the initiative ends, when the attacker is deleted, or (outside initiative, test mode) when the same character attacks again. Not saved, not sent to players who join mid-round. |
| Turn rules | Like the other actions: initiative, the character's turn, a free slot (a Minor attack may use the Major slot), enough stamina. Using it uses the slot and spends the stamina; using up the last stamina ends the turn. |
| Test mode | Lifts initiative, the turn, the used slot and stamina (like move actions). Range, ownership and the direction stay. |

## Architecture

| File | Job |
|---|---|
| `src/attacks.h` / `.c` | The table `ATTACK_ACTIONS` (id, name, slot, stamina, range, AV, damage per outcome), directions and their tiles, range, the checks, the contests, `attack_resolve` with its hooks, damage rolls, `attack_apply`, the tile marks. |
| `src/attack_ui.h` / `.c` | Choosing the tile and opening the direction menu; state `gh->attackMode`. |
| `src/radial_config.c` | Major action -> "Attack" submenu with the attack entries; the `direction` menu. |
| `src/radial_game.c` | Rules for the attack entries, starting `attack_ui`, the direction menu's events. |
| `src/ops.c`, `src/netmsg.*`, `src/session.c` | `OP_ATTACK_ACTION` / `NOP_ATTACK_ACTION`: id, tile, action, value = direction, values = the host's damage rolls (one per outcome; none from a client). Protocol 12. |
| `src/graphics.c` | The range while choosing, the target while the direction menu is open, the marks. |
| `src/moves.c` | Shared stamina spending and "exhausted ends the turn" for every action. |

Add an attack: an `AttackAction` value and its row in `ATTACK_ACTIONS`, an entry with the same id in
`radial_config.c` and its rule in `radial_game.c`.

## Testing

- `make test` (`test_attack_actions`): table and ids, direction tiles (adjacent and at range, both orientations),
  range (own tiles excluded, multi-tile attackers), checks and refusals, block and dodge contests, rolls checked
  against the damage dice, damage to every character on the tile, the slot and stamina, exhaustion, marks and when
  they go, a client applying the host's rolls; the wire round trip; test mode in `test_test_mode`; the menu in
  `test_action_menu`.
- `tests/e2e_attack.sh` (in `make e2e`): an attack through the menu, the direction menu and the damage.
- Guides: the Attack entry, choosing tile and direction, the marks, the test-mode lines.
