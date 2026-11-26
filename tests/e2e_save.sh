#!/usr/bin/env bash
# End-to-end check of :save, :load and the unsaved-changes guard of :q.
# Opens a window (needs Wayland or X11) and uses ImageMagick to compare screenshots.
# Usage: tests/e2e_save.sh   (or: make e2e)
set -euo pipefail
cd "$(dirname "$0")/.."
export TOH_SETTINGS= # no settings file: the user's theme must not change the pixels, and :t must not change it

out=${KEEP:-$(mktemp -d)}
[ -n "${KEEP:-}" ] || trap 'rm -rf "$out"' EXIT
icon=resources/images/toolIcons/area.png
bg=resources/images/toolIcons/cone.png
fail() { echo "FAIL: $*" >&2; exit 1; }

# Normalized RMSE between two screenshots (0 = identical). Only the top 70% is compared:
# the console log at the bottom shows different messages in each shot.
rmse() {
    magick "$1" "$2" -gravity North -crop 100%x70%+0+0 +repage -metric RMSE -compare -format '%[distortion]' info:
}
similar() { awk -v v="$(rmse "$1" "$2")" 'BEGIN { exit !(v < 0.005) }'; }

# The final :q must quit by itself (table unchanged since :load!); otherwise timeout fails the test.
timeout 60 ./toh --size 600x400 --auto-file "$icon" \
    --exec ":hud" --exec ":b $bg" \
    --exec "@click 300 200" --exec ":c" \
    --exec "@click 380 260" --exec ":w" \
    --exec "tp waypoint create camp 2 3" \
    --exec ":i" \
    --exec ":save $out/table" \
    --exec ":load $out/table.tuhh" \
    --exec "@shot $out/loaded.png" \
    --exec ":c delete" --exec ":g" \
    --exec "@shot $out/modified.png" \
    --exec ":q" \
    --exec "@shot $out/quit-refused.png" \
    --exec ":load $out/table.tuhh" \
    --exec "@shot $out/load-refused.png" \
    --exec ":load! $out/table.tuhh" \
    --exec "@shot $out/reloaded.png" \
    --exec "@click 300 200" --exec ":c owner Bob" \
    --exec ":q" \
    --exec "@shot $out/owner-refused.png" \
    --exec ":c owner none" \
    --exec ":q" \
    || fail "toh exited with status $? (124 = the last :q did not quit)"

[ "$(head -c 4 "$out/table.tuhh")" = TUHH ] || fail "table.tuhh has no TUHH header"
[ -f "$out/quit-refused.png" ] || fail ":q quit despite unsaved changes"
[ -f "$out/owner-refused.png" ] || fail ":q quit although the owner of a character changed"
similar "$out/modified.png" "$out/load-refused.png" || fail ":load replaced a modified table"
similar "$out/loaded.png" "$out/reloaded.png" || fail ":load! did not restore the saved table"
! similar "$out/loaded.png" "$out/modified.png" || fail "the modification was not visible (test is not sensitive)"

echo "e2e save/load: OK ($(stat -c %s "$out/table.tuhh") byte save file)"
