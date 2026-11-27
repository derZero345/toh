# Declaration and resolution phases: design

Date: 2026-11-27. Status: approved.

## Goal

Tales of Honor's combat turns (rulebook: Combat -> Combat Turns): a round has a declaration phase, where every
character in initiative order announces its actions (and may respond to earlier announcements), and a resolution
phase, where in the same order the announced actions happen. Outside initiative actions keep happening at once.

## Rules (rulebook)

- Declaration: in initiative order each character declares its Major / Minor actions; it may respond to others.
- Resolution: in initiative order "the action they have set up happens"; contests and damage are worked out then.
- "Prerequisites must be met to set up an action. Actions you made in the setup phase still happen in the Action
  Phase even when their prerequisites are no longer met."
- A character's actions happen in the order it declared them: Step -> Parry blocks on the new tile.
- Move: "When attacked at any point during this movement, the Attack scrapes. When attacked on the original tile or
  the destination tile the attack succeeds." Others "can still affect the original tile and the tiles you moved
  over within the same turn."
- Rolls of blocks and dodges are made when declaring ("Parry up ... for a 5 on Block"); damage when resolving.

## Decisions (with the user)

| Topic | Decision |
|---|---|
| Resolution model | Round-wide: every block and dodge declared this round counts against every attack this round; an attack on tile T hits every character that is on T at some point this round: a tile where one of its moves starts or ends (or where it stays) gives the normal result (blocks, dodges); a tile passed in the middle of a move gives a scrape unless a block or dodge gives a miss; a tile it dodged from gives the dodge contest. Directions come from the attacker's planned tile at that point of its sequence. Damage, HP and rolls happen in resolution order. |
| Stepping | `:i next` ends the declaring turn; after the last character it starts resolution at the top; each `:i next` resolves the current character's actions in declared order; after the last one, `:i next` starts the next round's declaration. Only the GM steps the resolution. |
| Stamina | Spent when declaring (prerequisites are checked then); running out ends the declaring turn. |
| Planned positions | A declared move or dodge moves the character's logical tile (`e->tile`) at once, so its later declarations and everyone's room checks use the planned tiles; the token is drawn where it stood (`Entity.shown`) with a faded copy and the path at the planned tile, and moves when the move resolves. |
| Take back | During its declaring turn (test mode: any time in the declaration phase) a character can take back its last declared action (action menu, "Take back"): slot, stamina, tile, block, dodge and mark are restored. |
| Visibility | All declarations are public: planned path and faded token, hollow red arrow for a declared attack (solid once resolved), block and dodge marks. A label shows `DECLARATION · Mao` / `RESOLUTION · Mao` / `RESOLUTION · done`. |
| Lifetimes | Marks, blocks, dodges and declarations last until the next round's declaration starts; a failed block's lock applies to the next round's declarations. |
| Test mode | During declaration any character may declare at any time (turn, slot, stamina lifted as before); nothing can be declared during resolution, even in test mode. Outside initiative everything happens at once. |
| Network / saving | Declarations are the existing action ops, applied as declarations while initiative runs. Resolving is `OP_INIT_NEXT` from the host carrying every result (rolls, characters hit and outcomes, recovered stamina), so late joiners stay right. The phase is part of the table state (save format 11, snapshot); declarations are not saved (a loaded table keeps the planned tiles). Protocol 15. |

## Architecture

| File | Job |
|---|---|
| `src/phases.h` / `.c` | Phase state, the declaration list (actor, op type, action, tile, value, origin, start tile, path, refund data), round start, resolving a character (host: results into values; everyone: applying them), take back, the round footprint (stop tiles, passed tiles). |
| `src/game.c` | `game_next_turn` runs the phase transitions. |
| `src/moves.c`, `recover.c`, `attacks.c`, `blocks.c`, `dodges.c` | Split into "declare" (slot, stamina, planned tile, records) and "resolve" (effects) parts; outside initiative both at once. `attack_targets` uses the footprint. |
| `src/ops.c`, `netmsg.c`, `session.c` | `OP_TAKE_BACK`; `OP_INIT_NEXT` values; phase in the snapshot / save. |
| `src/graphics.c`, `entity.c` | Draw tokens at `shown`, faded planned token and path, hollow attack marks, phase label. |
| `src/radial*` | "Take back" entry. |

## Testing

Unit: phase flow, planned positions (Step -> Block), footprint hits (stops normal, passed scrape, dodge), take back,
test mode in both phases, resolution results applied on a client without the declarations, save/wire of the phase.
e2e: two characters declare a move and an attack, the GM steps through resolution; log, positions and marks.
Guides rewritten for rounds and phases.
