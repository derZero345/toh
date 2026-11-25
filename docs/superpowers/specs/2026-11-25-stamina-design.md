# Stamina and the stamina bar: design

Date: 2026-11-25. Status: approved.

Builds on the HP panel (`2026-11-25-hp-panel-design.md`).

## Goal

Track stamina as in Tales of Honor and show it as a green bar under the HP bar on the HP panel.

## Tales of Honor rules used

- Max stamina is 8 + AGI. Actions cost stamina.
- At 0 or below a character is Exhausted (and its turn ends; the map does not enforce turns ending).
- Recover and Full Recover restore stamina.
- You can't do an action that would take your stamina below 0. Negative stamina is only a failsafe.

## Data

- `Entity` gets `stamina` and `maxStamina`. Max stamina 0 means the character has no stamina stat; that is the
  default for new characters.
- Current stamina is always clamped to [−max stamina, max stamina] (`hp_clamp`).
- When max stamina changes, a character at full stamina stays full; otherwise current stamina is clamped
  (`hp_after_max_change`).
- New characters start at full stamina (current = max).
- Save format v6 stores both values after the name. Versions 1–5 load with no stamina (0/0).
- Protocol 6: both values are sent in the full table, in spawns, in stats changes (max stamina) and in player
  character requests (max stamina).

## Commands

| Command | Short | Action |
|---|---|---|
| `:c maxstamina <n>` | `ms` | Max stamina (a stat like max HP) |
| `:c stamina <n>` | `st` | Set current stamina |
| `:c spend <n>` | | Lower current stamina by n |
| `:c recover <n>` | | Raise current stamina by n, up to max |

- Who may use them:
  - `stamina`, `spend` and `recover`: the GM on any character, a player on their own characters (like `hp`);
  - `maxstamina`: the GM, plus players in their `:c` request for a new character (like `maxhealth`).
- `spend` is refused when it would take any of the selected characters below 0. That stamina change is
  then refused for all of them and the log says "Not enough stamina"; other changes in the same command still apply.
- `stamina`, `spend` and `recover` on a character without a stamina stat (max 0) log
  "<name> has no stamina; give it some with :c maxstamina <n>".
- Max stamina goes from 0 to 1,000,000. The GM can still set a negative value with `:c stamina <n>`.
- `stamina`, `spend` and `recover` need selected characters, otherwise "Select the characters first".
- Negative numbers after `spend` / `recover` are refused with "Expected a number after ...".
- Values are limited to ±1,000,000, like HP.
- `stamina`, `spend` and `recover` count towards the limit of 8 HP or stamina changes per command.
- The person who made the change gets a log line per character: `Mao: stamina 9/9 -> 6/9`, with
  " (exhausted)" added at 0 or below.

## Network

- New op `OP_SET_STAMINA` with ids, a mode and a value. The modes are:
  - set: the value becomes the new stamina;
  - change: the value is added (used by `recover`; `spend` uses spend);
  - spend: the value is subtracted. The op is refused with `OPR_NO_STAMINA` ("Not enough stamina") if any
    character would go below 0.
- The host checks it like every op, so two people spending at once can't take a character below 0. The
  second request is refused.
- Permissions are the same as `OP_SET_HEALTH`.
- `OP_SET_STATS` gets a `STAT_MAXSTAMINA` bit.

## The panel

- Under the HP bar of each panel, if the character has max stamina > 0:
  - a green bar, 0.9 × fs high, the same width as the HP frame, with a small gap;
  - `current/max` centred in white;
  - no injured or critical lines.
- Negative stamina (failsafe) is drawn like negative HP: the fill grows left out of the frame.
- The name shows " (exhausted)" at 0 or below; " (dead)" wins over it.
- The HP bar keeps its position. Panels with a stamina bar are taller by the bar and the gap, and the
  `+N more` line takes the real panel heights into account.

## Out of scope

- Ending a character's turn when exhausted; stamina recovery dice (1d4); vigor.

## Testing

- Unit tests:
  - clamping and the full-stays-full rule for stamina;
  - `spend` refused below 0 (also with several characters), `recover` up to max;
  - permissions (owner yes, others no; `maxstamina` GM only);
  - save v6 round trip and v5 loading;
  - network encoding of the new op, the stats bit, specs and requests;
  - `OPR_NO_STAMINA` fits OP_REJECTED.
- e2e:
  - the GM gives Bob's character max stamina; Bob spends some; both see the green bar;
  - a spend that is too large is refused and changes nothing.

## Docs

`gmcontrols.md`, `playercontrols.md` and the README Controls section are updated in the commit that adds the
commands and the panel.
