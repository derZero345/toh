# Player controls

The basics for playing at someone else's table, most important first. The GM's full guide is
[gmcontrols.md](gmcontrols.md).

Commands are typed into the console at the bottom of the window. It is hidden at start: press `:` (on a German keyboard: Shift + `.`)
to open it with a `:` already typed, write the command and press Enter.

## 1. Join the table

```
:join <code> <your-name>
```

The GM gives you the code (it starts with `toh1-`); you can paste it with Ctrl + V. Your name can be up to
24 characters, without spaces or quotes. After you join, your table is replaced by the GM's.

## 2. Move your character

You can only move your own characters: the ones with a **thick ring in your color**. Every owned character has
a ring in its owner's color (grey if that player isn't at the table); characters without a ring belong to the GM.

**While initiative is running**, you move with move actions, on your character's turn:

1. **Right click** your character (with the Drag tool, selected at start). Its action menu opens: **Minor action**
   at the top, **Info** on the right, **Mental action** at the bottom, **Major action** on the left. Info and
   Mental action have nothing in them yet.
2. Left click Major action or Minor action, then the move:

   | Action | Type | Moves | Stamina |
   |---|---|---|---|
   | Walk | Major | half your speed, rounded up | 0 |
   | Run | Major | your speed | 2 |
   | Crawl | Major | half your speed, rounded up | 2 |
   | Step | Minor | 1 tile | 1 |
   | Sprint | Minor | your speed; only after another move action this turn | 3 |

3. The tiles it can reach light up in the action's color, with the way to the tile under the mouse. **Left click**
   one of them to move there. **Esc**, a right click or a click outside the lit tiles cancels.

Each turn your character has one Major and one Minor action. When the Minor one is used, a Minor action (Step,
Sprint) can take the Major one instead. An action that can't be used right now is greyed out; hover it to see
why. When your character's stamina reaches 0 it is exhausted and its turn ends. A character without stamina pays
nothing.

To get stamina back on your turn, choose **Recover** (Minor action: 1d4 stamina) or **Full Recover** (Major action:
all of it) in the same menus. They work at once and are greyed out when your stamina is already full.

To **attack** on your turn, choose Major action, then **Attack**, then the attack (so far only a placeholder, Test
attack: 1 tile, 1d4 damage). Left click a lit tile in range, then its direction in the ring that opens on it (Up:
away from you, Right, Neutral: the tile itself, Left). An attack hits the tile: **every character on it**, your
friends too. The tile gets a red arrow (a ring for neutral) until your next turn. Esc goes back one step.

To **block**, choose Major action, then **Block**, then the block (placeholders so far: Test block on your own tile,
or Test block (range 1) on a tile next to you, which you click first). In the ring, switch **Up**, **Right** and/or
**Left** on, then click the centre **Block**. Neutral is always blocked. Attacks on that tile in a blocked direction
then miss or scrape, until your next turn; the tile shows a blue hexagon. If you block a tile you don't stand on and
an attack gets through, you can't block that tile again on your next turn.

To **dodge**, choose Major action, then **Dodge** (2 stamina), and click an adjacent tile: your character moves
there. Until your next turn, an attack on the tile you left still targets you, against your Dodge Value (1d8 + AGI)
and your Evasion Threshold (ET): it misses or scrapes when you win, and the direction of your step (as the attacker
sees it) counts. The tile you left shows a green hexagon.

While a gold **TEST MODE** label shows at the top, the GM has turned on test mode: you can use move actions,
Recover / Full Recover, attacks, blocks and dodges at any time, even when it isn't your turn or you lack the action or
stamina. They still use up the action and stamina, and a failed block's tile stays locked.

**Outside initiative** you move freely:

1. **Left click** your character to select it. A green area shows how far it can move. If several characters
   share a tile, they stand side by side in it: click the one you mean.
2. **Hold Left Shift** to preview where it would end up and the path it would take.
3. **Shift + left click** a tile inside the green area to move there.

Characters can share a tile when there is room: a tile holds 5 bulk, and a Wee character takes 0, Tiny 1, Small 2,
Normal 3, Big 4. So a Small character can step onto a Normal one's tile, but two Normal ones can't share.

## 3. Your HP and stamina

The HUD at the bottom of the window shows your character: the one of yours you selected, or with nothing selected your oldest
one. The tab above it shows the name; while the character is selected, its ✕ clears the selection. The red HP bar shows `current/max`; the line
in its middle marks where your character becomes injured. Below 0 HP your character is dying: the bar grows to
the left out of its frame, and a tick shows where it becomes critical. When HP changes, the bar slides to the new
value and briefly shows what was lost in a lighter red. Beside the HP bar are your character's **STR** (left) and
**AGI** (right); hover the tab to see its COG and SPI.

