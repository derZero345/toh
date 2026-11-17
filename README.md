# pathfinder (C / Vulkan)

A C11 rewrite of the Java/Swing `pathfinder` hex-grid tabletop tool. It renders with Vulkan
and uses SDL3 for the window, input and file dialogs.

## Building

Dependencies:

| Library | Used for |
|---|---|
| SDL3 ≥ 3.4 | window, input, file dialogs, PNG loading (`SDL_LoadPNG`) |
| Vulkan loader + headers | rendering |
| FreeType 2 + fontconfig | text (looks up "Arial", falls back to the system sans font) |
| `glslc` (shaderc) | compiles `shaders/*.vert/frag` to SPIR-V at build time |
| libwebp, libzstd, libsodium | save files: image compression, state compression, integrity tag |

```sh
make            # builds ./pathfinder
./pathfinder    # run from this directory, or anywhere: resources/ is found next to the binary
```

Sanitizer build: `make clean && make CFLAGS="-O1 -g -fsanitize=address,undefined"`.

Tests: `make test` (save format unit tests), `make fuzz` (libFuzzer on the save file decoder, needs clang;
`FUZZ_TIME=600` fuzzes longer) and `make e2e` (saves and loads in a real window, needs a display and ImageMagick).
Set `PATHFINDER_VK_VALIDATION=1` to enable the Khronos validation layer when it is installed.

## Controls

These are the same as in the Java version.

| Input | Action |
|---|---|
| Left click (or Enter in keyboard mode) | select tile / entity |
| Right click (or Right-Shift + Enter) | tool action (drag start, measure point, place attack) |
| Shift + left click | move the selected character inside its (green) range |
| Hold Left Shift | preview footprint and path of the selected character |
| Ctrl + click / drag | multi-select |
| Mouse wheel | zoom (scrolls the log when over the console) |
| Shift + wheel | change the cone angle |
| WASD | pan (Shift = fast, Ctrl = medium, Alt = paused) |
| Arrow keys | move the keyboard cursor; Right Shift + Up/Down zooms |
| Ctrl + Enter / Alt + Enter | toggle console / toggle log |
| Up / Down / Tab (console) | history / auto-complete |
| `:` (Shift + `.` on German layouts) | open the console with `:` typed |
| Ctrl + C / V / A | copy / paste / select all entities |
| Ctrl + `+` / `-` | console and toolbox size |
| Alt + X / S / A / C / V | drag / measure / area / cone / line tool |

Console commands: `:q` quit, `:b [file.png]` background, `:d` debug, `:c` character
(`delete`, `size n`, `maxhealth n`, `armorclass n`, `speed n`, `initiative n`, `icon`),
`:w [delete]` wall, `:e delete`, `:r 2d6+3` roll, `:g` grid orientation, `:gm` gamemaster,
`:i [add|remove|clear|show|next]` initiative, `:cl` clear, `:t [name]` theme, `:h [command]` help,
`:save [file]` / `:load[!] [file]` save files (see below), `:q!` quit without saving.

## Saving

`:save file` writes the table to `file.pfsave` (the extension is added if missing) and remembers it as the
current file; a later `:save` without a name writes there again, or opens a save dialog if there is no current file.
`:load file` loads a save, `:load` opens a dialog. Paths may contain spaces, and `~/` means the home directory.

Like nvim, `:q` and `:load` refuse to throw away unsaved changes: `No write since last change (add ! to override)`.
Use `:q!` / `:load!` to discard them. Camera, zoom and theme are saved but don't count as changes. The window's
close button always quits.

A save contains the background, characters (image and stats), walls, the initiative order, waypoints, grid
orientation, camera and theme; not the console, selections, clipboard or measurements.

The format (`src/savefmt.h`) is binary and compact: the game state is packed with varints and zstd-compressed, and
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
./pathfinder --auto-file hero.png --exec "@move 300 250" --exec ":c speed 3" \
             --exec "@click 300 250" --exec "@shot out.png" --exec "@quit"
```

See the comment at the top of `src/main.c` for the full action list.

## Differences from the Java version

The port follows the Java code closely, including its quirks (for example `:c delete` and
`:e delete` remove *all* characters or walls, Large characters are drawn offset to the right,
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
- **Deleting entities** also removes them from initiative and clears stale selection highlights.
- **`:` shortcut** only fires while the console is hidden, so typing `:` inside a command no
  longer wipes the line.
- **Tool icons:** `move.png`, `length.png` and `line.png` in the Java project are actually JPEG files
  with a `.png` extension (Java's ImageIO accepted them). The copies in `resources/` were converted to real PNGs.
  User-selected images must be real PNG files.
