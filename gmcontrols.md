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

The console at the bottom of the window takes all commands.

| Input | Action |
|---|---|
| `:` (German keyboard: Shift + `.`) | Open the console with `:` typed |
| Ctrl + Enter | Open / close the console |
| Alt + Enter | Show / hide the log |
| Enter | Run the command |
| Up / Down | Earlier commands |
| Tab | Complete a command; after a command and a space it lists the arguments. Tab again cycles. |
| Left / Right, Home / End, Backspace / Delete | Edit the line |
| Ctrl + V | Paste text (line breaks become spaces) |
| Mouse wheel over the console | Scroll the log |
| `:h` / `:h <command>` | List the commands / help for one command |

Most commands have a long and a short form (`character` / `:c`). When the game asks a yes/no question in the
console, type `y` or `n` and press Enter.

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
| Left click | Select a tile and the entity on it (click again to deselect) |
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

Stats can be given right away, e.g. `:c size 3 maxhealth 30 speed 6 initiative 2`. Without `size` a character is
Normal (1).
**Set a speed:** a new character's speed is 0 unless you give one, so it cannot move.

### Edit

Select one or more characters, then:

| Command | Short | Action |
|---|---|---|
| `:c size <n>` | `sz` | Size class of Tales of Honor, see the table below |
| `:c maxhealth <n>` | `h` | Maximum health |
| `:c speed <n>` | `s` | Movement range in tiles (5 ft each), at most 1000 |
| `:c initiative <n>` | `in` | Initiative stat |
| `:c icon` | `ic` | Choose a new image |
| `:c owner <player>` | `ow` | Give the characters to a player (by their name) |
| `:c owner none` | | Take them back: only the GM controls them |

Several can be combined: `:c speed 6 owner Bob`.

Sizes:

| Size | Name | Tiles |
|---|---|---|
| -2 | Wee | 1 |
| -1 | Tiny | 1 |
| 0 | Small | 1 |
| 1 | Normal (the default) | 1 |
| 2 | Big | 1 |
| 3 | Huge | 3, a triangle |
| 4 | Gigantic | 7, a tile and its six neighbours |

Saves from older versions used other sizes; they are converted when loaded so every character keeps its
footprint (old large → Huge, old huge and gargantuan → Gigantic).

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

`:gm` only works for the GM (not for joined players).

## 7. Copy and paste

| Input | Action |
|---|---|
| Ctrl + C | Copy the selected entities |
| Ctrl + V | Paste them at the tile under the mouse, in the same arrangement. Each paste is relative to the previous one, so pasting repeatedly makes a row. |

Pasted characters have no owner.

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
| Alt + X | Drag | Hold the right mouse button and drag to move the view |
| Alt + S | Measure | Right click to start, right click to finish; several measurements add up (5 ft per tile, total shown at the mouse). Finishing on the start point clears all measurements. |
| Alt + V | Line attack | Right click the start; the line follows the mouse and highlights the tiles it hits |
| Alt + A | Area attack | Right click the centre; the radius follows the mouse |
| Alt + C | Cone attack | Right click the tip; the cone points to the mouse. Shift + mouse wheel changes its angle. |

Hold Ctrl while aiming to aim at the exact mouse position instead of a tile's middle. Switching tools removes the
current attack and any unfinished measurement. In a session everyone sees your selection, measurements and attacks
in your color (the GM is gold).

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
"<player> wants to claim character #... Allow? (y/n)". Type `y` or `n` and press Enter. Several requests wait in
line; each player can have at most 3 waiting.

**Player-created characters:**
- Players can create their own character with `:c [stats]` on an empty tile and a picture (at most 512×512).
- Your log says "<player> asks for a new character", and the console asks, for example,
  `Bob: new character [2|7] size 1 HP 20 speed 6 init 1. Allow? (y/n)`.
- While the question is open, only your screen shows the picture faintly on that tile, so you can check it.
- On `y` the character is created for everyone and belongs to that player; on `n` the player is told you
  declined.
- If the tile was taken in the meantime, nothing is created and the log says so.
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

Paths may contain spaces; `~/` means your home folder. The window's close button always quits.

A save holds the background, characters (image, stats and owner), walls, initiative, waypoints, grid orientation,
camera and theme. It does not hold the console, selections, clipboard or measurements. Saves from older versions
still load; an armor class stored in them is ignored.

## 16. Display and comfort

| Input | Action |
|---|---|
| `:t` | List the color themes |
| `:t <name>` | Choose a theme: purple, red, green, yellow, light, dark, black |
| Ctrl + `+` / Ctrl + `-` | Bigger / smaller console text and toolbox |
| `:cl` | Clear the log and command history |
| `:d` | Debug mode on / off (tile coordinates at the mouse) |

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
- use the measure and attack tools;
- save a copy of the table;
- change their own camera, zoom and theme.

Everything else is yours, including:
- creating characters directly, editing and deleting characters, and walls;
- giving characters to players;
- the initiative order (apart from `:i next`);
- waypoints, the background and the grid;
- `:gm`, `:load`, kicking and banning.

If a player tries any of these, they get "Only the GM can do that".
