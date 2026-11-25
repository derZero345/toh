#!/usr/bin/env bash
# End-to-end check of a multiplayer session: a host (GM) and a client (Bob) on localhost.
# Opens two windows (needs Wayland or X11) and uses ImageMagick to look at screenshots.
# The two programs synchronize with @touch / @waitfile.
# Usage: tests/e2e_net.sh   (or: make e2e, which runs it directly and through the relay)
#        TOH_E2E_RELAY=1 tests/e2e_net.sh   goes through build/toh-relay on a local port
set -euo pipefail
cd "$(dirname "$0")/.."

out=${KEEP:-$(mktemp -d)}
relay_pid=
cleanup() {
    [ -z "$relay_pid" ] || kill "$relay_pid" 2>/dev/null || true
    [ -n "${KEEP:-}" ] || rm -rf "$out"
}
trap cleanup EXIT
icon=resources/images/toolIcons/area.png
port=${TOH_TEST_PORT:-41299}
fail() { echo "FAIL: $*" >&2; exit 1; }
export TOH_NO_PORTMAP=1
if [ "${TOH_E2E_RELAY:-0}" = 1 ]; then
    mode=relay
    rport=$((port + 1))
    ./build/toh-relay --port "$rport" > "$out/relay.log" 2>&1 &
    relay_pid=$!
    for _ in $(seq 50); do grep -q listening "$out/relay.log" 2>/dev/null && break; sleep 0.1; done
    grep -q listening "$out/relay.log" || fail "the relay did not start (see $out/relay.log)"
    export TOH_RELAY="127.0.0.1:$rport"
    host_cmd=":host"
else
    mode=direct
    export TOH_RELAY=
    host_cmd=":host --direct"
fi

# Is the tile at pixel X Y occupied in a screenshot? Empty tiles are light grey (about 0.9),
# the test icon is mostly black (about 0.65).
occupied() {
    local v
    v=$(magick "$1" -crop 16x16+$(($2 - 8))+$(($3 - 8)) +repage -colorspace gray -format '%[fx:mean]' info:)
    awk -v v="$v" 'BEGIN { exit !(v < 0.8) }'
}

# Tint of the tile at pixel X Y: "gold", "blue" or "none" (other people's selections are filled
# with their color at 35 % alpha; the GM is gold, the first player blue).
tint() {
    local rb
    rb=$(magick "$1" -crop 12x12+$(($2 - 6))+$(($3 - 6)) +repage -format '%[fx:mean.r] %[fx:mean.b]' info:)
    awk -v r="${rb% *}" -v b="${rb#* }" 'BEGIN { print (r - b > 0.1) ? "gold" : (b - r > 0.1) ? "blue" : "none" }'
}

# Is there an ownership ring in blue (Bob's color) right of the tile at pixel X Y? Scans a 60 px strip
# starting 10 px right of the point for a clearly blue pixel.
blue_ring() {
    magick "$1" -crop 60x1+$(($2 + 10))+$3 +repage -depth 8 txt:- |
        awk -F'[(),]' 'NR > 1 { if ($5 - $3 > 80) found = 1 } END { exit !found }'  # fields 3-5: R G B
}

# Mean grey of the tile at pixel X Y is between that of an empty tile and the icon: a faint preview.
faint() {
    local v
    v=$(magick "$1" -crop 16x16+$(($2 - 8))+$(($3 - 8)) +repage -colorspace gray -format '%[fx:mean]' info:)
    awk -v v="$v" 'BEGIN { exit !(v > 0.70 && v < 0.86) }'
}

# Is the pixel X Y red (the HP bar)?
red() {
    local rgb
    rgb=$(magick "$1" -crop 4x4+$(($2 - 2))+$(($3 - 2)) +repage -format '%[fx:mean.r] %[fx:mean.g] %[fx:mean.b]' info:)
    awk -v s="$rgb" 'BEGIN { split(s, c, " "); exit !(c[1] > 0.6 && c[2] < 0.3 && c[3] < 0.3) }'
}

# Is the pixel X Y green (the stamina bar)?
green() {
    local rgb
    rgb=$(magick "$1" -crop 4x4+$(($2 - 2))+$(($3 - 2)) +repage -format '%[fx:mean.r] %[fx:mean.g] %[fx:mean.b]' info:)
    awk -v s="$rgb" 'BEGIN { split(s, c, " "); exit !(c[2] > 0.5 && c[1] < 0.35 && c[3] < 0.4) }'
}

# The HP panel is anchored to the right edge (margin 8, frame width 128, first bar at y 28..48), and the
# window manager may resize the window, so compute the pixel from the screenshot's width:
# "panel_x FILE OFFSET" prints "X 38" with X = (width - 136) + OFFSET, the frame's left edge plus OFFSET.
panel_x() { echo "$(($(magick identify -format %w "$1") - 136 + $2)) 38"; }
# The stamina bar sits under the HP bar at y 52..66.4 (sample y 59); 5/9 of 128 px is 71 px of fill.
ST_IN=10; ST_OUT=110
st_x() { echo "$(($(magick identify -format %w "$1") - 136 + $2)) 59"; }
HP_IN=10; HP_NEG=-16    # offsets: inside the frame, and left of it (negative HP)

