# Save files (`.pfsave`) — design

Date: 2026-11-18
Status: approved, ready for implementation planning

## Goal

Let the user save the current table to a file of their choice and load it again. Save files
will later be shared online, so they must be:

- **compact** — as small as practical,
- **not plain text** — binary, not casually editable,
- **tamper-evident** — an edited or corrupted file is rejected,
- **safe to load from strangers** — a malicious file must not crash or exploit the game.

Building the online sharing service itself is out of scope. The format only has to be ready for it.

## 1. User-facing behaviour

### Commands

| Command | Behaviour |
|---|---|
| `:save <path>` | Saves to `<path>`, which becomes the *current file*. |
| `:save` | Saves to the current file. If there is none yet, the save dialog opens (filter `*.pfsave`). |
| `:load <path>` | Loads `<path>`, which becomes the current file. |
| `:load` | Opens the load dialog (filter `*.pfsave`). |
| `:load! [path]` | Same as `:load`, but skips the unsaved-changes check. |
| `:q` | Quits, unless there are unsaved changes (see below). |
| `:q!` | Quits unconditionally. |

- `.pfsave` is appended to a path that does not already end in it (on save only).
- `:w` (wall) and `:e` (entity) are unchanged.
- `save` and `load` get tab completion (as commands; no argument suggestions) and `:h save` / `:h load` help text.
- The current file starts as "none" at startup.
- The window close button still quits without asking.
- `--auto-file <file>` makes the save and load dialogs return that file, like the image dialogs, so scripted tests work.
- The dialogs reuse the existing non-blocking dialog plumbing (`PendingAction`, `dialogEvent`). There are two new
  `ActionKind` values: `ACTION_SAVE` and `ACTION_LOAD`. The save dialog uses `SDL_ShowSaveFileDialog`.

### Unsaved-changes guard (like nvim)

If the table differs from what was last saved or loaded, `:q` and `:load` refuse and log:

```
No write since last change (add ! to override)
```

**What counts as a change:** this is decided by a *content fingerprint*, not by a dirty flag (a flag would need a
call at every mutation site, and one missed site would be a silent bug):

- The fingerprint is a BLAKE2b-256 hash of the encoded **state section** (see §3), *excluding* camera
  (`gridOffsetX/Y`, `hexSize`) and theme. Those are saved, but like nvim's cursor position they don't make the
  table "modified".
- Images are represented by their texture's unique serial id (assigned at creation, never reused), not by pixel
  content. Computing the fingerprint therefore never encodes images, and replacing an image with a new one is
  detected even if it lands at the same table index or memory address.
- The fingerprint is stored after every successful save or load. At startup it is set to the fingerprint of the
  empty table, so a fresh session can be quit without a warning.
- It is computed only when `:q` or `:load` is run (cheap: no compression, no image encoding).

### Feedback messages (console log)

- `Saved "<path>" (<size>)` / `Loaded "<path>"`
- `Not a pathfinder save file`
- `Save file was made by a newer version of pathfinder`
- `Save file is damaged or was modified`
- `Save file is too large`
- `Could not write "<path>": <reason>` / `Could not read "<path>": <reason>`

### Atomicity

- **Save:** the game writes `<path>.tmp`, flushes it, then `rename()`s it over `<path>`. If writing fails, the
  temporary file is removed and any existing save is untouched.
- **Load:** the whole file is verified and decoded into a staging `SaveData` and every image is decoded to
  pixels before any game state is touched. Only then is the current table replaced. If anything fails, the
  current game is unchanged.

## 2. Saved contents

**Saved:**

- Background image (or none), `backgroundRows`, `backgroundCols`
- Grid orientation (`isFlat`)
- Camera: `gridOffsetX`, `gridOffsetY`, `hexSize` (zoom); `thickness` is recomputed from `hexSize`
- Theme (by name)
- Entities, in list order:
  - type (character / wall)
  - image (index into the image table)
  - tile (`row`, `col`)
  - for characters only: `size`, `maxHealth`, `health`, `attack`, `ac`, `speed`, `initiative`
- Initiative list:
  - entries in order: entity index plus the `double` roll value
  - `currentTurnIndex`
  - whether a turn has started (`currentTurn != NULL`; it stays `NULL` until the first `:i next`)
  - whether the initiative markers are visible
- Waypoints: name and tile

**Not saved:** console log and history, selections, clipboard, active tool, measurements, line/area/cone attacks,
markers other than initiative markers, the debug and gamemaster flags, and the keyboard cursor.

**Restored on load:**

- Initiative markers are rebuilt through a new `game.c` function, so `currentTurn` and the marker colours match the
  saved state.
- Debug markers are recreated by the normal entity constructors.
- Selections, path, range and preview tiles, and measurements are cleared.

## 3. File format, version 1

All integers are little-endian.

