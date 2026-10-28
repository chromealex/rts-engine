#!/bin/sh
# Bit-exactness guard for the Lua port (scripting agent): every sample game, seeds 1-3, must print
# the checksum recorded in baseline.txt (fixtures in tests/checksums/games too: recorded with the C++
# implementation before each port). Usage: tests/checksums/check.sh [build dir] [--record]
# Prints sim time per match too (perf comparison). Exit 1 on any difference.
set -u
B=${1:-build/lin}
DIR=$(dirname "$0")
OUT=$(mktemp)
fail=0
# sample games + fixtures that exercise mechanics the samples don't (tests/checksums/games)
for gd in games/* "$DIR"/games/*; do
  [ -f "$gd/game.json" ] || continue
  g=$(basename "$gd")
  for s in 1 2 3; do
    line=$("$B/rts_headless" --project "$gd" --seed $s --quiet 2>&1)
    sum=$(echo "$line" | sed -n 's/^Checksum: //p')
    ms=$(echo "$line" | sed -n 's/.*sim=\([0-9]*\) ms.*/\1/p')
    win=$(echo "$line" | sed -n 's/^Result: winner=\([^ ]*\).*/\1/p')
    echo "$g $s $sum" >> "$OUT"
    printf '%-14s seed %s  %s  %6s ms  winner=%s\n' "$g" "$s" "$sum" "$ms" "$win"
  done
done
if [ "${2:-}" = "--record" ]; then cp "$OUT" "$DIR/baseline.txt"; echo "recorded $DIR/baseline.txt"; rm -f "$OUT"; exit 0; fi
if ! diff "$DIR/baseline.txt" "$OUT" > /dev/null; then echo "CHECKSUMS DIFFER:"; diff "$DIR/baseline.txt" "$OUT"; fail=1; else echo "all checksums identical"; fi
rm -f "$OUT"
exit $fail
