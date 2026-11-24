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

# Tiles (pixel coordinates): A = Bob's character, B = the GM's, C = spawned after Bob joined.
A="300 200"; A2="300 260"    # Bob moves A one tile down
B="380 260"; B2="380 320"    # Bob tries to move B (refused); the GM moves B to B3
B3="440 260"
C="200 320"; C2="200 380"    # Bob claims C, the GM agrees, Bob moves it
P="500 140"; Q="140 140"     # empty tiles: the GM selects P, Bob selects Q (presence)

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
    --exec ":r 2d6" \
    --exec "@touch $out/host-changed" \
    --exec "@waitfile $out/claimed" --exec "@wait 30" --exec "y" --exec "@touch $out/granted" \
    --exec "@waitfile $out/client-checked" --exec "@wait 30" \
    --exec "@shot $out/host-claim.png" \
    --exec ":unhost" --exec "@wait 10" --exec ":q!" \
    > "$out/host.log" 2>&1 &
host=$!

for _ in $(seq 100); do [ -s "$out/code" ] && break; sleep 0.1; done
[ -s "$out/code" ] || fail "the host wrote no join code"
code=$(cat "$out/code")
[[ $code == toh1-* ]] || fail "unexpected join code: $code"

# The client's final :q must be refused (after the session ended the table counts as modified);
# :q! then quits.
timeout 90 ./toh --size 600x400 \
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
[ -f "$out/client-quit-refused.png" ] || fail ":q quit the client although the session ended"

if [ "$mode" = relay ]; then
    grep -q linked "$out/relay.log" || fail "the client did not come through the relay (see $out/relay.log)"
fi

echo "e2e net ($mode): OK"
