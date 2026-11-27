#!/usr/bin/env bash
# End-to-end check of a player creating a character through the bar: the GM declines the first request (the
# waiting line goes, Create character stays), accepts the second (the character is the player's).
# Opens two windows (needs Wayland or X11) and uses ImageMagick. Usage: tests/e2e_create.sh (or: make e2e)
set -euo pipefail
cd "$(dirname "$0")/.."
export TOH_SETTINGS= TOH_NO_PORTMAP=1 TOH_RELAY=

out=${KEEP:-$(mktemp -d)}
[ -n "${KEEP:-}" ] || trap 'rm -rf "$out"' EXIT
mkdir -p "$out"
icon=resources/images/toolIcons/area.png
port=${TOH_TEST_PORT:-41297}
fail() { echo "FAIL: $*" >&2; exit 1; }
# is the tile at pixel X Y occupied? empty tiles are light grey (about 0.9), the test icon mostly black
occupied() {
    local v
    v=$(magick "$1" -crop 16x16+$(($2 - 8))+$(($3 - 8)) +repage -colorspace gray -format '%[fx:mean]' info:)
    awk -v v="$v" 'BEGIN { exit !(v < 0.8) }'
}
T="420 172"   # a tile centre: where Bob places both characters

# the steps for a character named $1, then Place on T, as --exec arguments in the array "steps"
make_steps() {
    steps=(--exec "@clickhud cc.start" --exec "@wait 3" --exec "@type $1" --exec "@wait 2"
        --exec "@clickhud cc.next" --exec "@wait 3" --exec "@clickhud cc.next" --exec "@wait 3"
        --exec "@clickhud cc.agi.plus" --exec "@wait 2" --exec "@clickhud cc.next" --exec "@wait 3"
        --exec "@clickhud cc.picture" --exec "@wait 3" --exec "@clickhud cc.next" --exec "@wait 3"
        --exec "@clickhud cc.next" --exec "@wait 3" --exec "@click $T" --exec "@wait 10")
}
make_steps Mao; mao=("${steps[@]}")
make_steps Kiri; kiri=("${steps[@]}")

TOH_JOINCODE_FILE="$out/code" timeout 90 ./toh --size 900x650 --port "$port" --auto-file "$icon" \
    --exec ":host --direct" --exec "@waitnet 1" \
    --exec "@waitfile $out/asked1" --exec "@wait 30" --exec "n" --exec "@touch $out/declined" \
    --exec "@waitfile $out/asked2" --exec "@wait 30" --exec "y" --exec "@touch $out/accepted" \
    --exec "@waitfile $out/client-done" --exec "@logfile $out/host-log.txt" --exec ":unhost" --exec "@wait 10" \
    --exec ":q!" > "$out/host.log" 2>&1 &
host=$!
for _ in $(seq 100); do [ -s "$out/code" ] && break; sleep 0.1; done
[ -s "$out/code" ] || fail "the host wrote no join code"
code=$(cat "$out/code")

timeout 90 ./toh --size 900x650 --auto-file "$icon" \
    --exec ":join $code Bob" --exec "@waitnet 1" --exec "@wait 10" --exec "@hudrect cc.start $out/start" \
    "${mao[@]}" --exec "@hudrect cc.wait.1 $out/waiting1" --exec "@touch $out/asked1" \
    --exec "@waitfile $out/declined" --exec "@wait 30" \
    --exec "@hudrect cc.wait.1 $out/after-decline" --exec "@hudrect cc.start $out/start2" \
    "${kiri[@]}" --exec "@touch $out/asked2" --exec "@waitfile $out/accepted" --exec "@wait 30" \
    --exec "@shot $out/created.png" --exec "@hudrect cc.start $out/start3" \
    --exec "@click $T" --exec "@wait 5" --exec "@hudrect hp $out/hp" --exec ":c hp 0" --exec "@wait 30" \
    --exec "@logfile $out/client-log.txt" \
    --exec "@touch $out/client-done" --exec "@wait 60" --exec ":q!" \
    > "$out/client.log" 2>&1 || fail "the client exited with status $? (see $out/client.log)"
wait "$host" || fail "the host exited with status $? (see $out/host.log)"

[ -s "$out/start" ] || fail "a player without characters has no Create character button"
[ -s "$out/waiting1" ] || fail "no waiting line after the request"
[ ! -s "$out/after-decline" ] || fail "the waiting line stayed after the GM declined"
[ -s "$out/start2" ] || fail "Create character is gone after the GM declined"
grep -q "The GM declined your character." "$out/client-log.txt" || fail "no decline message"
grep -q "The GM accepted your character." "$out/client-log.txt" || fail "no accept message"
occupied "$out/created.png" $T || fail "the accepted character is not on the tile"
[ ! -s "$out/start3" ] || fail "Create character still shows although Bob owns a character"
# the bar's HP row is there only when the viewer has a character ("No character" has the tab, not the bars)
[ -s "$out/hp" ] || fail "Bob does not see his new character in the HUD (not his?)"
# the host lets Bob change it (the line comes back with the host's broadcast); STR 0 is max HP 12
grep -q "Kiri: 12/12 -> 0/12" "$out/client-log.txt" || fail "Bob cannot change his new character (see $out/client-log.txt)"
echo "e2e create: OK"
