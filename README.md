# Tales of Honor (ToH) — C / Vulkan

A C11 rewrite of the Java/Swing hex-grid tabletop tool (originally called `pathfinder`). It renders with Vulkan
and uses SDL3 for the window, input and file dialogs.

## Building

Dependencies:

| Library | Used for |
|---|---|
| SDL3 ≥ 3.4 | window, input, file dialogs, PNG loading (`SDL_LoadPNG`) |
| Vulkan loader + headers | rendering |
| FreeType 2 + fontconfig | text (looks up "Arial", falls back to the system sans font) |
| `glslc` (shaderc) | compiles `shaders/*.vert/frag` to SPIR-V at build time |
| libwebp, libzstd, libsodium | save files: image compression, state compression, integrity tag; multiplayer encryption |
| miniupnpc, libnatpmp (optional) | `:host --direct` opens the port on the router; without them the build prints a note and hosting works on LAN / VPN or with a manually forwarded port |

```sh
make            # builds ./toh
./toh           # run from this directory, or anywhere: resources/ is found next to the binary
```

Sanitizer build: `make clean && make CFLAGS="-O1 -g -fsanitize=address,undefined"`.

Tests: `make test` (save format, game core and network message and relay unit tests), `make fuzz` (libFuzzer on the save file
and network message decoders, needs clang; `FUZZ_TIME=600` fuzzes each longer) and `make e2e` (saves and loads in a real
window, then runs a host and a client on localhost; needs a display and ImageMagick).
Set `TOH_VK_VALIDATION=1` to enable the Khronos validation layer when it is installed.

Your settings (the theme chosen with `:t` and the UI size from Ctrl + / Ctrl -) are kept in `settings.txt` next to the program, a plain text file
of `key = value` lines that git ignores. `TOH_SETTINGS=<file>` uses another file, `TOH_SETTINGS=` none (the e2e tests do that).

## Controls

Full guides: [gmcontrols.md](gmcontrols.md) (every control and command) and
[playercontrols.md](playercontrols.md) (the basics for players). The mouse and keyboard controls are the same as
in the Java version.

| Input | Action |
|---|---|
| Left click (or Enter in keyboard mode) | select tile / entity |
| Right click (or Right-Shift + Enter) | tool action (drag start, measure point, place attack) |
| Right click without moving (Drag tool) | tile menu: a ring of entries around the tile; Esc / click outside closes |
| Right click your character (Drag tool) | action menu: Minor / Info / Mental / Major; during initiative its move actions (Walk, Run, Crawl, Step, Sprint) move it on its turn, and Recover (1d4) / Full Recover restore its stamina |
| Shift + left click | move the selected character inside its (green) range (players: outside initiative) |
| Hold Left Shift | preview footprint and path of the selected character |
| Ctrl + click / drag | multi-select |
| Mouse wheel | zoom (scrolls the log when over the console) |
| Shift + wheel | change the cone angle |
| WASD | pan (Shift = fast, Ctrl = medium, Alt = paused) |
| Arrow keys | move the keyboard cursor; Right Shift + Up/Down zooms |
| Ctrl + Enter / Alt + Enter | toggle console (hidden at start) / toggle log |
| Up / Down / Tab (console) | history / auto-complete |
| `:` (Shift + `.` on German layouts) | open the console with `:` typed |
| Ctrl + C / V / A | copy / paste / select all entities |
| Ctrl + `+` / `-` | console, toolbox, HUD and radial menu size (remembered in `settings.txt`) |
| 1 … 9, 0 | hotbar slots (console closed) |
| Alt + X / S / A / C / V | drag / measure / area / cone / line tool |