If your character has stamina, a green bar under the HP bar shows it (`current/max`). Type `:c spend <n>` to use
stamina (refused with "Not enough stamina" if you don't have enough) and `:c recover <n>` to get it back, up to the
maximum. At 0 or below your character is exhausted. `:c stamina <n>` sets it directly.

If the GM put your character into a party, select another party member's character to see its HP and stamina
too. `:party` lists your parties and who is in them.

Select your character and type `:c damage <n>`, `:c heal <n>` or `:c hp <n>` to change its HP, and
`:c name <name>` to name it (quotes for spaces: `:c name "Mao Clausewitz"`).

## 4. End your turn

```
:i next
```

Ends your character's turn, so the next one in the initiative order is up. While initiative runs, a marker above
each character shows the order: **green** = whose turn it is, **yellow** = next, **red** = later.

## 5. Roll dice

```
:r 1d20+3
```

Format: `<number>d<sides>`, optionally followed by `+<n>` or `-<n>` (for example `:r 2d6`, `:r 1d8-1`). The GM's
game rolls, and everyone sees the result. You can roll at most 10 times in 10 seconds.

## 6. Look around

| Input | Action |
|---|---|
| Mouse wheel | Zoom in / out |
| W A S D | Move the view (hold Shift for fast, Ctrl for medium) |
| Drag tool (Alt + X), then hold the right mouse button and drag | Move the view with the mouse |
| `tp <waypoint>` | Jump to a waypoint the GM made (`tp waypoint` lists them) |
| `tp <row> <col>` | Jump to a tile |

## 7. Get a character

**Create one in the bar:** while you have no character of your own, the bar at the bottom shows **+ Create character**. Click
it and go through the steps; **Cancel** stops, **< Back** goes back without losing anything.

1. **Name**: type it (Enter = Next). While the name field has the focus, keys go to it, not to the map; Esc or a
   click elsewhere takes the focus away.
2. **Size**: Normal is chosen; hover a size to see its tiles and bulk.
3. **Attributes**: STR, AGI, COG and SPI with − and +, from −2 to 3. The line below shows what they give:
   Max HP 12 + 3 × STR, Stamina 8 + AGI, Speed AGI + 3 (at least 2), Initiative COG.
4. **Picture**: **Choose picture…** and pick a PNG (large pictures are shrunk to 512×512).
5. **Summary**: check it and click **Place**. The picture follows the mouse: green tiles = it fits, red = no room
   ("No room on that tile"). Left click a green tile. Esc or a right click goes back to the summary. If the
   request can't be sent, the log says why and you can click again.

The GM then accepts or declines; until then the bar shows "Waiting for the GM: <name>". The rest of the table
only sees your character once the GM says yes, so you can surprise the party. It then belongs to you. Only the GM
can change its attributes afterwards.

**Claim one that exists:** select a character that has no owner yet, then type `:claim`. The GM answers yes or
no.

**Or type it:** click an empty tile so that nothing else is selected, type `:c` with your stats, e.g.
`:c name Mao str 1 agi 2` (attributes −2 to 3; they fill in Max HP, stamina, speed and initiative), then pick a
picture. Without `agi` or `speed` your character can't move. The tile needs room for your character (see
section 2); otherwise you get "No room on that tile".

Claims and new characters count together: at most 3 can wait for the GM at once. With 3 new characters waiting,
**+ Create character** is greyed out.

## 8. Measure and aim

Pick a tool in the toolbox on the left, or with these shortcuts:

| Shortcut | Tool | How to use it (right mouse button) |
|---|---|---|
| Alt + S | Measure | Right click to start, right click again to finish. Shows the distance in feet (5 ft per tile). Right clicking the start point again clears all measurements. |
| Alt + V | Line attack | Right click the start point; the line follows the mouse |
| Alt + A | Area attack | Right click the centre; the circle grows with the mouse |
| Alt + C | Cone attack | Right click the tip; Shift + mouse wheel changes the cone angle |
| Alt + X | Drag | Hold the right mouse button to move the view; a right click opens the tile menu (section 9), on your character its action menu (section 2) |

Hold Ctrl while aiming to aim at the exact mouse position instead of the middle of a tile. Everyone at the
table sees your selection, measurements and attacks in your color.

## 9. The tile menu

With the Drag tool (selected at start), **right click** a tile without moving the mouse: a ring menu opens
around it. On your own character it is the character's action menu (section 2).

- **Left click** an entry to use it. An entry with a small triangle opens a second ring around the first; the
  centre button (or the highlighted entry) then goes back.
- Hover an entry to see its name. A greyed-out entry can't be used right now; hovering it says why.
- **Esc**, a click outside or another right click closes the menu. A right click on another tile opens it there.

On other tiles the entries are placeholders: choosing one writes `Menu: <entry> on [row|col]` to the log.

## 10. Other useful commands

| Command | Action |
|---|---|
| `:players` | Who is at the table, and their colors |
| `:party` | Your parties and who is in them |
| `:save [file]` | Save a copy of the table for yourself |
| `:leave` | Leave the table (you keep a copy as an unsaved table) |
| `:q` / `:q!` | Quit / quit without saving |
| `:h [command]` | Help |

## 11. Comfort

| Input | Action |
|---|---|
| Ctrl + Enter | Open / close the console |
| Alt + Enter | Show / hide the log |
| Up / Down in the console | Earlier commands |
| Tab in the console | Complete a command or argument (`:c` too); Tab again cycles |
| Mouse wheel over the console | Scroll the log |
| Ctrl + `+` / Ctrl + `-` | Bigger / smaller console text, toolbox, HUD and menus (remembered for the next start) |
| `:hud` | Show / hide the HUD |
| `:t [name]` | List the color themes, or choose one (remembered for the next start) |
| `:cl` | Clear the log and command history |
| Arrow keys, Enter | Play without a mouse: move a cursor, Enter = left click, Right Shift + Enter = right click, Right Shift + Up / Down = zoom. While placing a new character, Enter places it on the cursor's tile and Right Shift + Enter goes back to the summary; the creation steps need the mouse |

Everything else (editing your characters' other stats and attributes, deleting characters, walls, initiative, the background) is the GM's job.
