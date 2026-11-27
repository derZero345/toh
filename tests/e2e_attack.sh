#!/usr/bin/env bash
# End-to-end check of attacks: during initiative, the attacker's action menu -> Major action -> Attack -> Test attack
# shows the tiles in range, a click on the tile with the other character opens the direction menu there (four
# entries), Up declares the attack: the tile gets a (hollow) mark; :i next twice resolves it (the resolution phase)
# and the log says the character took damage. Esc in the direction menu
# goes back to choosing a tile, a second Esc cancels. Then blocks, in test mode: the target blocks up on its own
# tile through Major action -> Block -> Test block and the toggle ring (Up, then the centre), the tile gets a block
# mark, and an attack up on it misses. Then dodges, in test mode: the target gets ET 0 (:c et 0), dodges up through
# Major action -> Dodge and a click on the tile above, the tile it left gets a dodge mark, and a neutral attack on
# that tile meets the dodge (a step away from the attacker: up) and misses. Opens a window (needs Wayland or X11) and
# uses ImageMagick.
# Usage: tests/e2e_attack.sh   (or: make e2e)
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

# the attacker at 300 220, the target on the tile above it (about 70 px up); only the attacker is in the initiative,
# so its turn comes first
timeout 60 ./toh --size 600x500 --auto-file "$icon" \
    --exec "@click 300 150" --exec ":c speed 4 maxhealth 10" --exec "@wait 5" \
    --exec "@click 300 220" --exec ":c speed 4" --exec "@wait 5" --exec "@click 300 220" \
    --exec ":i add" --exec ":i next" --exec "@click 500 60" --exec "@wait 5" --exec "@shot $out/before.png" \
    --exec "@rclick 300 220" --exec "@wait 20" --exec "@clickradial ring.4" --exec "@wait 20" \
    --exec "@clickradial ring.5" --exec "@wait 20" --exec "@clickradial ring.1" --exec "@wait 20" \
    --exec "@move 500 60" --exec "@wait 5" --exec "@shot $out/choosing.png" \
    --exec "@click 300 150" --exec "@wait 20" \
    --exec "@radialpoint ring.4 $out/d4" --exec "@radialpoint ring.5 $out/d5" \
    --exec "@key escape" --exec "@wait 20" --exec "@shot $out/back.png" \
    --exec "@click 300 150" --exec "@wait 20" --exec "@clickradial ring.1" --exec "@wait 20" \
    --exec "@move 500 60" --exec "@wait 5" --exec "@shot $out/attacked.png" \
    --exec "@rclick 300 220" --exec "@wait 20" --exec "@clickradial ring.4" --exec "@wait 20" \
    --exec "@clickradial ring.5" --exec "@wait 20" --exec "@radialpoint ring.1 $out/used" \
    --exec "@key escape" --exec "@wait 20" \
    --exec ":i next" --exec ":i next" --exec "@wait 5" --exec "@logfile $out/log.txt" \
    --exec "@quit" > "$out/toh.log" 2>&1 || fail "toh exited with status $? (see $out/toh.log)"

# the range: the tiles around the attacker light up (the one below it, away from the target, too)
differ "$out/before.png" "$out/choosing.png" 300 290 || fail "choosing a tile for the attack shows no range"
[ -s "$out/d4" ] && [ ! -s "$out/d5" ] || fail "a click on a tile in range did not open the direction menu (see $out/toh.log)"
differ "$out/before.png" "$out/back.png" 300 290 || fail "Esc in the direction menu left choosing a tile"
grep -q "declares Test attack on \[.*\] up" "$out/log.txt" || fail "the attack was not declared (see $out/log.txt)"
grep -q ": Test attack on \[.*\] up" "$out/log.txt" || fail "the attack did not resolve (see $out/log.txt)"
grep -q ": success, [1-4] damage, HP [0-9]/10 " "$out/log.txt" || fail "the character on the tile took no damage (see $out/log.txt)"
differ "$out/back.png" "$out/attacked.png" 300 150 || fail "the attacked tile has no mark"
! differ "$out/before.png" "$out/attacked.png" 300 290 || fail "the range is still shown after the attack"
[ -s "$out/used" ] || fail "the Attack submenu did not open again"