```
offset  size  field
0       4     magic "PFSV"
4       2     u16 version = 1
6       2     u16 flags   (bit 0 = signature block present; must be 0 in v1, other bits must be 0)
8       4     u32 state_len           length of the zstd frame that follows
12      n     zstd frame              compressed state section
...     4     u32 image_count
              per image:
                1   u8  kind          0 = lossless (icon / wall), 1 = lossy (background)
                2   u16 width
                2   u16 height
                4   u32 data_len
                n   WebP bytes
...     (reserved: signature block, only when flags bit 0 is set — not written or accepted in v1)
end-32  32    keyed BLAKE2b-256 tag over every preceding byte
```

- **Images** sit outside the zstd frame because WebP is already compressed.
- **One copy per image:** images are de-duplicated by texture, so 20 characters sharing an icon store it once.
- **Encoding:**
  - lossless icons and walls: WebP lossless, `exact` on, so transparent pixels keep their colour;
  - lossy background: WebP quality 90, method 6;
  - a background larger than 16383 px on either side is scaled down proportionally to fit (WebP's limit), with a console note.
- **Compression:** the zstd level is 19.

### State section (before compression)

Encoding conventions:

- `uv` is an unsigned LEB128 varint (at most 5 bytes, value ≤ `UINT32_MAX`).
- `sv` is a zig-zag-encoded signed varint (fits `int32_t`).
- `f64` is a raw IEEE-754 little-endian double.
- `str` is `uv` length followed by UTF-8 bytes.

```
world:
  u8  flags              bit0 isFlat, bit1 has background, bit2 initiative markers visible,
                         bit3 turn started; other bits 0
  uv  background image index   (only if bit1)
  uv  backgroundRows, uv backgroundCols
  sv  gridOffsetX, sv gridOffsetY
  uv  hexSize
  str theme name
entities:
  uv  count
  per entity:
    u8  type             0 character, 1 wall
    uv  image index
    sv  row, sv col
    (characters only) sv size, sv maxHealth, sv health, sv attack, sv ac, sv speed, sv initiative
initiative:
  uv  count
  per entry: uv entity index, f64 value
  uv  currentTurnIndex
waypoints:
  uv  count
  per waypoint: str name, sv row, sv col
```

The decoder must consume the state section exactly; trailing bytes are an error.

### Encoder validation

The encoder runs the same validation as the decoder before writing, so the game can never produce a file it would
reject itself. If validation fails (only possible in extreme cases, such as a waypoint created at coordinates beyond
±1 000 000), saving fails with "The table contains values that cannot be saved" and nothing is written.

### Integrity tag

- **What it is:** `crypto_generichash` (BLAKE2b, 32-byte output) keyed with a 32-byte constant compiled into the
  game. It is verified with `sodium_memcmp` (constant time) **before** anything else in the file is parsed or
  decompressed.
- **What it protects against:** accidental corruption, and edits made by anyone who has not extracted the key
  from the binary.
- **What it doesn't protect against:** someone who extracts the key from the binary and forges a valid tag. Real
  authenticity for shared files has to come from the future online service signing uploads with a private key.
  The reserved `flags` bit 0 and signature block (Ed25519) leave room for that without breaking v1 files.
- **The parser must therefore be safe on its own,** whether or not the tag is valid.

### Version handling

- The magic is checked first; a mismatch gives "Not a pathfinder save file".
- `version > 1` gives "made by a newer version". `version == 0` or unknown flag bits give "damaged or was modified".

## 4. Safe loading (untrusted input)

Order of checks:

1. File size ≤ 256 MiB and ≥ 12 + 32 bytes; read the whole file into memory.
2. Magic, version and flags.
3. Verify the tag.
4. Parse the remaining structure with bounds checks.

Hard limits, enforced before any allocation sized by file data:

| Item | Limit |
|---|---|
| file size | 256 MiB |
| decompressed state | 16 MiB (`ZSTD_getFrameContentSize` must be known and ≤ limit; decompression output buffer is exactly that size) |
| images | ≤ 4096 |
| image width/height | 1 … 16383, `w*h*4` computed in 64-bit |
| decoded pixels, all images together | 1 GiB of RGBA (stops tiny WebP files that decode to huge images) |
| entities | ≤ 100 000 |
| initiative entries | ≤ number of entities |
| waypoints | ≤ 10 000 |
| strings | ≤ 255 bytes, valid UTF-8, no NUL; waypoint names additionally < `WAYPOINT_NAME_MAX` |

Value validation:

- Image index < `image_count`.
- Entity index in initiative < entity count; no duplicate entities in initiative.
- `currentTurnIndex` < initiative count (or 0 when empty).
- Character stats accept any `int32` (the game itself accepts any number for them; unknown sizes draw as normal),
  except `speed` ≤ `MAX_SPEED`, which the game enforces too.
- Tile coordinates (entities, waypoints) within ±1 000 000, camera offsets within ±1 000 000 000, so hex
  arithmetic can't overflow.
- Initiative values must be finite doubles. "Turn started" requires at least one initiative entry.
- Waypoint names must not be empty.
- `hexSize` in 5…400 (zoom stops at about 15…200, but a single step can overshoot slightly); `backgroundRows/Cols` in 1…10 000.
- Unknown theme name → keep the current theme and log a note (not an error).
- Image `kind` ∈ {0, 1}.
- Every WebP image is decoded with `WebPGetInfo` first. Its dimensions must equal the declared ones, then it is
  decoded with `WebPDecodeRGBAInto` into a buffer of exactly `w*h*4`.
- File paths come only from the user or a dialog, never from inside the file, so there is no path traversal.

## 5. Code structure

### New files

- **`src/savefmt.h`, `src/savefmt.c`:** pure format code with no SDL, Vulkan or `App`. It depends only on libc,
  libzstd, libwebp and libsodium, so the future server can reuse it to validate uploads.
  - `SaveData`: plain structs and arrays mirroring §2. Images are `{kind, w, h, rgba pixels, optional cached webp bytes}`.
  - `savefmt_encode(const SaveData *, uint8_t **out, size_t *len, char *err, size_t errlen)`
  - `savefmt_decode(const uint8_t *buf, size_t len, SaveData *out, SaveError *err)`. `SaveError` is an enum that
    maps to the messages in §1.
  - `savefmt_fingerprint(const SaveData *, uint8_t out[32])`: the hash described in §1.
  - `savefmt_free(SaveData *)`
- **`src/save.c`** (declarations in `app.h`): the bridge to the game.
  - `save_gather(SaveData *)`: builds a `SaveData` from `gh`, de-duplicating textures.
  - `save_apply(SaveData *)`: creates textures, clears the current table, installs the loaded entities,
    initiative, waypoints, background, camera and theme.
  - Command handlers `save_command(path)` and `load_command(path, force)`, plus `save_quit(force)`.
  - Dialog result handling for `ACTION_SAVE` / `ACTION_LOAD`.
  - Atomic write.
  - The current-file path and the stored fingerprint.

### Changes to existing files

- **`vk_renderer.c/.h`:** textures can keep a CPU-side RGBA copy and a cached encoded blob.
  - `vkr_texture_create_kept(w, h, px)` takes ownership of the pixel buffer and keeps it.
  - `vkr_texture_pixels(t)` returns `NULL` for textures without a copy (fonts, tool icons).
  - `vkr_texture_set_blob` / `vkr_texture_blob` cache the encoded WebP bytes. Re-saving then doesn't re-encode, and
    a lossy background isn't re-compressed (no generation loss). Setting a blob frees the pixel copy, so a large
    background costs CPU memory only until its first save. Textures loaded from a save keep only the blob.
  - `vkr_texture_id(t)` returns a unique serial id (used by the fingerprint).
- **`graphics.c`:** `texture_from_image` uses the kept variant for user images (background, characters, walls,
  icons). It also handles `ACTION_SAVE` and `ACTION_LOAD` in the dialog flow; the save dialog is shown via
  `SDL_ShowSaveFileDialog`.
- **`game.c`:** `game_restore_initiative(entries, n, current_index, markers_visible)`.
- **`console.c`:**
  - `save`, `load` and `load!` commands;
  - `:q` / `quit` goes through the guard;
  - `:q!` is added;
  - help text and completion entries for the new commands.
- **`Makefile`:** `PKGS += libwebp libzstd libsodium`; `test` and `fuzz` targets.
- **`README.md`:** "Saving" section (commands, format summary, the tag's limits); dependency table updated.

## 6. Testing

- **`make test` → `tests/save_test.c`** (links `savefmt.c` only). It checks:
  - Round trip: encode, decode, then compare field by field. Images: lossless ones are pixel-exact; the lossy
    background has the right dimensions.
  - Every single-byte flip in an encoded file is rejected (`damaged or was modified`, or `not a pathfinder save`
    for the magic bytes).
  - Every truncation length is rejected.
  - Bad magic, `version = 2`, and non-zero flag bits give the right error.
  - Semantic validation: files re-tagged with the correct key but with out-of-range indices, oversized counts,
    non-UTF-8 names, a zstd frame with no content size, and mismatched WebP dimensions are all rejected. The test
    builds these with an internal "raw encode" helper.
  - The fingerprint ignores camera and theme changes but changes when an entity moves.
- **`make fuzz` → `tests/fuzz_savefmt.c`:** a libFuzzer target (clang, `-fsanitize=fuzzer,address,undefined`) on
  `savefmt_decode`. A build-time define disables the tag check, so the fuzzer reaches the parser.
- **End-to-end with `--exec`:**
  1. Build a scene (background, characters, walls, initiative, waypoint).
  2. `:save` to a file, then `@shot a.png`.
  3. Change the scene; check that `:q` is refused.
  4. `:load!` the file, then `@shot b.png`.
  5. Compare the screenshots. Some pixel tolerance is allowed only in the lossy background area.
- **Manual:** load files produced by the test generator under the ASan build (`make CFLAGS="-O1 -g -fsanitize=address,undefined"`).

## Out of scope

- The online sharing service, accounts, and server-side signing. The format only reserves space for these.
- Autosave, multiple save slots, a save browser UI, and a `:wq` equivalent.
- Saving console history, UI state or measurements.
