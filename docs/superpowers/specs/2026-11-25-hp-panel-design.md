# HP panel, current HP and character names: design

Date: 2026-11-25. Status: approved.

## Goal

Track a character's current HP as in Tales of Honor and show it to the people who control the character:
a panel in the top right corner with the character's name, a red HP bar with `current/max` inside it, a line
for "injured" and, while dying, the negative part of the bar and a line for "critical".

## Tales of Honor rules used

- HP goes from Max HP down to −Max HP.
- Stages: Healthy at HP ≥ ½ Max, Injured at HP ≥ 1, Dying at HP ≤ 0, Critical at HP < −½ Max,
  dead at HP ≤ −Max.

## Data

- `Entity` gets `char name[CHAR_NAME_MAX]` with `CHAR_NAME_MAX` = 33 (1–32 bytes plus the terminator;
  any bytes except control characters; "" = no name).
- `Entity.health` already exists, is saved and is sent to players. It is now used as the current HP.
- Save format v5 stores the name of each character. v1–v4 still load, with empty names.
- Protocol 5: names are sent in the full table, in spawns and in player character requests.
- A character without a name is shown as `Character #<id>`.

## Commands

| Command | Short | Action |
|---|---|---|
| `:c name <name>` | `nm` | Name the selected characters (or the new one when creating) |
| `:c hp <n>` | | Set current HP |
| `:c damage <n>` | `dmg` | Lower current HP by n |
| `:c heal <n>` | | Raise current HP by n |

- These can be combined with each other and with the other `:c` options.
- With no character selected, `:c name ...` names the new character. It is also allowed in a player's
  `:c` request for a new character, and the GM's y/n question shows it.
- `hp`, `damage` and `heal` need selected characters. Without a selection the log says "Select the
  characters first".
- The console splits arguments on spaces, except inside double quotes: `:c name "Mao Clausewitz"`.
  An unclosed quote runs to the end of the line. This applies to all commands.
- Names longer than 32 bytes are refused with "Names are 1 to 32 bytes long".
- Negative or missing numbers after `damage` / `heal` are refused with "Expected a number after ...".

## Rules for HP

- Current HP is always clamped to [−Max HP, Max HP].
- `damage` and `heal` add to the current value, so changes from several people at the same time add up.
- Changing Max HP with `:c h <n>`: a character that was at full HP stays at full HP (current = new max).
  Otherwise current HP is clamped to the new range. A new character starts at full HP (current = max), as now.
- The person who made the change gets a log line per character:
  `Mao: 14/20 → 10/20 (Injured)`. The stage words are Healthy, Injured, Dying, Critical and Dead.

## Who may do what

- The GM may change the name and HP of any character.
- A player may change the name and HP of characters they own. For anyone else's character they get
  "Not your character".
- All other `:c` options stay GM-only.
- Playing without a session counts as being the GM.

## Network

- New op `OP_SET_HEALTH`: ids, a mode (set or change) and a value. The host checks ownership per id and
  applies it; set uses the value, change adds it. The result is clamped as above. Clients receive the op
  like every other op.
- New op `OP_SET_NAME`: ids and a name. Same permission rule.
- Both ops are checked like the others (`ops_check`): the counts are limited, the name must be terminated and
  free of control characters.

## The HP panel

Who sees which panels:
- A player sees a panel for every character they own, always, whether selected or not.
- The GM sees panels for the selected characters.

Layout:
- Panels are stacked downwards from the top right corner, in the order the characters were created (by id),
  with a small gap between them and from the window edge.
- If not all fit above the console, the last line shows `+N more`.
- Sizes follow the UI scale (Ctrl + `+` / `-`).
- Each panel:
  - The character's name on top, right-aligned with the frame. Dead characters show "<name> (dead)".
  - Below it the frame: it stands for 0 to Max HP, filled red from its left edge in proportion to the
    current HP, over a dark background.
  - `current/max` centred inside the frame, in white.
  - A thin line in the middle of the frame (½ Max HP): below it the character is injured.
- While dying (HP ≤ 0):
  - The red fill grows to the left, out of the frame, starting at the frame's left edge: its length is
    |HP| / Max HP of the frame's width. At −Max HP it is as long as the frame.
  - A second line at −½ Max HP, left of the frame, marks where the character becomes critical. It is only
    drawn while the character is dying.
  - Room for the negative part is reserved on the left of every panel, so the frames don't move.
- Max HP 0: the frame is empty and shows `current/0`, with no lines.

## Out of scope

- HP on the tokens on the map.
- Stamina, Vigor, attributes and derived stats.
- Messages to all players when a character changes stage.

## Testing

- Unit tests:
  - the clamping and stage thresholds;
  - the full-HP rule when Max HP changes;
  - saving and loading names, and loading a v4 save;
  - encoding and decoding `OP_SET_HEALTH`, `OP_SET_NAME` and named spawns and requests;
  - quoted console arguments.
- Permission tests: a player may change their own character's HP and name, but not others'.
- e2e:
  - a player damages their own character and the GM's table shows the new HP;
  - concurrent `damage` from GM and player adds up.
- A screenshot of the panel when healthy, injured and dying, checked by eye.

## Docs

- `gmcontrols.md`: the new `:c` options, who may use them, the panel, quoting, and names in player requests.
- `playercontrols.md`: the panel and the HP commands for your own characters (short).
- `README.md` Controls section: kept consistent.
