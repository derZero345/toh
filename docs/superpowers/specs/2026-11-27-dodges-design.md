# Dodges: design

Date: 2026-11-27. Status: approved.

## Goal

The Dodge action of Tales of Honor (Notion: Personal Actions -> Dodge; rulebook: Base Action Types -> Dodge), plugged
into the attack framework's dodge hook (`find_dodge` in `src/attacks.c`). No placeholders: only the real action.
The ability dodges (Spot Dodge, Leap) are left out, like ability move actions: the app has no abilities, and Leap
needs levels, falling and prone.

## Rules

- Dodge: Major action, 2 stamina, "Move to an adjacent tile", Dodge Value = 1d8 + AGI.
- Dodge contest (DX), already in `attack_dodge_contest`: won if DV >= AV + ET. Won and another direction: miss; lost
  and another direction or won and the same direction: scrape; lost and the same direction: success.
- "You always dodge on the original tile, not on the tile you move to." The dodge's direction is its first tile of
  movement as seen from the attacker: away from it = up, the same-distance neighbour on its left / right = left /
  right. A step toward the attacker matches no attack direction ("another direction" for every attack).
- Evasion Threshold: base 3 (rulebook stats table).

## Decisions (with the user)

| Topic | Decision |
|---|---|
| Model | A real move: the character moves now; until its next turn starts, an attack on a tile it left is resolved with the dodge contest, the direction worked out per attacker from the step. Attacks on the new tile hit it normally. |
| DV | Rolled by the host when dodging (1d8 + AGI), sent in the op, logged, kept with the dodge. |
| ET | A new character stat `Entity.evasion`, default 3: saved (save format 10), sent (protocol 14), `:c et <n>` / `:c evasion <n>` on selected characters, shown in character creation. Attributes don't change it. |
| Block + dodge | Both contests count; the better result for the defender wins (miss > scrape > success). A block whose contest is a success still locks its tile, unless every character hit got a scrape or a miss through a dodge. |
| Path | Dodge moves 1 tile: one click on an adjacent tile the character fits on. |
| Lifetime | A dodge lasts until the dodger's next turn starts, the initiative ends, it is deleted or a table is loaded; outside initiative (test mode) its next dodge replaces it. Not saved. A character in both groups (standing on the tile and dodged from it) counts once, with the dodge; with two dodges from one tile the latest counts. |
| Mark | A green hexagon on each tile it left, an arrow toward the step and its DV. |
| Turn rules | Like the other actions; a dodge counts as a move (Sprint). Test mode lifts initiative, turn, slot and stamina; not ownership, adjacency or room. |

## Architecture

| File | Job |
|---|---|
| `src/dodges.h` / `.c` | `DODGE_ACTIONS`, range (adjacent tiles it fits on), checks, the DV roll, `dodge_apply` (moves, records), `dodge_dir` (direction from the step per attacker), lifetime. |
| `src/attacks.c` | `find_dodge` asks the dodge records; `attack_targets` adds dodgers of the tile; the better of block and dodge. `attack_resolve` / `attack_targets` take the attacker's origin. |
| `src/target_ui.c` | `TARGET_DODGE`: the tiles in range lit, a click dodges. |
| `src/radial*.c` | Major action -> Dodge. |
| ops / netmsg | `OP_DODGE_ACTION` (id, action, tile, value = the d8 roll from the host), `STAT_ET`, ET in specs and saves. |

## Testing

Unit: dodge direction per attacker (up, left, right, toward), contests through `attack_resolve`, dodgers hit on the
tile they left and not on the new one, block + dodge combination and the lock, lifetime and replacement, checks and
test mode, the DV roll, ET (`:c et`, save round trip, older saves get 3, wire). e2e: Dodge through the menu, then an
attack on the tile it left. Guides updated.
