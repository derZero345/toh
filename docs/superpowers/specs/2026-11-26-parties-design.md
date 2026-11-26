# Parties: design

Date: 2026-11-26. Status: approved.

## Goal

Players in the same party can see each other's characters in the HUD: a player who selects a party member's
character sees its tab, HP and stamina, just like for their own characters.

## What a party is

- A party is a name on characters. The GM puts characters into a party; a player sees every character in a
  party that one of their own characters is in. NPCs (characters without an owner) can be in a party too.
- A character is in at most one party.
- Party names follow the character name rules: 1 to 32 bytes, no control characters. They are compared
  ignoring case (ASCII), so "Heroes" and "heroes" are the same party.
- This only decides what the HUD shows. Every player's game already receives every character's HP; a modified
  client could still read it. That is fine for a table of friends, and the guides don't promise more.

## Data

- `Entity` gets `char party[CHAR_NAME_MAX]`, "" = no party.
- Save format v7 stores each character's party. v1–v6 still load, with no parties.
- Protocol 7: the party is sent in the full table and in spawns.
- New op `OP_SET_PARTY`: ids and a name ("" = leave the party). It is built like `OP_SET_NAME`, but GM-only:
  - `ops_check` refuses it from players with `OPR_GM_ONLY` ("Only the GM can do that"); the host checks it
    too, as for every op.
  - The structure check limits the id count and refuses a name that is not terminated or not valid (an empty
    name is valid here and means "no party").

## Commands

| Command | Short | Who | Action |
|---|---|---|---|
| `:c party <name>` | `pt` | GM | Put the selected characters into the party, or the new one when creating (`:c name Mao party Heroes`). Quotes for spaces: `:c party "Silver Hand"`. |
| `:c party` | `pt` | GM | As the last option with no name after it: take the selected characters out of their party. |
| `:party` | | everyone | List the parties and their members. |

- The word after `party` is always taken as the name, so `:c party size 3` makes a party called "size". Only
  `party` at the end of the line clears; with nothing selected it says "Select the characters first" (there is
  nothing to create).
- `party` can be combined with the other `:c` options. Like the other selection options, without a
  selection it applies to the new character being created.
- A player's `:c party ...` is refused with "Only the GM can do that"; a player's request for a new
  character can't contain `party` (same message, nothing is sent).
- A name that is too long is refused with "Names are 1 to 32 bytes long" (the existing message).
- The person who made the change gets a log line per character: `Mao: party Heroes` or `Mao: no party`.
- `:party` output, one line per party, parties and members in creation order (by id):
  `Heroes: Mao (anna), Kiri (ben), Brann (NPC)`.
  - The party's name is spelled as on its oldest member.
  - Members are shown by name, unnamed ones as `Character #<id>`, followed by their owner or "NPC".
  - The GM (and anyone playing without a session) sees all parties. A player sees only the parties their own
    characters are in.
  - With nothing to show: "No parties" (GM) or "None of your characters is in a party" (player).
- `:party` is added to `:help` and to Tab completion (no arguments). `party` is added to the `:c` arguments for
  Tab completion.

## The HUD

- `hud_can_see(e)` (src/hud_game.c) for a player in a session is true for:
  - their own characters (as now);
  - a character with a party, if one of the player's own characters is in the same party.
- The GM and offline play still see every character.
- Nothing else changes:
  - with nothing selected, a player's HUD still shows their own oldest character (not a party member's);
  - with several characters selected, the HUD shows the oldest one the player can see, and `+N` counts the
    other visible ones;
  - party members can't change each other's characters: HP, stamina, names and moves stay owner-only.
- When a party changes, the next drawn frame uses it (the HUD re-picks its character every frame).

## Out of scope

- A party list or party HP bars in the HUD, HP bars on the map.
- Players joining or leaving parties themselves.
- Hiding HP from the network data.

## Testing

- Unit tests (core):
  - `hud_can_see` with parties: same party, another party, an NPC in the player's party, a party member while
    the player owns no character, case-insensitive names, a character without a party;
  - `ops_check`: `OP_SET_PARTY` from the GM is accepted, from a player (also on their own character) refused;
    an invalid name is refused; `ops_apply` sets and clears the party;
  - `:party` output for the GM and for a player (via the stubbed log);
  - `:c party` parsing: a name, quotes, clearing at the end of the line, a too-long name.
- Save tests: a v7 round trip keeps parties; a v6 save loads with no parties.
- Netmsg tests: `NOP_SET_PARTY` and the party in the full table and in spawns encode and decode; a bad name is
  refused.
- e2e (tests/e2e_net.sh): the GM creates an NPC and a character for the player; before they share a party the
  player's `@hudrect hp` after selecting the NPC is empty; after `:c party Heroes` on both it exists.

## Docs

- `gmcontrols.md`: `:c party`, `:party`, the rule about whose HUD shows what, and the note that the GM sets
  parties.
- `playercontrols.md`: selecting a party member's character shows their HP; `:party` lists your parties.
- `README.md` Controls section: kept consistent.