# The dice of a roll land anywhere on screen; ":r 0d6" right after ":r 2d6" clears them before any check.
# Tiles (pixel coordinates): A = Bob's character, B = the GM's, C = spawned after Bob joined.
A="300 200"; A2="300 260"    # Bob moves A one tile down
B="380 260"; B2="380 320"    # Bob tries to move B (refused); the GM moves B to B3
B3="440 260"
C="200 320"; C2="200 380"    # Bob claims C, the GM agrees, Bob moves it
P="500 140"; Q="140 140"     # empty tiles: the GM selects P, Bob selects Q (presence)
D="440 140"; D2="440 200"    # Bob asks for a new character on D, the GM agrees, Bob moves it to D2

TOH_JOINCODE_FILE="$out/code" timeout 90 ./toh --size 600x400 --port "$port" --auto-file "$icon" \
    --exec "@click $A" --exec ":c speed 6" --exec "@click $A" --exec ":c owner Bob" --exec "@click $A" \
    --exec "@click $B" --exec ":c speed 6" --exec "@click $B" \
    --exec "$host_cmd" \
    --exec "@waitnet 1" \
    --exec "@waitfile $out/client-joined" --exec "@click $P" --exec "@touch $out/host-selected" \
    --exec "@waitfile $out/client-selected" --exec "@wait 30" --exec "@shot $out/host-presence.png" \
    --exec "@touch $out/host-saw" \
    --exec "@waitfile $out/client-moved" --exec "@wait 30" \
    --exec "@shot $out/host-moved.png" \
    --exec "@click $B" --exec "@down lshift" --exec "@click $B3" --exec "@up lshift" \
    --exec "@click $C" --exec ":c speed 6" \
    --exec ":r 2d6" --exec ":r 0d6" \
    --exec "@touch $out/host-changed" \
    --exec "@waitfile $out/claimed" --exec "@wait 30" --exec "y" --exec "@touch $out/granted" \
    --exec "@waitfile $out/client-checked" --exec "@wait 30" \
    --exec "@shot $out/host-claim.png" \
    --exec "@waitfile $out/requested" --exec "@wait 30" --exec "@shot $out/host-request.png" --exec "y" \
    --exec "@touch $out/created" --exec "@waitfile $out/client-moved2" --exec "@wait 30" \
    --exec "@shot $out/host-created.png" \
    --exec "@click $A2" --exec ":c h 20 ms 9" --exec "@wait 10" --exec "@shot $out/host-hp-full.png" \
    --exec "@touch $out/hp-set" --exec "@waitfile $out/hp-damaged" --exec "@wait 30" \
    --exec "@shot $out/host-hp.png" \
    --exec ":unhost" --exec "@wait 10" --exec ":q!" \
    > "$out/host.log" 2>&1 &
host=$!

for _ in $(seq 100); do [ -s "$out/code" ] && break; sleep 0.1; done
[ -s "$out/code" ] || fail "the host wrote no join code"
code=$(cat "$out/code")
[[ $code == toh1-* ]] || fail "unexpected join code: $code"

# The client's final :q must be refused (after the session ended the table counts as modified);
# :q! then quits.
timeout 90 ./toh --size 600x400 --auto-file "$icon" \
    --exec ":join $code Bob" --exec "@waitnet 1" --exec "@wait 10" \
    --exec "@shot $out/client-joined.png" --exec "@touch $out/client-joined" \
    --exec "@waitfile $out/host-selected" --exec "@wait 30" --exec "@shot $out/client-presence.png" \
    --exec "@click $Q" --exec "@wait 10" --exec "@touch $out/client-selected" --exec "@waitfile $out/host-saw" \
    --exec "@click $A" --exec "@down lshift" --exec "@click $A2" --exec "@up lshift" --exec "@wait 30" \
    --exec "@click $B" --exec "@down lshift" --exec "@click $B2" --exec "@up lshift" --exec "@wait 30" \
    --exec "@shot $out/client-moved.png" --exec "@touch $out/client-moved" \
    --exec "@waitfile $out/host-changed" --exec "@wait 30" \
    --exec "@shot $out/client-changed.png" \
    --exec "@click $C" --exec ":claim" --exec "@touch $out/claimed" \
    --exec "@waitfile $out/granted" --exec "@wait 30" \
    --exec "@down lshift" --exec "@click $C2" --exec "@up lshift" --exec "@wait 30" \
    --exec "@shot $out/client-claim.png" --exec "@touch $out/client-checked" \
    --exec "@click $D" --exec ":c speed 6" --exec "@wait 20" --exec "@shot $out/client-requested.png" \
    --exec "@touch $out/requested" --exec "@waitfile $out/created" --exec "@wait 30" \
    --exec "@shot $out/client-created.png" \
    --exec "@click $D" --exec "@down lshift" --exec "@click $D2" --exec "@up lshift" --exec "@wait 30" \
    --exec "@touch $out/client-moved2" \
    --exec "@waitfile $out/hp-set" --exec "@wait 30" \
    --exec "@click $A2" --exec ":c damage 25" --exec ":c spend 4" --exec ":c spend 9" --exec "@wait 30" --exec "@shot $out/client-hp.png" \
    --exec "@touch $out/hp-damaged" \
    --exec "@wait 60" \
    --exec ":q" --exec "@shot $out/client-quit-refused.png" --exec ":q!" \
    > "$out/client.log" 2>&1 || fail "the client exited with status $? (see $out/client.log)"
