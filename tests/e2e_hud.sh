#!/usr/bin/env bash
# End-to-end check of the HUD: the selected character's tab and HP bar, a click on the HUD not reaching the map,
# and the tab's close button clearing the selection. Opens a window (needs Wayland or X11) and uses ImageMagick.
# Usage: tests/e2e_hud.sh   (or: make e2e)
set -euo pipefail
cd "$(dirname "$0")/.."

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

echo "e2e HUD: OK"