# blocks (test mode: no initiative needed)
timeout 60 ./toh --size 600x500 --auto-file "$icon" \
    --exec "@click 300 150" --exec ":c speed 4 maxhealth 10" --exec "@wait 5" \
    --exec "@click 300 220" --exec ":c speed 4" --exec "@wait 5" --exec ":tm" \
    --exec "@click 500 60" --exec "@wait 5" --exec "@shot $out/b-before.png" \
    --exec "@rclick 300 150" --exec "@wait 20" --exec "@clickradial ring.4" --exec "@wait 20" \
    --exec "@clickradial ring.6" --exec "@wait 20" --exec "@clickradial ring.1" --exec "@wait 20" \
    --exec "@radialpoint ring.3 $out/t3" --exec "@radialpoint ring.4 $out/t4" \
    --exec "@clickradial ring.1" --exec "@wait 5" --exec "@clickradial center" --exec "@wait 20" \
    --exec "@move 500 60" --exec "@wait 5" --exec "@shot $out/blocked.png" \
    --exec "@rclick 300 220" --exec "@wait 20" --exec "@clickradial ring.4" --exec "@wait 20" \
    --exec "@clickradial ring.5" --exec "@wait 20" --exec "@clickradial ring.1" --exec "@wait 20" \
    --exec "@click 300 150" --exec "@wait 20" --exec "@clickradial ring.1" --exec "@wait 20" \
    --exec "@logfile $out/b-log.txt" \
    --exec "@quit" > "$out/toh-b.log" 2>&1 || fail "toh exited with status $? (see $out/toh-b.log)"

[ -s "$out/t3" ] && [ ! -s "$out/t4" ] || fail "Test block did not open the ring of three directions (see $out/toh-b.log)"
grep -q "Test block on \[.*\]: up and neutral, BV 2" "$out/b-log.txt" || fail "the block is not in the log (see $out/b-log.txt)"
differ "$out/b-before.png" "$out/blocked.png" 300 150 || fail "the blocked tile has no mark"
grep -q "AV 0 against a block of BV 2" "$out/b-log.txt" || fail "the attack did not meet the block (see $out/b-log.txt)"
grep -q ": miss, 0 damage, HP 10/10 " "$out/b-log.txt" || fail "an attack up on a tile blocked up did not miss (see $out/b-log.txt)"

# dodges (test mode); with ET 0 every Dodge Value (1d8 + AGI 0) wins against the Test attack's AV 0
timeout 60 ./toh --size 600x500 --auto-file "$icon" \
    --exec "@click 300 150" --exec ":c speed 4 maxhealth 10" --exec "@wait 5" \
    --exec "@click 300 220" --exec ":c speed 4" --exec "@wait 5" --exec ":tm" \
    --exec "@click 300 150" --exec ":c et 0" --exec "@click 500 60" --exec "@wait 5" \
    --exec "@shot $out/d-before.png" \
    --exec "@rclick 300 150" --exec "@wait 20" --exec "@clickradial ring.4" --exec "@wait 20" \
    --exec "@clickradial ring.7" --exec "@wait 20" --exec "@click 300 80" --exec "@wait 20" \
    --exec "@move 500 60" --exec "@wait 5" --exec "@shot $out/dodged.png" \
    --exec "@rclick 300 220" --exec "@wait 20" --exec "@clickradial ring.4" --exec "@wait 20" \
    --exec "@clickradial ring.5" --exec "@wait 20" --exec "@clickradial ring.1" --exec "@wait 20" \
    --exec "@click 300 150" --exec "@wait 20" --exec "@clickradial ring.3" --exec "@wait 20" \
    --exec "@logfile $out/d-log.txt" \
    --exec "@quit" > "$out/toh-d.log" 2>&1 || fail "toh exited with status $? (see $out/toh-d.log)"

grep -q "Dodge from \[.*\] to \[.*\], DV [0-9]* (1d8: [1-8], AGI 0)" "$out/d-log.txt" ||
    fail "the dodge is not in the log (see $out/d-log.txt)"
differ "$out/d-before.png" "$out/dodged.png" 300 150 || fail "the tile left has no dodge mark"
differ "$out/d-before.png" "$out/dodged.png" 300 80 || fail "the dodger did not move up"
grep -q "Test attack on \[.*\] neutral" "$out/d-log.txt" || fail "the attack on the tile left is missing (see $out/d-log.txt)"
grep -q " (dodged up, DV [0-9]* against AV 0 + ET 0): miss, 0 damage, HP 10/10 " "$out/d-log.txt" ||
    fail "a neutral attack on the tile left did not meet the dodge and miss (see $out/d-log.txt)"

echo "e2e attacks, blocks and dodges: OK"