wait "$host" || fail "the host exited with status $? (see $out/host.log)"

# the client got the host's table
occupied "$out/client-joined.png" $A || fail "the client does not show Bob's character"
occupied "$out/client-joined.png" $B || fail "the client does not show the GM's character"
# presence: each side sees the other's selected tile in their color, and nothing on the others
[ "$(tint "$out/client-joined.png" $P)" = none ] || fail "tile P is tinted before the GM selected it"
[ "$(tint "$out/client-presence.png" $P)" = gold ] || fail "Bob does not see the GM's selection in gold"
[ "$(tint "$out/client-presence.png" $Q)" = none ] || fail "Bob sees a selection on Q that nobody made"
[ "$(tint "$out/host-presence.png" $Q)" = blue ] || fail "the GM does not see Bob's selection in blue"
[ "$(tint "$out/host-presence.png" $P)" = none ] || fail "the GM's own selection is drawn as presence"
# Bob's legal move reached the host; his move of the GM's character did not
occupied "$out/host-moved.png" $A2 || fail "Bob's move did not show on the host"
! occupied "$out/host-moved.png" $A || fail "Bob's character is still at its old tile on the host"
occupied "$out/host-moved.png" $B || fail "the GM's character moved although Bob may not move it"
! occupied "$out/host-moved.png" $B2 || fail "Bob moved the GM's character"
# the GM's move and new character reached the client (the image too)
occupied "$out/client-changed.png" $B3 || fail "the GM's move did not show on the client"
occupied "$out/client-changed.png" $C || fail "the character the GM spawned did not show on the client"
# the GM granted Bob's claim, so Bob could move the new character
occupied "$out/host-claim.png" $C2 || fail "Bob could not move the character he claimed"
! occupied "$out/host-claim.png" $C || fail "the claimed character is still at its old tile"
# ownership rings: Bob's character has his blue ring, the GM's has none
blue_ring "$out/client-joined.png" $A || fail "Bob's character has no blue ring"
! blue_ring "$out/client-joined.png" $B || fail "the GM's character has a blue ring"
# Bob's own character: the party does not see the request, the GM sees a faint preview, then it is created
! occupied "$out/client-requested.png" $D || fail "the requested character showed before the GM agreed"
faint "$out/host-request.png" $D || fail "the GM does not see a preview of the requested character"
occupied "$out/client-created.png" $D || fail "the accepted character does not show on the client"
occupied "$out/host-created.png" $D2 || fail "Bob could not move the character he created"
[ -f "$out/client-quit-refused.png" ] || fail ":q quit the client although the session ended"
# HP panel: the GM's full bar, Bob's own damage (-5 of 20) on his screen and on the GM's
red "$out/host-hp-full.png" $(panel_x "$out/host-hp-full.png" $HP_IN) || fail "the GM's HP panel does not show a full red bar"
! red "$out/host-hp-full.png" $(panel_x "$out/host-hp-full.png" $HP_NEG) || fail "the full HP bar reaches left of its frame"
red "$out/client-hp.png" $(panel_x "$out/client-hp.png" $HP_NEG) || fail "Bob's HP panel does not show negative HP left of the frame"
! red "$out/client-hp.png" $(panel_x "$out/client-hp.png" $HP_IN) || fail "Bob's HP bar is still filled inside the frame at -5 HP"
red "$out/host-hp.png" $(panel_x "$out/host-hp.png" $HP_NEG) || fail "the GM does not see Bob's damage on the HP panel"
# Stamina bar: 9 max, spent 4 (5 left), the spend of 9 refused, so green at +10 and not at +110, on both screens
green "$out/client-hp.png" $(st_x "$out/client-hp.png" $ST_IN) || fail "Bob's stamina bar is not green at 5/9"
! green "$out/client-hp.png" $(st_x "$out/client-hp.png" $ST_OUT) || fail "Bob's stamina bar is too full (the spend of 9 should be refused)"
green "$out/host-hp.png" $(st_x "$out/host-hp.png" $ST_IN) || fail "the GM does not see Bob's stamina bar"
! green "$out/host-hp.png" $(st_x "$out/host-hp.png" $ST_OUT) || fail "the GM sees a too full stamina bar for Bob"

if [ "$mode" = relay ]; then
    grep -q linked "$out/relay.log" || fail "the client did not come through the relay (see $out/relay.log)"
fi

echo "e2e net ($mode): OK"
