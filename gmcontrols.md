# GM controls

The complete guide to every control and command in Tales of Honor. Players only need
[playercontrols.md](playercontrols.md).

Contents:
1. [The console](#1-the-console)
2. [Camera and view](#2-camera-and-view)
3. [Selecting](#3-selecting)
4. [Characters](#4-characters)
5. [Walls](#5-walls)
6. [Moving characters](#6-moving-characters)
7. [Copy and paste](#7-copy-and-paste)
8. [Initiative](#8-initiative)
9. [Dice](#9-dice)
10. [Tools: measure, line, area, cone, drag](#10-tools-measure-line-area-cone-drag)
11. [Waypoints and teleport](#11-waypoints-and-teleport)
12. [Background and grid](#12-background-and-grid)
13. [Hosting a session](#13-hosting-a-session)
14. [Managing players](#14-managing-players)
15. [Saving and loading](#15-saving-and-loading)
16. [Display and comfort](#16-display-and-comfort)
17. [Keyboard-only play](#17-keyboard-only-play)
18. [Command-line options and environment variables](#18-command-line-options-and-environment-variables)
19. [What players can and cannot do](#19-what-players-can-and-cannot-do)

## 1. The console

The console at the bottom of the window takes all commands. It is hidden when the game starts: press `:` or Ctrl + Enter to open it.

| Input | Action |
|---|---|
| `:` (German keyboard: Shift + `.`) | Open the console with `:` typed |
| Ctrl + Enter | Open / close the console |
| Alt + Enter | Show / hide the log |
| Enter | Run the command |
| Up / Down | Earlier commands |
| Tab | Complete a command or argument, long or short (`:c` offers `:c`, `:cl`, `:claim`; `:c ms` becomes `:c maxstamina`). After a command and a space it lists the arguments. Tab again cycles. |
| Left / Right, Home / End, Backspace / Delete | Edit the line |
| Ctrl + V | Paste text (line breaks become spaces) |
| Mouse wheel over the console | Scroll the log (long lines wrap to the console's width; a new message scrolls back to the newest line) |
| `:h` / `:h <command>` | List the commands / help for one command |

Most commands have a long and a short form (`character` / `:c`). Double quotes keep spaces inside one argument:
`:c name "Mao Clausewitz"`. When the game asks a yes/no question in the
console, type `y` or `n` (or `yes` / `no`) and press Enter. Enter alone also means yes. Anything else is not
taken as an answer: the log says "Please answer y or n" and the question is asked again.

## 2. Camera and view

| Input | Action |
|---|---|
| Mouse wheel | Zoom at the mouse position |
| W A S D | Move the view; hold Shift = fast, Ctrl = medium, Alt = stop |
| Drag tool (Alt + X), hold the right mouse button and drag | Move the view with the mouse |
| `tp <row> <col>` | Centre the view on a tile (see [section 11](#11-waypoints-and-teleport)) |
| `:d` | Debug mode: shows the tile coordinates `[row|col]` at the mouse |

Camera, zoom and theme are stored in save files but don't count as unsaved changes.

## 3. Selecting

| Input | Action |
|---|---|
| Left click | Select a tile and the entity on it (click again to deselect). On a tile several characters share, click the picture you mean |
| Ctrl + left click | Add tiles / entities to the selection |
| Ctrl + left drag | Select every tile the mouse passes over |
| Ctrl + A | Select all entities |

Many commands act on the selection: `:c` (edit characters), `:w` (walls on the selected tiles), `:i add`,
`:i remove`, Ctrl + C.

## 4. Characters

### Create

**In the bar.** With no character on the board, the bar at the bottom shows **+ Create character**. The small
**+** box at the bottom left of the bar starts the same steps; it is always there for you (players never see it)
and hides while you create. In the tile menu of an empty tile (right click, Drag tool) **Create character** starts
them with that tile already chosen (while you are creating one, that starts over: the draft is dropped). The
steps are shown inside the bar; the tab says `New character · 2/5`.

| Step | What you do |
|---|---|
| 1 Name | Type it (1 to 32 bytes; typing past 32 does nothing). Enter = Next, Ctrl + V pastes (line breaks become spaces). The field gets the focus when the step opens; while it has it, keys go to it, not to the map (W A S D don't move the view, `:` doesn't open the console; Ctrl + Enter and Alt + Enter still work). Esc, a click elsewhere or any right click takes the focus away. |
| 2 Size | Wee to Gigantic; Normal is chosen. Hover a size for its tiles and bulk, e.g. "Huge: 3 tiles in a triangle, bulk 3", "Gigantic: 7 tiles, bulk 5 centre, 3 around". |
| 3 Attributes | STR, AGI, COG, SPI with − / +, −2 to 3; the line below shows Max HP, Stamina, Speed and Initiative (see **Attributes**) |
| 4 Picture | **Choose picture…** opens the PNG dialog; the preview shows the round cut |
| 5 Summary | The picture with `name · size` beside it and the stats below, e.g. `STR 1 AGI -1 COG 0 SPI 0 · HP 15 · Stamina 7 · Speed 2 · Initiative 0`. **Place** (see below); started from the tile menu the button is **Create** and puts the character on that tile at once (no room check). |

**Cancel** is on every step ("Character creation cancelled"); **< Back** (from step 2 on) keeps what was entered.
**Next** is greyed out until there is a name and a picture; hovering it says "Give the character a name" or
"Choose a picture first".

**Placing.** After **Place** the log says `Click a tile to place Mao (Esc: back)` and the bar shows its normal
content again. The picture follows the mouse, its tiles green where it fits (see **Sharing tiles**) and red where
not. Left click a green tile to place it; on a red one the log says "No room on that tile". Esc, or a right click
without moving, goes back to the summary (from there **Cancel** stops). The camera still works (W A S D, arrow
keys, mouse wheel, and a right drag with the Drag tool), and clicks on the toolbox still choose tools.

Your new character has full HP and stamina and no owner; give it away with `:c owner`, put it into a party with
`:c party`. A player's character made this way comes to you as a yes/no question (see section 14).

**Typed.** `:c [stats]` with no character selected creates one, then opens a file dialog for its image (PNG). It
is placed on:
- the selected tile, if exactly one is selected;
- every selected tile, if several are selected (the console asks first);
- otherwise the tile under the mouse.

Stats can be given right away, e.g. `:c name Mao size 3 str 1 agi 2` or
`:c name Mao maxhealth 30 maxstamina 9 speed 6 initiative 2`. Attributes fill in the stats they give unless you give
those too. Without `size` a character is Normal (1).
**Set a speed:** a new character without `agi` or `speed` has speed 0 and cannot move.

### Edit

Select one or more characters, then:

| Command | Short | Action |
|---|---|---|
| `:c size <n>` | `sz` | Size class of Tales of Honor, see the table below |
| `:c maxhealth <n>` | `h` | Maximum health |
| `:c maxstamina <n>` | `ms` | Maximum stamina; 0 (the default) means the character has no stamina |
| `:c speed <n>` | `s` | Movement range in tiles (5 ft each), at most 1000 |
| `:c initiative <n>` | `in` | Initiative stat |
| `:c icon` | `ic` | Choose a new image |
| `:c owner <player>` | `ow` | Give the characters to a player (by their name) |
| `:c owner none` | | Take them back: only the GM controls them |
| `:c str <n>` | | STR; sets max HP to 12 + 3 × STR |
| `:c agi <n>` | | AGI; sets max stamina to 8 + AGI and speed to AGI + 3 (at least 2) |
| `:c cog <n>` | | COG; sets initiative to COG |
| `:c spi <n>` | | SPI (sets nothing yet) |

Several can be combined: `:c speed 6 owner Bob`.

**Attributes.** Characters have the four attributes of Tales of Honor, −10 to 10 with `:c` ("Attributes go from
-10 to 10"); in the creation steps and in a player's request for a new character −2 to 3. Changing one
recalculates the stats it gives; a stat given by hand in the same command wins (`:c str 1 maxhealth 40` = STR 1,
max HP 40) and holds until that attribute changes again. Full HP and stamina stay full. The log shows e.g.
`Mao: STR 1 -> 2, max HP 15 -> 18` or `Mao: AGI 0 -> 1, max stamina 8 -> 9, speed 3 -> 4`. Older saves load with
all attributes at 0. Only you change the attributes of existing characters; a player's `:c str` on a selection says
"Only the GM can do that". Hover the HUD's tab to see a character's COG and SPI.

Sizes:

| Size | Name | Tiles | Bulk per tile |
|---|---|---|---|
| -2 | Wee | 1 | 0 |
| -1 | Tiny | 1 | 1 |
| 0 | Small | 1 | 2 |
| 1 | Normal (the default) | 1 | 3 |
| 2 | Big | 1 | 4 |
| 3 | Huge | 3, a triangle | 3 |
| 4 | Gigantic | 7, a tile and its six neighbours | 5 in the centre, 3 on the edges |

**Sharing tiles:** as in Tales of Honor, a tile holds 5 bulk, so characters can share one while their bulk adds up
to 5 or less (a Normal and a Small, two Smalls and a Tiny, any number of Wee). Walls fill their tile. Characters on
a shared tile are drawn side by side inside it; the selected one gets a blue circle.

Saves from older versions used other sizes; they are converted when loaded so every character keeps its
footprint (old large → Huge, old huge and gargantuan → Gigantic).

### Names, current HP and stamina

| Command | Short | Action |
|---|---|---|
| `:c name <name>` | `nm` | Name the selected characters, or the new one when creating. Use quotes for spaces: `:c name "Mao Clausewitz"`. 1 to 32 bytes. |
| `:c hp <n>` | | Set current HP |
| `:c damage <n>` | `dmg` | Lower current HP by n |
| `:c heal <n>` | | Raise current HP by n |
| `:c stamina <n>` | `st` | Set current stamina (can go below 0, down to −max) |
| `:c spend <n>` | | Lower current stamina by n; refused if it would go below 0 |
| `:c recover <n>` | | Raise current stamina by n, up to the maximum |

- You can change the name, HP and stamina of any character. Players can change them for their own characters.
- HP goes from max HP down to −max HP, as in Tales of Honor:
  - Healthy at ½ max HP or more;
  - Injured at 1 or more;
  - Dying at 0 or below;
  - Critical below −½ max HP;
  - dead at −max HP.
- After each change your log shows the result, e.g. `Mao: 14/20 -> 10/20 (Injured)`.
- `damage` and `heal` add to the current HP, so changes from several people at once add up. HP and stamina values
  go from -1,000,000 to 1,000,000 (max stamina from 0), and one command takes at most 8 HP or stamina changes.
- `hp`, `damage`, `heal`, `stamina`, `spend`, `recover`, `name` and `party` on a selection without characters say "Select the characters first".
- `stamina`, `spend` and `recover` on a character without max stamina say "<name> has no stamina; give it some with
  :c maxstamina <n>".
- A character at full HP stays at full HP when you change its max HP.
- **Stamina:** a character with max stamina above 0 has a stamina stat, as in Tales of Honor.
  - `spend` is refused with "Not enough stamina" if the result would be below 0. If several characters are selected
    and any of them can't pay, nothing is spent. The host checks it, so two people spending at once can't get past 0.
  - `recover` stops at the maximum.
  - Only `:c stamina <n>` can set it below 0.
  - At 0 or below the character is exhausted: its name shows "(exhausted)" and the log line ends with " (exhausted)".
  - The log shows e.g. `Mao: stamina 9/9 -> 6/9`.
  - A character at full stamina stays full when you change its max stamina.
  - Players can also give `maxstamina` in a request for a new character; the GM's question shows " ST n".
- **HUD** at the bottom centre of the window (see [section 16](#16-display-and-comfort)). It shows one character:
  - the selected one; if several are selected, the oldest, and the tab says `+N` for the others;
  - with nothing selected, a player sees their own oldest character; you see none (the bars stay empty).
  - A player's HUD shows characters they own and characters in a party with one of theirs (see **Parties**); if
    they select anyone else's, it still shows their own.
  - The tab above the panel shows the name, with "(dead)" at −max HP or "(exhausted)" at 0 stamina or below.
    Its ✕ clears the selection.
  - A red HP bar with `current/max` and a line in the middle where the character becomes injured. Left of its
    frame is the zone below 0 HP: while the character is dying the red fill grows into it from the frame's left
    edge, and a tick in its middle shows where the character becomes critical.
  - Beside the HP bar the character's STR (left) and AGI (right); hovering the tab shows its COG and SPI
    (`COG 0 · SPI 0`).
  - Under it a green stamina bar with `current/max`, if the character has max stamina (below 0 it grows to the
    left in the same way).
  - With no character to show at all (you: none on the board; a player: none of their own) the tab says "No
    character" and the panel shows **+ Create character** instead (see **Create**); a player's waiting requests
    for new characters show under it as `Waiting for the GM: Mao`.
  - When HP or stamina changes, the bar slides to the new value; a loss leaves a lighter part for a moment that
    shows how much was lost.

### Parties

| Command | Short | Action |
|---|---|---|
| `:c party <name>` | `pt` | Put the selected characters into a party, or the new one when creating (`:c name Mao party Heroes`). Quotes for spaces: `:c party "Silver Hand"`. 1 to 32 bytes. |
| `:c party` | `pt` | As the last option, with no name after it: take the selected characters out of their party |
| `:party` | | List the parties: `Heroes: Mao (anna), Kiri (ben), Brann (NPC)` |

- A party is a name on characters; NPCs (characters without an owner) can be in one. A character is in at most
  one party. Names ignore case: "Heroes" and "heroes" are the same party.
- Only you set parties. A player's `:c party` says "Only the GM can do that".
- The word after `party` is always the name: `:c party size 3` makes a party called "size". Only `party` at the end
  of the line leaves the party; with nothing selected it says "Select the characters first".
- Your log shows each change, e.g. `Mao: party Heroes` or `Mao: no party`.
- Players see the HUD (HP and stamina) of every character that shares a party with one of their own, when they
  select it. Party members still can't change each other's characters.
- `:party` lists parties in creation order. Players see only the parties their own characters are in. With
  nothing to list it says "No parties" (players: "None of your characters is in a party").
- This only decides what the HUD shows: every player's game receives all HP values.

### Delete

`:c delete` (or `:c d`) deletes **every character on the table**, not just the selected ones.

## 5. Walls

1. Select the tiles (Ctrl + click / Ctrl + drag).
2. `:w` and choose an image in the file dialog.

`:w delete` (also `:e delete`) deletes **every wall on the table**.

## 6. Moving characters

| Input | Action |
|---|---|
| Left click | Select the character; its movement range is shown in green |
| Hold Left Shift | Preview where it would stand and the path |
| Shift + left click inside the green range | Move it there |
| `:gm` | Gamemaster mode on / off: a plain left click on any tile moves the selected character there, without range limits |

The green range and the path only use tiles the character fits on (see **Sharing tiles** in section 4), so it can
walk through and stop on tiles other characters leave room on. You can still put characters anywhere: with `:gm`,
or by creating them on a tile with `:c` or the tile menu's **Create character**, no bulk is checked.

`:gm` only works for the GM (not for joined players). These free moves stay yours during initiative too; players
then move with move actions (below).

### Move actions (during initiative)

While initiative runs, a character moves with the move actions of Tales of Honor, on its turn. **Right click** the
character with the Drag tool: its action menu opens (see **The action menu** in section 10). Choose Major action
or Minor action, then the move:

| Action | Type | Moves | Stamina | Needs |
|---|---|---|---|---|
| Walk | Major | half its speed, rounded up | 0 | |
| Run | Major | its speed | 2 | |
| Crawl | Major | half its speed, rounded up | 2 | |
| Step | Minor | 1 tile | 1 | |
| Sprint | Minor | its speed | 3 | another move action earlier this turn |

The tiles the action can reach light up in its color (Major blue, Minor gold) and the way to the tile under the
mouse is drawn in white. Left click a lit tile to move there; Esc, a right click or a left click elsewhere cancels
("Move cancelled"). Starting to create a character, or placing one, also ends it (without a log line). The log
shows e.g. `Mao: Walk up to 3 tiles; click where to go (Esc cancels)` and then `Mao: Run, stamina 6/8`.

- Each turn a character has one Major and one Minor action; both are free again when its turn starts. When the
  Minor one is used, a Minor action uses the Major one instead.
- The move goes from tile to adjacent tile, only over tiles the character fits on, like the green range.
- The stamina cost is paid when the character moves. It needs that much stamina ("Not enough stamina"). At 0 it is
  exhausted, the log says `Mao is exhausted; the turn ends` and the turn goes to the next one in the order. A
  character without max stamina pays nothing.
- Speed 0: Walk, Run, Crawl and Sprint are greyed out ("This character can't move (speed 0)"); Step still moves 1 tile.
- Outside initiative the move actions are greyed out ("Only during initiative"); characters move freely then.
  Test mode (`:tm`, below) lifts this and the other requirements.
- Greyed-out actions say why when hovered: "Not this character's turn", "No action of that kind left this turn",
  "Needs another move action first this turn", "Not enough stamina".
- The rules hold for you too when you use a move action. Your Shift + click and `:gm` moves stay free and use no
  action.
- Players' Shift + click moves are refused during initiative with "During initiative, move with a move action:
  right click your character".
- Levels, terrain and conditions are not part of the map yet, so Jump, Long Jump, Climb, Swimming, Glide, Hang and
  Turn Around are not in the menu, and nothing checks whether a character is prone or falling.

### Recovering stamina (during initiative)

The same menus hold the two personal actions of Tales of Honor that only recover stamina. They take effect as soon as
you choose them; there is no tile to pick.

| Action | Type | Effect | Stamina |
|---|---|---|---|
| Recover | Minor | recovers the Stamina Recovery Die, 1d4 | 0 |
| Full Recover | Major | stamina back to its maximum | 0 |

The log shows e.g. `Mao: Recover 1d4 = 3, stamina 5/8` or `Mao: Full Recover, stamina 8/8`.

- They follow the turn rules of the move actions: only during initiative, on the character's turn, one Major and
  one Minor action per turn, and a second Recover can use the Major action. Recover doesn't count as a move action
  for Sprint.
- Greyed out with "Stamina is already full" at full stamina, and with "This character has no stamina" for a
  character without max stamina.
- Stamina never goes above the maximum. From below 0, Recover adds the roll and Full Recover goes straight to the
  maximum.
- In a session your game (the host) rolls the die and everyone sees the roll in the log.

### Test mode

| Command | Short | Action |
|---|---|---|
| `testmode [on\|off]` | `:tm` | Test mode on / off for the whole table (without an argument it toggles; Tab completes `on` / `off`) |

Test mode is for trying things out without setting up turns and stamina first. While it is on, move actions and the
recover actions (Recover, Full Recover) work for everyone at the table:
- outside initiative, and for any character, not only the one whose turn it is;
- when the Major or Minor action is already used this turn;
- for Sprint without another move action before it;
- with too little stamina (move actions).

The actions still have their effects: they use up their Major or Minor action, and they spend stamina, but never
below 0. A character whose stamina an action brings to 0 is exhausted and its turn ends, as usual; actions at 0
stamina don't end the turn again.

Still checked in test mode: players only act with their own characters, the move stays within the action's range,
a character with speed 0 can only Step, and the recover actions need a character with max stamina whose stamina isn't
full. Players' Shift + click moves keep their rules (refused during
initiative). Roll and claim limits stay too.

- Only the GM can switch it. Everyone gets `Test mode on: actions ignore turns, used actions and stamina` or
  `Test mode off` in the log, and a gold **TEST MODE** label shows at the top of the map while it is on.
- Players who join while it is on get it too. It is not saved, and it is off at every start. `:tm off` when it is
  already off says `Test mode is already off`.

## 7. Copy and paste

| Input | Action |
|---|---|
| Ctrl + C | Copy the selected entities |
| Ctrl + V | Paste them at the tile under the mouse, in the same arrangement. Each paste is relative to the previous one, so pasting repeatedly makes a row. |

Pasted characters keep their name, HP, attributes and party but have no owner.

## 8. Initiative

| Command | Short | Action |
|---|---|---|
| `:i` | | Add every entity to the initiative order |
| `:i add` | `:i a` | Add the selected entities |
| `:i remove` | `:i r` | Remove the selected entities |
| `:i next` | `:i n` | Next turn. The first `:i next` starts the round. |
| `:i show` | `:i s` | Show / hide the turn markers |
| `:i clear` | `:i c` | End initiative |

The order is random; the characters' initiative stat doesn't change it. A marker above each entity shows its
place in the order (counting from 0): green = current turn, yellow = next, red = later.

While initiative runs, players move their characters only with move actions on their own turn (section 6), and
only the owner of the current character (or the GM) can use `:i next`.

## 9. Dice

`:r <dice>`, e.g. `:r 1d20+3`, `:r 2d6`, `:r 4d8-2`. Up to 1000 dice. The dice are shown on screen; long rolls
only show the total in the log. In a session your game rolls for everyone, and everyone sees each roll.

## 10. Tools: measure, line, area, cone, drag

Choose a tool by clicking it in the toolbox on the left, or with these shortcuts. Tools use the **right** mouse
button.

| Shortcut | Tool | Use |
|---|---|---|
| Alt + X | Drag | Hold the right mouse button and drag to move the view; a right click without moving opens the tile menu, on a character the action menu (below) |
| Alt + S | Measure | Right click to start, right click to finish; several measurements add up (5 ft per tile, total shown at the mouse). Finishing on the start point clears all measurements. |
| Alt + V | Line attack | Right click the start; the line follows the mouse and highlights the tiles it hits |
| Alt + A | Area attack | Right click the centre; the radius follows the mouse |
| Alt + C | Cone attack | Right click the tip; the cone points to the mouse. Shift + mouse wheel changes its angle. |

Hold Ctrl while aiming to aim at the exact mouse position instead of a tile's middle. Switching tools removes the
current attack and any unfinished measurement. In a session everyone sees your selection, measurements and attacks
in your color (the GM is gold).

### The tile menu

With the Drag tool (the tool selected at start), **right click** a tile without moving the mouse (moving more than
a few pixels is a drag): a ring menu opens around that tile. It stays on the tile when the view moves, and moves
inside the window near the edges.

| Input | Action |
|---|---|
| Left click an entry | Use it. The menu closes. |
| Left click an entry with a small triangle | Open its submenu: the ring shrinks into an inner ring, the submenu's ring opens around it, and the entry that opened it stays highlighted |
| Left click the centre | On the first ring: the menu's main action. In a submenu: back one level |
| Left click the highlighted entry in the inner ring | Back one level |
| Left click another triangle entry in the inner ring | Switch the outer ring to that submenu |
| Hover an entry | Its name; a greyed-out entry can't be used right now and says why |
| Esc, a left click outside, or a right click on the menu or its tile | Close the menu |
| Right click another tile | Close the menu and open it on that tile |

On a character you control (you: every character; players: their own) the menu is the character's action menu
(below). A tile with another character gets a slightly different menu than an empty one. The menu of a tile
without a character has **Create character** (only for you; greyed out on a wall: "Needs an empty tile"): it starts
the creation steps with that tile (section 4). The other entries are placeholders: choosing one writes
`Menu: <entry> on [row|col]` to the log. "Action 3" (and "Option A4" in Menu A) needs something on the tile, and
"GM action" is only shown to you. While the menu is open, a left click never reaches the map (outside
the menu it only closes it), and the mouse wheel and the pointer over the menu don't either. The menu needs a mouse:
keyboard-only play can't open it.

### The action menu

Right click a character you control. If several share the tile, the menu is for the selected one, else the one
whose turn it is, else the first. Its four entries, clockwise from the top:

| Entry | Color | What it holds |
|---|---|---|
| Minor action | gold (#84672B) | Step, Sprint, Recover |
| Info | grey (#5E5E5E) | nothing yet; choosing it only closes the menu |
| Mental action | violet (#80537E) | nothing yet: greyed out, "Nothing to choose here" |
| Major action | blue (#355C85) | Walk, Run, Crawl, Full Recover |

The move and recover actions and their rules are in section 6. The centre button closes the menu.

## 11. Waypoints and teleport

| Command | Action |
|---|---|
| `tp <row> <col>` | Centre the view on a tile. Also accepts `row,col`, `row|col` and `[row|col]` (as debug mode shows). |
| `tp <name>` | Centre the view on a waypoint |
| `tp waypoint` | List the waypoints (`wp` works instead of `waypoint`) |
| `tp waypoint create <name> [row col]` | Create a waypoint, or move an existing one. Without coordinates it uses the last selected tile, else the middle of the screen. |
| `tp waypoint delete <name>` | Delete a waypoint |

Players can jump to waypoints but not create or delete them.

## 12. Background and grid

| Command | Action |
|---|---|
| `:b` | Choose a background image (file dialog) |
| `:b <file.png>` | Use that file as the background |
| `:g` | Switch between flat and pointy hexagons (unstable) |

## 13. Hosting a session

| Command | Action |
|---|---|
| `:host [name]` | Host this table through the relay. The join code is copied to the clipboard and shown in the log. Your name defaults to `GM`. |
| `:host --direct [name]` | Host without the relay: players connect straight to this computer (TCP port 41234). The game tries to open the router port; the log says if that failed (then forward the port by hand or use a VPN such as Tailscale). |
| `:unhost` | End the session. Players keep a copy of the table. |

Every `:host` makes a new code, so old codes stop working. Players need the same version of the game as you;
otherwise they get "The host runs a different version of ToH". Up to 32 players can join. A joining player's table
is replaced by yours. Your table changes are sent to everyone right away, and `:load` sends the new table to all
players.

## 14. Managing players

| Command | Action |
|---|---|
| `:players` | Who is at the table, with their colors |
| `:c owner <name>` | Give the selected characters to a player |
| `:kick <name>` | Remove a player; they can rejoin with the same code |
| `:ban <name>` | Remove a player and keep them out: both their name and their address (IPv4 address or IPv6 /64) are turned away |
| `:ban` | List the bans |
| `:unban <name>` | Lift a ban |

Bans last until `:unban` or the end of the session. Players behind the same router share its address, so at
school a ban can also keep out classmates. The log then says "Turned away ... (same address)"; `:unban` them if
needed.

**Claims:** a player can select an unowned character and type `:claim`. The console then asks you
"<player> wants to claim character #... Allow? (y/n)". Type `y` or `n` and press Enter (Enter alone is yes;
anything else asks again). Several requests wait in line; each player can have at most 3 waiting.

**Player-created characters:**
- Players can create their own character through the bar's **+ Create character** (while they have none of their
  own; the same steps as yours, see section 4) or with `:c [stats]` on an empty tile, and a picture (at most
  512×512). Their attributes go from −2 to 3 ("Attributes go from -2 to 3").
- Your log says "<player> asks for a new character.", and the console asks, for example,
  `Bob: new character Mao [2|7] size 1 HP 15 speed 5 init 0 ST 10 STR 1 AGI 2 COG 0 SPI 0. Allow? (y/n)` (the name
  only if they gave one). Until you answer, the player's bar shows `Waiting for the GM: Mao`.
- If a player's request can't be sent (their log says why, e.g. "The picture is too large" or "Could not send that
  to the host"), their draft is kept: they are still placing it (or on the summary) and can try again.
- While the question is open, only your screen shows the picture faintly on that tile, so you can check it.
- On `y` the character is created for everyone with full HP and stamina and belongs to that player ("The GM
  accepted your character."); on `n` the player is told "The GM declined your character."
- If the character no longer fits there (see **Sharing tiles**), nothing is created and the log says so.
- Change its stats afterwards with `:c` like any other character.
- These requests share the limit of 3 waiting requests per player with claims. With 3 new characters waiting, the
  player's **+ Create character** is greyed out ("Wait for the GM to answer your requests"); a request over the
  limit (claims counted too) is refused with "Wait for the GM to answer your earlier requests".

**Ownership rings:**
- During a session every owned character has a ring in its owner's color: blue, red, …, or grey if the owner isn't
  at the table.
- Each player sees the rings of their own characters thicker.
- Characters without a ring are yours.

## 15. Saving and loading

| Command | Action |
|---|---|
| `:save <file>` | Save as `<file>.tuhh` and remember it as the current file |
| `:save` | Save to the current file, or choose one in a dialog |
| `:load <file>` | Load a saved table |
| `:load` | Choose a file to load in a dialog |
| `:load! [file]` | Load and throw away unsaved changes |
| `:q` | Quit (refused if there are unsaved changes) |
| `:q!` | Quit without saving |

Paths may contain spaces, with or without quotes around them; `~/` means your home folder. The window's close button always quits.

A save holds the background, characters (image, stats, attributes, current HP and stamina, name, owner, party and
the actions used this turn), walls, initiative, waypoints, grid orientation,
camera and theme. It does not hold the console, selections, clipboard or measurements. Saves from older versions
still load; an armor class stored in them is ignored. Loading a save shows its theme, but the game keeps remembering
the theme you chose with `:t` for the next start.

## 16. Display and comfort

| Input | Action |
|---|---|
| `:t` | List the color themes |
| `:t <name>` | Choose a theme: purple, red, green, yellow, light, dark, black; the colours fade over a quarter second (the HUD outline follows the theme). The game remembers it for the next start (see below) |
| Ctrl + `+` / Ctrl + `-` | Bigger / smaller console text, toolbox, HUD and radial menu (remembered for the next start) |
| `:hud [on\|off]` | Show / hide the HUD (without an argument it toggles; Tab completes `on` / `off`) |
| 1 … 9, 0 (console closed; also on the keypad) | Use hotbar slot 1–10 (once per key press; holding does not repeat) |
| `:cl` | Clear the log and command history |
| `:d` | Debug mode on / off (tile coordinates at the mouse) |

The HUD at the bottom centre holds the character's tab, its HP and stamina bars, its STR and AGI beside the HP bar
(they show "—" while no character is shown) and a hotbar of ten slots (keys 1–9 and 0). While you create a
character the steps replace this content (section 4). The hotbar has no effect yet; in debug mode (`:d`) using a
slot writes a log line. The HUD is shown at every start; `:hud off`
hides it until the next start. The map does not react to the pointer over the HUD: no tile is highlighted under it,
and clicks, the mouse wheel and the right button there don't reach the map (a camera drag can cross it). The keyboard
cursor scrolls the view when it would move under the HUD.

### Settings file

The game remembers your settings in `settings.txt` in the game's folder (next to the program):

| Key | Setting |
|---|---|
| `theme` | The theme chosen with `:t`, e.g. `theme = dark` |
| `ui_scale` | The UI size from Ctrl + / Ctrl −: steps from the normal size, e.g. `ui_scale = 3` (0 = normal, −24 to 64) |

It is a plain text file with one `key = value` per line; the game
rewrites it when a setting changes and keeps any other lines (comments starting with `#`, keys it doesn't know). You
can edit it while the game is closed. A missing file or an unknown theme in it means the default theme, Light; a
`ui_scale` that isn't a whole number from −24 to 64 means the normal size. The
file is in `.gitignore`, so it never ends up in the repository.

## 17. Keyboard-only play

| Input | Action |
|---|---|
| Arrow keys | Move a cursor from tile to tile (the view follows) |
| Right Shift + Left / Right | Move the cursor along the other diagonals |
| Enter | Left click on the cursor |
| Right Shift + Enter | Right click on the cursor (tools) |
| Right Shift + Up / Down | Zoom in / out at the cursor |

While placing a new character (section 4), Enter places it on the cursor's tile and Right Shift + Enter goes back to
the summary.

## 18. Command-line options and environment variables

```
./toh [--port N] [--size WxH] [--exec <line>]... [--auto-file <png>]
```

| Option | Meaning |
|---|---|
| `--port N` | Port for `:host --direct` (default 41234) |
| `--size WxH` | Window size |
| `--exec <line>` | Run a command after start (one per frame; mostly for tests) |
| `--auto-file <png>` | Every file dialog returns this file (for tests) |

| Variable | Meaning |
|---|---|
| `TOH_RELAY=host:port` | Use another relay; `TOH_RELAY=` (empty) turns the relay off |
| `TOH_NO_PORTMAP=1` | `:host --direct` doesn't touch the router |
| `TOH_VK_VALIDATION=1` | Vulkan validation layer (debugging) |
| `TOH_SETTINGS=<file>` | Read and write the settings there instead of `settings.txt` next to the program; `TOH_SETTINGS=` (empty) uses no settings file, so nothing is remembered (the tests do that) |

## 19. What players can and cannot do

Players can:
- move their own characters (freely outside initiative; while it runs, with move actions on their turn), and
  recover their stamina with Recover and Full Recover on their turn;
- end their turn with `:i next`;
- roll dice (at most 10 rolls in 10 seconds);
- ask for existing characters with `:claim`, or for a new one of their own through the bar's **+ Create
  character** (while they have none) or `:c` (attributes −2 to 3; you answer y/n);
- jump to waypoints and coordinates;
- use the measure and attack tools, the tile menu (without the entries only you see) and their own characters' action menu;
- save a copy of the table;
- change the HP, stamina and name of their own characters (`:c hp`, `damage`, `heal`, `stamina`, `spend`, `recover`, `name`), and give max stamina in a request for a new character;
- change their own camera, zoom, theme and HUD (`:hud`), and see the HP and stamina of their own characters and of the members of their parties (select them), and list their parties with `:party`.

Everything else is yours, including:
- creating characters directly (also with the bar's + box and the tile menu's Create character), editing
  characters' other stats, changing attributes (`:c str` etc.) after a character exists, deleting characters, and
  walls;
- giving characters to players, and putting characters into parties (`:c party`);
- the initiative order (apart from `:i next`);
- waypoints, the background and the grid;
- `:gm`, `:tm` (test mode), `:load`, kicking and banning.

If a player tries any of these, they get "Only the GM can do that".