Console commands: `:q` quit, `:b [file.png]` background, `:d` debug, `:c` character
(`delete`, `size n`, `maxhealth n`, `maxstamina n`, `stamina n`, `spend n`, `recover n`, `speed n`, `initiative n`, `icon`, `owner <player|none>`, `name <name>`, `party <name>` (GM),
`hp n`, `damage n`, `heal n`; the HUD at the bottom shows the selected (your own, or a party member's) character's HP and stamina),
`:w [delete]` wall, `:e delete`, `:r 2d6+3` roll, `:g` grid orientation, `:gm` gamemaster,
`:i [add|remove|clear|show|next]` initiative, `:tm [on|off]` test mode (GM: move and recover actions without turn, action or stamina requirements, for the whole table), `:cl` clear, `:t [name]` theme (remembered in `settings.txt`), `:h [command]` help, `:hud [on|off]` HUD on/off,
`:save [file]` / `:load[!] [file]` save files (see below), `:q!` quit without saving,
`:host`, `:join`, `:leave`, `:unhost`, `:players`, `:kick`, `:ban`, `:unban`, `:claim`, `:party` multiplayer (see below).

## Playing together

The GM hosts the table on their own computer. By default the game connects through a relay (`toh-relay` on a
home server, see `deploy/relay/README.md`), so the GM needs no router setup; the relay only passes encrypted bytes
along and cannot read the game.

1. GM: `:host [name]` (name defaults to `GM`). The game connects to the relay and copies a join code (`toh1-…`)
   to the clipboard. `:host --direct [name]` skips the relay: the game listens on TCP port 41234
   (`./toh --port N` for another), tries to open that port on the router (UPnP, then NAT-PMP) and puts this
   computer's addresses into the join code. The log says whether the router opened the port. If it didn't,
   forward the port by hand or use a VPN such as Tailscale; a router behind carrier-grade NAT (100.64.x.x) always
   needs a VPN.
2. Players: `:join <code> <name>`. Their table is replaced by the GM's; camera, zoom and theme stay their own.
3. The GM gives characters to players with `:c owner <name>`, or a player selects an unowned character and asks
   with `:claim` (the GM answers y/n in the console). A player can also create a character of their own: `:c [stats]`
   on an empty tile and a picture; the GM sees the stats and a faint preview and answers y/n.

Players can move their own characters (Shift + click inside the green range; while initiative runs only on their
turn), change the HP, stamina and name of their own characters, see the HP of characters in a party with one of
theirs (the GM sets parties with `:c party <name>`; `:party` lists them), end their turn with `:i next`, roll dice (`:r`, rolled by the host and shown to everyone) and save a copy of
the table. Everything else is the GM's. `:players` lists who is at the table, `:kick <name>` removes someone,
`:leave` / `:unhost` end it; players keep the last table as an unsaved offline table.
A kicked player can rejoin with the same code; `:ban <name>` removes them and turns away both that name and their
address (IPv4 address or IPv6 /64) until `:unban <name>` or the end of the session (`:ban` lists the bans). Players
behind the same router share its address, so at school a ban can also keep out classmates: `:unban` them if so.
During a session every owned character has a ring in its owner's color. A player can have at most 3 requests
(claims and new characters) waiting for the GM and roll at most 10 times in 10 seconds.

Everyone sees what the others are pointing at, in their color (the GM is gold, players get blue, red, green,
...; `:players` lists the colors): selected tiles and characters with a name label, the movement preview while
Shift is held, and measurements and line / area / cone attacks. Your own selection and tools look as before.

The connection is encrypted (libsodium) with a key from the join code; each `:host` makes a new code, so old codes
stop working. The relay address is built in (`make RELAY=host:port`); `TOH_RELAY=host:port` overrides it and
`TOH_RELAY=` turns it off. `TOH_NO_PORTMAP=1` skips the router with `--direct` (LAN-only use, tests).

## Saving

`:save file` writes the table to `file.tuhh` (the extension is added if missing) and remembers it as the
current file; a later `:save` without a name writes there again, or opens a save dialog if there is no current file.
`:load file` loads a save, `:load` opens a dialog. Paths may contain spaces, and `~/` means the home directory.

Like nvim, `:q` and `:load` refuse to throw away unsaved changes: `No write since last change (add ! to override)`.
Use `:q!` / `:load!` to discard them. Camera, zoom and theme are saved but don't count as changes. The window's
close button always quits.

A save contains the background, characters (image, stats, current HP and stamina, name, owner and party), walls, the initiative order, waypoints, grid
orientation, camera and theme (loading shows it, but the theme remembered for the next start stays the one from `:t`);
not the console, selections, clipboard or measurements. `:c owner <player>` gives the
selected characters to a player; pasted copies have no owner (but keep their party).

The format (`src/savefmt.h`, version 8; files from older versions still load) is binary and compact: the game state is packed with varints and zstd-compressed, and
every image is stored once as WebP (lossless for characters and walls, quality 90 for the background; images over
16383 px are scaled down). Saves are written to `file.tmp` first and then renamed, and a load only replaces the
table once the whole file has been checked.

Every file ends with a keyed BLAKE2b tag, so damaged or edited files are rejected ("Save file is damaged or was
modified"). The key is part of the program, so the tag is no proof of who made a file; that needs a signature from
a server, for which the format reserves a flag. Because of that, the loader treats every file as untrusted: all
sizes, counts and references are checked against fixed limits before use.

## Source layout

| File | Java origin |
|---|---|
| `src/main.c` | `main/Main` – startup, main loop, 16 ms game tick |
| `src/graphics.c` | `main/GraphicsHandler` – state, zoom/pan, dialogs, scene rendering |
| `src/game.c` | `main/GameHandler` – camera tick, movement, initiative |
| `src/ops.c` | – (new) every change to the table as a checked operation; GM / player rules |
| `src/netmsg.c` | – (new) multiplayer wire format: messages, ops and join codes |
| `src/net.c` | – (new) network thread: sockets, handshake, encryption, send queues |
| `src/session.c` | – (new) `:host` / `:join` and the other session commands; host and client message handling |
| `src/presence.c` | – (new) what each person points at: collected from the local state, drawn for the others |
| `src/portmap.c` | – (new) router port mapping (UPnP / NAT-PMP) and the addresses in the join code |
| `src/wire.h` | – (new) varint / string reader and writer shared by the save format and `netmsg.c` |
| `src/input.c` | `main/IOHandler` – mouse and keyboard |
| `src/console.c` | `main/Consol`, `ConsolPanel`, `CommandCompleter` |
| `src/tools.c` | `main/Toolbox`, `tools/*` (measure, line, area, cone) |
| `src/entity.c` | `entities/*`, `fx/Marker` |
| `src/hex.c`, `src/astar.c` | `fx/Hexagon`, `calc/Calc`, `calc/AStar`, `calc/TwoKeyMap` |
| `src/dice.c`, `src/image.c` | `calc/Dice`, image cropping from `calc/Calc` |
| `src/vk_renderer.c` | Vulkan backend: swapchain, 4× MSAA, one textured-triangle pipeline |
| `src/draw.c`, `src/font.c` | Graphics2D-style primitives (miter-joined strokes, arcs, text), FreeType glyph atlas |

The Swing panel stack is drawn in the same order: background, entities, grid at 50% alpha,
effects/markers, then toolbox and console. Shapes and text share the glyph atlas texture, so a
typical frame is only a few draw calls.

### Test hook

`--exec` runs console commands or scripted input actions one per frame. `@shot` saves a
screenshot, which makes headless-ish regression checks possible:

```sh
./toh --auto-file hero.png --exec "@move 300 250" --exec ":c speed 3" \
             --exec "@click 300 250" --exec "@shot out.png" --exec "@quit"
```

See the comment at the top of `src/main.c` for the full action list (including `@waitnet`, `@waitfile`,
`@waitbars` (waits until the HUD bars stop moving), `@hudrect` / `@clickhud` (HUD part positions), `@radialpoint` / `@clickradial` (radial menu parts), `@rdown` / `@rdrag` / `@rup` (a right drag) and `@touch`, which `tests/e2e_net.sh` uses to synchronize a host and a client).

## Differences from the Java version

The port follows the Java code closely, including its quirks (for example `:c delete` and
`:e delete` remove *all* characters or walls, Huge characters are drawn offset to the right,
and finished measurements use the straight-line distance between grid coordinates).
The changes below are deliberate:

- **File dialogs are non-blocking** (SDL portal/zenity dialog). `:b` also accepts a file path.
- **Confirmation prompts** (spawning on several tiles) no longer use a waiting thread; the answer
  typed into the console is handled directly. The console opens if it was hidden.
- **The close button quits.** The Java window ignored it, so `:q` was the only way out.
- **Help text is complete.** The Java `help` command had lost most of its text.
- **Dice accept `+K` / `-K`** as the README already promised. Bad syntax prints a message
  instead of throwing.
- **The area attack label** uses the same pixel-to-tile conversion as the cone. The Java formula had
  an operator-precedence bug and showed a pixel count.
- **Crashes fixed:** using tools or measuring with no tile under the cursor, `:c delete` with
  nothing selected (Java then also spawned a character), Tab completion after commands
  without arguments, and selecting a wall while in gamemaster mode.
- **Known issues fixed:**
  - WASD key releases are always seen and modifiers reset when focus is lost, so panning no longer "freezes".
  - The total-length marker is no longer added to the marker list on every repaint (a leak in Java).
  - Tile selections no longer get lost after they scroll off screen.
- **Keyboard cursor:** moving it past the window edge scrolls the view.
- **`:c icon`** applies the new image cropped to a circle, as with newly created characters. It no
  longer swallows the arguments that follow it.
- **Sizes** follow Tales of Honor (-2 Wee to 4 Gigantic, Normal = 1) instead of the Java version's D&D sizes, and
  characters may share a tile while their bulk fits (5 per tile). Selection follows characters, not tiles.
- **Deleting entities** also removes them from initiative and clears stale selection highlights.
- **Initiative order:** the first `:i next` starts with the highest roller (Java skipped them), removing
  the current character passes the turn to the next one in order (Java skipped that one too), and
  adding characters mid-round keeps the turn with the same character.
- **`:` shortcut** only fires while the console is hidden, so typing `:` inside a command no
  longer wipes the line.
- **Tool icons:** `move.png`, `length.png` and `line.png` in the Java project are actually JPEG files
  with a `.png` extension (Java's ImageIO accepted them). The copies in `resources/` were converted to real PNGs.
  User-selected images must be real PNG files.
