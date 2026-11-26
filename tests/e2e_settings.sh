#!/usr/bin/env bash
# End-to-end check of the settings file: the theme chosen with :t is written to it and used at the next start,
# a theme shown by :load is not remembered, and an unknown theme in the file falls back to the default.
# Opens a window (needs Wayland or X11) and uses ImageMagick.
# Usage: tests/e2e_settings.sh   (or: make e2e)
set -euo pipefail
cd "$(dirname "$0")/.."

out=${KEEP:-$(mktemp -d)}
[ -n "${KEEP:-}" ] || trap 'rm -rf "$out"' EXIT
mkdir -p "$out"
export TOH_SETTINGS="$out/settings.txt"
fail() { echo "FAIL: $*" >&2; exit 1; }

# the colour of the empty map in the top right corner (the toolbox is left, the HUD at the bottom): "r g b" 0..255
corner() { magick "$1" -gravity northeast -crop 4x4+8+8 +repage -format '%[fx:int(mean.r*255)] %[fx:int(mean.g*255)] %[fx:int(mean.b*255)]' info:; }
near() { awk -v s="$1" -v r="$2" -v g="$3" -v b="$4" 'BEGIN { split(s, c, " ");
    exit !((c[1]-r)^2 < 100 && (c[2]-g)^2 < 100 && (c[3]-b)^2 < 100) }'; }
run() { timeout 60 ./toh --size 400x300 "$@" --exec "@quit" >> "$out/toh.log" 2>&1 || fail "toh exited with status $? (see $out/toh.log)"; }

# 1. :t dark is written to the file; a save with another theme is made along the way
run --exec ":t red" --exec ":save $out/red.tuhh" --exec ":t dark" --exec "@wait 30" --exec "@shot $out/1.png"
grep -qx "theme = dark" "$out/settings.txt" || fail "the file does not say theme = dark: $(cat "$out/settings.txt")"
head -c1 "$out/settings.txt" | grep -q "#" || fail "the new file has no header comment"

# 2. the next start uses it (Dark: 0d1219)
run --exec "@wait 2" --exec "@shot $out/2.png"
near "$(corner "$out/2.png")" 13 18 25 || fail "the next start is not dark: $(corner "$out/2.png")"

# 3. :load shows the save's theme (Red: 101820) but doesn't change the remembered one
run --exec ":load $out/red.tuhh" --exec "@wait 30" --exec "@shot $out/3.png"
near "$(corner "$out/3.png")" 16 24 32 || fail ":load did not show the save's theme: $(corner "$out/3.png")"
grep -qx "theme = dark" "$out/settings.txt" || fail ":load changed the remembered theme"

# 4. lines the game doesn't know stay; an unknown theme falls back to Light (d0d0d0)
printf '# my notes\ntheme = nope\nother = 1\n' > "$out/settings.txt"
run --exec "@wait 2" --exec "@shot $out/4.png"
near "$(corner "$out/4.png")" 208 208 208 || fail "an unknown theme did not fall back to light: $(corner "$out/4.png")"
run --exec ":t green"
[ "$(cat "$out/settings.txt")" = "$(printf '# my notes\ntheme = green\nother = 1')" ] ||
    fail "the file lost lines: $(cat "$out/settings.txt")"

echo "e2e settings: OK"
