#!/usr/bin/env bash
# End-to-end check of move actions: during initiative a right click on your character opens its action menu (four
# entries), Major action -> Walk shows the action's range, a click on a tile in it moves the character, and its
# action menu then opens on the new tile while the old tile gets the plain tile menu. Opens a window (needs Wayland
# or X11) and uses ImageMagick.
# Usage: tests/e2e_moves.sh   (or: make e2e)
set -euo pipefail
cd "$(dirname "$0")/.."
export TOH_SETTINGS= # no settings file: the user's theme must not change the pixels

out=${KEEP:-$(mktemp -d)}
[ -n "${KEEP:-}" ] || trap 'rm -rf "$out"' EXIT
mkdir -p "$out"
icon=resources/images/toolIcons/area.png
fail() { echo "FAIL: $*" >&2; exit 1; }
# Normalized RMSE between the 60x60 blocks around X Y of two screenshots (0 = identical)
differ() {
    awk -v v="$(magick "$1" "$2" -crop 60x60+$(($3 - 30))+$(($4 - 30)) +repage -metric RMSE -compare \
        -format '%[distortion]' info: 2>/dev/null || true)" 'BEGIN { exit !(v > 0.02) }'
}

# the character's tile: at 300 220; the tile above it is one hex height (about 70 px) up
timeout 60 ./toh --size 600x500 --auto-file "$icon" \
    --exec "@click 300 220" --exec ":c speed 4 maxstamina 8" --exec "@wait 5" --exec "@click 300 220" \
    --exec ":i" --exec ":i next" --exec "@click 500 60" --exec "@wait 5" --exec "@shot $out/before.png" \
    --exec "@rclick 300 220" --exec "@wait 20" \
    --exec "@radialpoint center $out/c" --exec "@radialpoint ring.4 $out/a4" --exec "@radialpoint ring.5 $out/a5" \
    --exec "@clickradial ring.4" --exec "@wait 20" --exec "@clickradial ring.1" --exec "@wait 20" \
    --exec "@move 300 150" --exec "@wait 5" --exec "@shot $out/choosing.png" \
    --exec "@click 300 150" --exec "@wait 5" --exec "@click 500 60" --exec "@wait 5" --exec "@shot $out/moved.png" \
    --exec "@rclick 300 150" --exec "@wait 20" \
    --exec "@radialpoint ring.4 $out/b4" --exec "@radialpoint ring.5 $out/b5" \
    --exec "@key escape" --exec "@wait 20" \
    --exec "@rclick 300 220" --exec "@wait 20" --exec "@radialpoint ring.6 $out/t6" \
    --exec "@key escape" --exec "@wait 20" \
    --exec "@quit" > "$out/toh.log" 2>&1 || fail "toh exited with status $? (see $out/toh.log)"

[ -s "$out/a4" ] && [ ! -s "$out/a5" ] || fail "a right click on the character did not open its action menu (see $out/toh.log)"
differ "$out/before.png" "$out/choosing.png" 300 185 || fail "choosing a tile for Walk shows no range or path"
differ "$out/before.png" "$out/moved.png" 300 220 || fail "the character is still on its old tile"
differ "$out/before.png" "$out/moved.png" 300 150 || fail "the character did not appear on the new tile"
[ -s "$out/b4" ] && [ ! -s "$out/b5" ] || fail "the character's action menu did not open on its new tile"
[ -s "$out/t6" ] || fail "the old tile did not get the tile menu (the character did not move)"

echo "e2e move actions: OK"
