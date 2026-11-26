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

`:c [stats]` with no character selected creates one, then opens a file dialog for its image (PNG). It is placed on:
- the selected tile, if exactly one is selected;
- every selected tile, if several are selected (the console asks first);
- otherwise the tile under the mouse.

Stats can be given right away, e.g. `:c name Mao size 3 maxhealth 30 maxstamina 9 speed 6 initiative 2`. Without `size` a character is
Normal (1).
**Set a speed:** a new character's speed is 0 unless you give one, so it cannot move.

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

Several can be combined: `:c speed 6 owner Bob`.

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
  - Under it a green stamina bar with `current/max`, if the character has max stamina (below 0 it grows to the
    left in the same way).
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
or by creating them on a tile, no bulk is checked.

`:gm` only works for the GM (not for joined players).

## 7. Copy and paste

| Input | Action |
|---|---|
| Ctrl + C | Copy the selected entities |
| Ctrl + V | Paste them at the tile under the mouse, in the same arrangement. Each paste is relative to the previous one, so pasting repeatedly makes a row. |

Pasted characters keep their name, HP and party but have no owner.

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

While initiative runs, players can only move their characters on their own turn, and only the owner of the
current character (or the GM) can use `:i next`.

## 9. Dice

`:r <dice>`, e.g. `:r 1d20+3`, `:r 2d6`, `:r 4d8-2`. Up to 1000 dice. The dice are shown on screen; long rolls
only show the total in the log. In a session your game rolls for everyone, and everyone sees each roll.

## 10. Tools: measure, line, area, cone, drag

Choose a tool by clicking it in the toolbox on the left, or with these shortcuts. Tools use the **right** mouse
button.

| Shortcut | Tool | Use |
|---|---|---|
| Alt + X | Drag | Hold the right mouse button and drag to move the view; a right click without moving opens the tile menu (below) |
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

A tile with a character on it gets a slightly different menu than an empty one. The entries are placeholders:
choosing one writes `Menu: <entry> on [row|col]` to the log. "Action 3" (and "Option A4" in Menu A) needs something
on the tile, and "GM action" is only shown to you. While the menu is open, a left click never reaches the map (outside
the menu it only closes it), and the mouse wheel and the pointer over the menu don't either. The menu needs a mouse:
keyboard-only play can't open it.

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
- Players can create their own character with `:c [stats]` on an empty tile and a picture (at most 512×512).
- Your log says "<player> asks for a new character", and the console asks, for example,
  `Bob: new character Mao [2|7] size 1 HP 20 speed 6 init 1. Allow? (y/n)` (the name only if they gave one).
- While the question is open, only your screen shows the picture faintly on that tile, so you can check it.
- On `y` the character is created for everyone and belongs to that player; on `n` the player is told you
  declined.
- If the character no longer fits there (see **Sharing tiles**), nothing is created and the log says so.
- Change its stats afterwards with `:c` like any other character.
- These requests share the limit of 3 waiting requests per player with claims.

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

A save holds the background, characters (image, stats, current HP and stamina, name, owner and party), walls, initiative, waypoints, grid orientation,
camera and theme. It does not hold the console, selections, clipboard or measurements. Saves from older versions
still load; an armor class stored in them is ignored.

## 16. Display and comfort

| Input | Action |
|---|---|
| `:t` | List the color themes |
| `:t <name>` | Choose a theme: purple, red, green, yellow, light, dark, black; the colours fade over a quarter second (the HUD outline follows the theme) |
| Ctrl + `+` / Ctrl + `-` | Bigger / smaller console text, toolbox and HUD |
| `:hud [on\|off]` | Show / hide the HUD (without an argument it toggles; Tab completes `on` / `off`) |
| 1 … 9, 0 (console closed; also on the keypad) | Use hotbar slot 1–10 (once per key press; holding does not repeat) |
| `:cl` | Clear the log and command history |
| `:d` | Debug mode on / off (tile coordinates at the mouse) |

The HUD at the bottom centre holds the character's tab, its HP and stamina bars, two stat fields (they show "—") and
a hotbar of ten slots (keys 1–9 and 0). The stat fields and hotbar have no effect yet; in debug mode (`:d`) using a
slot writes a log line. The HUD is shown at every start; `:hud off`
hides it until the next start. The map does not react to the pointer over the HUD: no tile is highlighted under it,
and clicks, the mouse wheel and the right button there don't reach the map (a camera drag can cross it). The keyboard
cursor scrolls the view when it would move under the HUD.

## 17. Keyboard-only play

| Input | Action |
|---|---|
| Arrow keys | Move a cursor from tile to tile (the view follows) |
| Right Shift + Left / Right | Move the cursor along the other diagonals |
| Enter | Left click on the cursor |
| Right Shift + Enter | Right click on the cursor (tools) |
| Right Shift + Up / Down | Zoom in / out at the cursor |

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

## 19. What players can and cannot do

Players can:
- move their own characters (on their turn while initiative runs);
- end their turn with `:i next`;
- roll dice (at most 10 rolls in 10 seconds);
- ask for existing characters with `:claim`, or for a new one of their own with `:c` (you answer y/n);
- jump to waypoints and coordinates;
- use the measure and attack tools, and the tile menu (without the entries only you see);
- save a copy of the table;
- change the HP, stamina and name of their own characters (`:c hp`, `damage`, `heal`, `stamina`, `spend`, `recover`, `name`), and give max stamina in a request for a new character;
- change their own camera, zoom, theme and HUD (`:hud`), and see the HP and stamina of their own characters and of the members of their parties (select them), and list their parties with `:party`.

Everything else is yours, including:
- creating characters directly, editing characters' other stats, deleting characters, and walls;
- giving characters to players, and putting characters into parties (`:c party`);
- the initiative order (apart from `:i next`);
- waypoints, the background and the grid;
- `:gm`, `:load`, kicking and banning.

If a player tries any of these, they get "Only the GM can do that".
