#!/usr/bin/env bash
# End-to-end check of move actions: during initiative a right click on your character opens its action menu (four
# entries), Major action -> Walk shows the action's range, a click on a tile in it moves the character, and its
# action menu then opens on the new tile while the old tile gets the plain tile menu. Then test mode (:tm): Walk
# without initiative, and the TEST MODE label. Last, Full Recover from the action menu fills the stamina bar. Opens a window (needs Wayland
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

# test mode (:tm): without initiative, Walk works and the TEST MODE label shows at the top of the map
timeout 60 ./toh --size 600x500 --auto-file "$icon" \
    --exec "@click 300 220" --exec ":c speed 4 maxstamina 8" --exec "@wait 5" --exec "@click 500 60" \
    --exec "@wait 5" --exec "@shot $out/tm-off.png" --exec ":tm" --exec "@wait 5" --exec "@shot $out/tm-on.png" \
    --exec "@rclick 300 220" --exec "@wait 20" --exec "@clickradial ring.4" --exec "@wait 20" \
    --exec "@clickradial ring.1" --exec "@wait 20" \
    --exec "@click 300 150" --exec "@wait 5" --exec "@click 500 60" --exec "@wait 5" --exec "@shot $out/tm-moved.png" \
    --exec ":tm off" --exec "@wait 5" --exec "@shot $out/tm-back.png" \
    --exec "@quit" > "$out/toh-tm.log" 2>&1 || fail "toh exited with status $? (see $out/toh-tm.log)"

# the label is centred over the map (right of the 32 px toolbox); a tiling window manager may resize the window
lx=$(( (32 + $(magick identify -format '%w' "$out/tm-on.png")) / 2 ))
differ "$out/tm-off.png" "$out/tm-on.png" "$lx" 30 || fail ":tm shows no TEST MODE label"
! differ "$out/tm-off.png" "$out/tm-back.png" "$lx" 30 || fail ":tm off leaves the TEST MODE label"
differ "$out/tm-on.png" "$out/tm-moved.png" 300 220 || fail "in test mode Walk did not move the character outside initiative"
differ "$out/tm-on.png" "$out/tm-moved.png" 300 150 || fail "in test mode the character did not appear on the new tile"

# Full Recover (Major action -> 4th entry): the stamina bar is full again. The bar's frame is the right half of the
# element (@hudrect); a full bar is green at 90 % of the frame, an almost empty one isn't.
timeout 60 ./toh --size 600x500 --auto-file "$icon" \
    --exec "@click 300 220" --exec ":c speed 4 maxstamina 8" --exec "@wait 5" --exec "@click 300 220" \
    --exec ":c stamina 1" --exec ":i" --exec ":i next" --exec "@wait 5" --exec "@waitbars" \
    --exec "@hudrect stamina $out/st" --exec "@shot $out/rec-before.png" \
    --exec "@rclick 300 220" --exec "@wait 20" --exec "@clickradial ring.4" --exec "@wait 20" \
    --exec "@clickradial ring.4" --exec "@wait 5" --exec "@waitbars" --exec "@shot $out/rec-after.png" \
    --exec "@quit" > "$out/toh-rec.log" 2>&1 || fail "toh exited with status $? (see $out/toh-rec.log)"
green() {
    local rgb
    rgb=$(magick "$1" -crop 4x4+$(($2 - 2))+$(($3 - 2)) +repage -format '%[fx:mean.r] %[fx:mean.g] %[fx:mean.b]' info:)
    awk -v s="$rgb" 'BEGIN { split(s, c, " "); exit !(c[2] > 0.5 && c[1] < 0.35 && c[3] < 0.4) }'
}
read -r sx sy sw sh < "$out/st"
px=$((sx + sw / 2 + sw / 2 * 90 / 100)); py=$((sy + sh / 2))
! green "$out/rec-before.png" $px $py || fail "the stamina bar was full before Full Recover"
green "$out/rec-after.png" $px $py || fail "Full Recover did not fill the stamina bar (see $out/toh-rec.log)"

echo "e2e move actions: OK"
