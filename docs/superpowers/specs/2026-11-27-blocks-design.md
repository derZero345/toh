# Blocks: design

Date: 2026-11-27. Status: approved.

## Goal

Blocks of Tales of Honor (rulebook: Base Action Types -> Block), plugged into the attack framework's block hook
(`find_block` in `src/attacks.c`, see `2026-11-27-attacks-design.md`). Framework plus placeholder rows, like attacks.

## Rules (rulebook)

- A block covers one, two or all three of up / left / right on a tile; neutral is always covered.
- Block contest (BX), already in `attack_block_contest`: right direction and BV >= AV: miss; right direction and
  BV < AV: scrape; wrong direction: success.
- A tile has a single BV: with several blocks on it, their directions combine and the highest BV applies.
- "If you block a tile that you don't occupy and the attack succeeds, you can't block the same tile in your next
  turn unless you occupy it then."

## Decisions (with the user)

| Topic | Decision |
|---|---|
| Rows | `BLOCK_ACTIONS` (id, name, slot, stamina, range, BV). Placeholders: "Test block" (Major, 0 stamina, own tiles, BV 2) and "Test block (range 1)" (Major, 0 stamina, range 1, BV 2). |
| Tiles | Range 0 = the blocker's own tiles (all of a big character's tiles). Range > 0 = one tile within range (own tiles included), chosen like an attack's tile. |
| Directions | A ring of Up / Right / Left **toggles** on the blocked tile; the centre button "Block" confirms, greyed out until one is on. New radial entry kind `RADIAL_TOGGLE`; `radial_toggled(id)`; state reset when a menu opens. |
| Lifetime | A block stays on its tile (also when the blocker moves away) until the blocker's next turn starts, the initiative ends or the blocker is deleted. Outside initiative (test mode) the blocker's next block replaces its earlier ones. Not saved. |
| Failed block | An attack whose block contest on a tile gives success locks that tile for every blocker there who didn't occupy it when blocking: it can't block that tile again (unless it occupies it then) until the end of its next turn. During initiative the lock ends at the start of its turn after next; outside initiative its next block somewhere else ends it; the initiative ending and deleting the blocker end it too. **Test mode does not lift it** (user). |
| Turn rules | Like the other actions; test mode lifts initiative, turn, slot and stamina; not range, ownership or the lock. |
| Marks | Blue inner hexagon on a blocked tile with the combined directions and BV, e.g. `U L 2`. |
| Network | `OP_BLOCK_ACTION` (id, action, tile, value = direction bits). Attacks become host-authoritative: the op carries the characters hit (`ids`) and, after the 3 damage rolls, one outcome per character, because a player who joined mid-round lacks earlier blocks. Protocol 13. |

## Architecture

| File | Job |
|---|---|
| `src/blocks.h` / `.c` | Table, range, checks (incl. the lock), `block_apply`, `block_on_tile` (combined dirs + max BV), locks and their turn bookkeeping. |
| `src/target_ui.h` / `.c` | Replaces `attack_ui`: choosing a tile in range for an attack or a block, then the direction ring (attack) or toggle ring (block). |
| `src/attacks.c` | `find_block` asks `block_on_tile`; `attack_targets` (host) resolves the tile; `attack_apply` applies given outcomes and creates locks. |
| `src/radial*.c` | `RADIAL_TOGGLE`, the `block` menu, Major -> Block submenu, rules. |

## Testing

Unit: toggle kind (toggle, draw state, reset, centre rule), block range and checks, combined BV and directions,
attacks against blocks (miss / scrape / success), the lock (created, armed, ends; test mode keeps it; occupying the
tile lifts it), lifetime of blocks, host-authoritative attack outcomes, wire round trips. e2e: block through the
menu, then an attack on it scrapes or misses. Guides updated.
