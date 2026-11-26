#!/usr/bin/env bash
# End-to-end check of the radial menu: a right click on a tile opens it, a submenu ring grows around the shrunk
# parent with the opener highlighted, Esc closes it, a right drag (panning) doesn't open it, a right click on
# another tile moves it there and a right click on it closes it. Opens a window
# (needs Wayland or X11) and uses ImageMagick.
# Usage: tests/e2e_radial.sh   (or: make e2e)
set -euo pipefail
cd "$(dirname "$0")/.."

out=${KEEP:-$(mktemp -d)}
[ -n "${KEEP:-}" ] || trap 'rm -rf "$out"' EXIT
mkdir -p "$out"
fail() { echo "FAIL: $*" >&2; exit 1; }

# mean colour of the 4x4 block around pixel X Y: "r g b" from 0 to 1
rgb() { magick "$1" -crop 4x4+$(($2 - 2))+$(($3 - 2)) +repage -format '%[fx:mean.r] %[fx:mean.g] %[fx:mean.b]' info:; }
# the point D pixels along the ring from part point X Y (around the centre CX CY): beside the part's icon
beside() { awk -v x="$1" -v y="$2" -v cx="$3" -v cy="$4" -v d="$5" 'BEGIN {
    dx = x - cx; dy = y - cy; l = sqrt(dx * dx + dy * dy); printf "%d %d", x - dy / l * d, y + dx / l * d }'; }
dark() { awk -v s="$(rgb "$@")" 'BEGIN { split(s, c, " "); exit !(c[1] < 0.3 && c[2] < 0.3 && c[3] < 0.3) }'; }
blue() { awk -v s="$(rgb "$@")" 'BEGIN { split(s, c, " "); exit !(c[3] > c[1] + 0.1 && c[3] < 0.7) }'; }

timeout 60 ./toh --size 600x500 \
    --exec "@rclick 300 220" --exec "@wait 20" \
    --exec "@radialpoint ring.1 $out/r1" --exec "@radialpoint ring.2 $out/r2" --exec "@radialpoint center $out/c" \
    --exec "@shot $out/open.png" \
    --exec "@clickradial ring.3" --exec "@wait 20" \
    --exec "@radialpoint ring.1 $out/s1" --exec "@radialpoint parent.3 $out/p3" --exec "@move 590 490" \
    --exec "@wait 2" --exec "@shot $out/sub.png" \
    --exec "@key escape" --exec "@wait 20" --exec "@shot $out/closed.png" \
    --exec "@rdown 300 220" --exec "@rdrag 330 240" --exec "@rdrag 360 260" --exec "@rup 360 260" \
    --exec "@wait 20" --exec "@shot $out/dragged.png" \
    --exec "@rclick 300 220" --exec "@wait 20" --exec "@rclick 450 330" --exec "@wait 20" \
    --exec "@radialpoint center $out/c2" --exec "@radialpoint ring.2 $out/m2" --exec "@shot $out/moved.png" \
    --exec "@rclick 450 330" --exec "@wait 20" --exec "@shot $out/rclosed.png" \
    --exec "@quit" > "$out/toh.log" 2>&1 || fail "toh exited with status $? (see $out/toh.log)"

[ -s "$out/r1" ] || fail "the right click opened no menu (see $out/toh.log)"
[ -s "$out/s1" ] && [ -s "$out/p3" ] || fail "the submenu did not open (see $out/toh.log)"
read -r cx cy < "$out/c"
r1=$(beside $(cat "$out/r1") $cx $cy 16)
r2=$(beside $(cat "$out/r2") $cx $cy 16)
s1=$(beside $(cat "$out/s1") $cx $cy 20)
p3=$(beside $(cat "$out/p3") $cx $cy 12)
dark "$out/open.png" $r1 || fail "the menu's ring is not drawn"
dark "$out/sub.png" $s1 || fail "the submenu's ring is not drawn"
blue "$out/sub.png" $p3 || fail "the segment that opened the submenu is not highlighted"
! dark "$out/closed.png" $s1 || fail "Esc did not close the menu"
! dark "$out/closed.png" $r2 || fail "Esc did not close the menu"
! dark "$out/dragged.png" $r2 || fail "a right drag opened the menu"
[ -s "$out/c2" ] || fail "a right click on another tile did not reopen the menu there"
read -r c2x c2y < "$out/c2"
[ $((c2x > 400)) = 1 ] || fail "the menu did not move to the other tile ($c2x $c2y)"
m2=$(beside $(cat "$out/m2") $c2x $c2y 16)
dark "$out/moved.png" $m2 || fail "the reopened menu is not drawn"
! dark "$out/moved.png" $r2 || fail "the first menu stayed open"
! dark "$out/rclosed.png" $m2 || fail "a right click on the menu did not close it"
grep -q "no radial menu part" "$out/toh.log" && fail "a script asked for a part that isn't there (see $out/toh.log)"

echo "e2e radial menu: OK"
