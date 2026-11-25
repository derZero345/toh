# Player controls

The basics for playing at someone else's table, most important first. The GM's full guide is
[gmcontrols.md](gmcontrols.md).

Commands are typed into the console at the bottom of the window. Press `:` (on a German keyboard: Shift + `.`)
to open it with a `:` already typed, write the command and press Enter.

## 1. Join the table

```
:join <code> <your-name>
```

The GM gives you the code (it starts with `toh1-`); you can paste it with Ctrl + V. Your name can be up to
24 characters, without spaces. After you join, your table is replaced by the GM's.

## 2. Move your character

1. **Left click** your character to select it. A green area shows how far it can move.
2. **Hold Left Shift** to preview where it would end up and the path it would take.
3. **Shift + left click** a tile inside the green area to move there.

You can only move characters the GM gave you. While initiative is running, you can only move on your
character's turn.

## 3. End your turn

```
:i next
```

Ends your character's turn, so the next one in the initiative order is up. While initiative runs, a marker above
each character shows the order: **green** = whose turn it is, **yellow** = next, **red** = later.

## 4. Roll dice

```
:r 1d20+3
```

Format: `<number>d<sides>`, optionally followed by `+<n>` or `-<n>` (for example `:r 2d6`, `:r 1d8-1`). The GM's
game rolls, and everyone sees the result. You can roll at most 10 times in 10 seconds.

## 5. Look around

| Input | Action |
|---|---|
| Mouse wheel | Zoom in / out |
| W A S D | Move the view (hold Shift for fast, Ctrl for medium) |
| Drag tool (Alt + X), then hold the right mouse button and drag | Move the view with the mouse |
| `tp <waypoint>` | Jump to a waypoint the GM made (`tp waypoint` lists them) |
| `tp <row> <col>` | Jump to a tile |

## 6. Get a character

**Claim one that exists:** select a character that has no owner yet, then type `:claim`. The GM answers yes or
no. You can have at most 3 requests waiting at once.

**Create your own (coming soon, not in the game yet):** with nothing selected, click an empty tile and type
`:c` with your stats, for example `:c speed 6 maxhealth 20 armorclass 12`. Then pick a picture. The GM sees
your stats and the picture and accepts or declines. The rest of the table only sees your character once the GM
says yes, so you can surprise the party. Until then, ask the GM to create the character for you.

## 7. Measure and aim

Pick a tool in the toolbox on the left, or with these shortcuts:

| Shortcut | Tool | How to use it (right mouse button) |
|---|---|---|
| Alt + S | Measure | Right click to start, right click again to finish. Shows the distance in feet (5 ft per tile). Right clicking the start point again clears all measurements. |
| Alt + V | Line attack | Right click the start point; the line follows the mouse |
| Alt + A | Area attack | Right click the centre; the circle grows with the mouse |
| Alt + C | Cone attack | Right click the tip; Shift + mouse wheel changes the cone angle |
| Alt + X | Drag | Hold the right mouse button to move the view |

Hold Ctrl while aiming to aim at the exact mouse position instead of the middle of a tile. Everyone at the
table sees your selection, measurements and attacks in your color.

## 8. Other useful commands

| Command | Action |
|---|---|
| `:players` | Who is at the table, and their colors |
| `:save [file]` | Save a copy of the table for yourself |
| `:leave` | Leave the table (you keep a copy as an unsaved table) |
| `:q` / `:q!` | Quit / quit without saving |
| `:h [command]` | Help |

## 9. Comfort

| Input | Action |
|---|---|
| Ctrl + Enter | Open / close the console |
| Alt + Enter | Show / hide the log |
| Up / Down in the console | Earlier commands |
| Tab in the console | Complete a command |
| Mouse wheel over the console | Scroll the log |
| Ctrl + `+` / Ctrl + `-` | Bigger / smaller console text and toolbox |
| `:t [name]` | List the color themes, or choose one |
| `:cl` | Clear the log and command history |
| Arrow keys, Enter | Play without a mouse: move a cursor, Enter = left click, Right Shift + Enter = right click, Right Shift + Up / Down = zoom |

Everything else (creating characters, walls, initiative, the background) is the GM's job.
