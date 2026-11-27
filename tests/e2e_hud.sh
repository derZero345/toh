#!/usr/bin/env bash
# End-to-end check of the HUD: the selected character's tab and HP bar, a click on the HUD not reaching the map,
# and the tab's close button clearing the selection. Then the GM creates characters through the bar: the steps and
# placing, the "+" box, Cancel, and the tile menu's Create character. Opens a window (needs Wayland or X11) and uses
# ImageMagick.
# Usage: tests/e2e_hud.sh   (or: make e2e)
set -euo pipefail
cd "$(dirname "$0")/.."
export TOH_SETTINGS= # no settings file: the user's theme must not change the pixels, and :t must not change it

out=${KEEP:-$(mktemp -d)}
[ -n "${KEEP:-}" ] || trap 'rm -rf "$out"' EXIT
mkdir -p "$out"
icon=resources/images/toolIcons/area.png
fail() { echo "FAIL: $*" >&2; exit 1; }

# mean colour of the 4x4 block around pixel X Y: "r g b" from 0 to 1
rgb() { magick "$1" -crop 4x4+$(($2 - 2))+$(($3 - 2)) +repage -format '%[fx:mean.r] %[fx:mean.g] %[fx:mean.b]' info:; }
red() { awk -v s="$(rgb "$@")" 'BEGIN { split(s, c, " "); exit !(c[1] > 0.6 && c[2] < 0.3 && c[3] < 0.3) }'; }
# the HUD's dark background (empty map tiles are light grey)
dark() { awk -v s="$(rgb "$@")" 'BEGIN { split(s, c, " "); exit !(c[1] < 0.3 && c[2] < 0.3 && c[3] < 0.3) }'; }
# is the tile at pixel X Y occupied? empty tiles are light grey (about 0.9), the test icon mostly black
occupied() {
    local v
    v=$(magick "$1" -crop 16x16+$(($2 - 8))+$(($3 - 8)) +repage -colorspace gray -format '%[fx:mean]' info:)
    awk -v v="$v" 'BEGIN { exit !(v < 0.8) }'
}

timeout 60 ./toh --size 600x500 --auto-file "$icon" \
    --exec "@click 300 120" --exec ":c h 10" --exec "@wait 5" --exec "@click 300 120" \
    --exec "@wait 5" --exec "@waitbars" \
    --exec "@hudrect tab $out/tab" --exec "@hudrect hp $out/hp" --exec "@shot $out/selected.png" \
    --exec "@clickhud pill.left" --exec "@wait 5" --exec "@shot $out/hud-click.png" \
    --exec "@clickhud tab.close" --exec "@wait 5" --exec "@shot $out/closed.png" \
    --exec "@quit" > "$out/toh.log" 2>&1 || fail "toh exited with status $? (see $out/toh.log)"

[ -s "$out/tab" ] || fail "no target tab for the selected character (see $out/toh.log)"
read -r tx ty tw th < "$out/tab"
read -r hx hy hw hh < "$out/hp"
fx=$((hx + hw / 2))   # the HP frame is the right half of the bar
tab="$((tx + 5)) $((ty + th / 2))"
dark "$out/selected.png" $tab || fail "the target tab is not drawn"
red "$out/selected.png" $((fx + 10)) $((hy + hh / 2)) || fail "the HP bar is not red at 10/10"
! red "$out/selected.png" $((fx - 16)) $((hy + hh / 2)) || fail "the full HP bar reaches left of its frame"
dark "$out/hud-click.png" $tab || fail "a click on the HUD reached the map (the selection is gone)"
! dark "$out/closed.png" $tab || fail "the close button did not clear the selection"

# Creating characters through the bar (GM, empty board): the steps, placing, "+", Cancel and the tile menu
P="420 172"; R="540 312"   # tile centres: where Mao is placed; the tile menu's tile for Orc
timeout 60 ./toh --size 900x650 --auto-file "$icon" \
    --exec "@wait 5" --exec "@hudrect cc.start $out/start" --exec "@clickhud cc.start" --exec "@wait 3" \
    --exec "@type Mao" --exec "@wait 2" --exec "@clickhud cc.next" --exec "@wait 3" \
    --exec "@clickhud cc.next" --exec "@wait 3" \
    --exec "@clickhud cc.str.plus" --exec "@wait 2" --exec "@clickhud cc.str.plus" --exec "@wait 2" \
    --exec "@clickhud cc.str.plus" --exec "@wait 2" --exec "@clickhud cc.next" --exec "@wait 3" \
    --exec "@clickhud cc.picture" --exec "@wait 3" --exec "@clickhud cc.next" --exec "@wait 3" \
    --exec "@clickhud cc.next" --exec "@wait 3" --exec "@move $P" --exec "@wait 3" --exec "@shot $out/placing.png" \
    --exec "@click $P" --exec "@wait 5" --exec "@shot $out/placed.png" \
    --exec "@hudrect plus $out/plus" --exec "@click $P" --exec ":c hp 0" \
    --exec "@clickhud plus" --exec "@wait 3" --exec "@type Gone" --exec "@clickhud cc.cancel" --exec "@wait 3" \
    --exec "@rclick $R" --exec "@wait 20" --exec "@clickradial ring.7" --exec "@wait 5" \
    --exec "@type Orc" --exec "@wait 2" --exec "@clickhud cc.next" --exec "@wait 3" --exec "@clickhud cc.next" \
    --exec "@wait 3" --exec "@clickhud cc.next" --exec "@wait 3" --exec "@clickhud cc.picture" --exec "@wait 3" \
    --exec "@clickhud cc.next" --exec "@wait 3" --exec "@clickhud cc.next" --exec "@wait 5" \
    --exec "@shot $out/preset.png" --exec "@logfile $out/create.log" \
    --exec "@quit" > "$out/toh-create.log" 2>&1 || fail "toh exited with status $? (see $out/toh-create.log)"

[ -s "$out/start" ] || fail "no Create character button on an empty board (see $out/toh-create.log)"
occupied "$out/placing.png" $P || fail "the picture does not follow the mouse while placing"
occupied "$out/placed.png" $P || fail "the character was not placed"
grep -q "Mao: 21/21 -> 0/21" "$out/create.log" || fail "Mao does not have STR 3's max HP 21 (see $out/create.log)"
[ -s "$out/plus" ] || fail "the GM has no + box with a character on the board"
grep -q "Character creation cancelled" "$out/create.log" || fail "Cancel did not end the creation"
! grep -q "Gone" "$out/create.log" || fail "the cancelled character exists"
occupied "$out/preset.png" $R || fail "the tile menu's Create character did not create on its tile"
! grep -q "place Orc" "$out/create.log" || fail "the tile menu's character was placed by hand, not created on its tile"

echo "e2e HUD: OK"
