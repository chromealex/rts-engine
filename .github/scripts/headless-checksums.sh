#!/usr/bin/env bash
# Runs a bot match on every sample game and writes "<game> <seed> <checksum> <winner>" lines.
# Usage: headless-checksums.sh <rts_headless binary (or "wine path.exe")> <out file>
# The CI "determinism" job compares these files across platforms: they must be byte-identical.
set -euo pipefail
bin="$1"; out="$2"; : > "$out"
for dir in games/*/; do
  game=$(basename "$dir")
  [ -f "$dir/game.json" ] || continue
  for seed in 1 3 7; do
    log=$($bin --project "games/$game" --seed "$seed" 2>&1) || { echo "$log"; echo "::error::$game seed $seed: headless failed"; exit 1; }
    sum=$(grep -o 'Checksum: [0-9a-f]*' <<<"$log" | awk '{print $2}')
    win=$(grep -o 'winner=[^ ]*' <<<"$log" | head -1 || true)
    [ -n "$sum" ] || { echo "$log"; echo "::error::$game seed $seed: no checksum"; exit 1; }
    [ -n "$win" ] || { echo "$log"; echo "::error::$game seed $seed: match ended without a winner"; exit 1; }
    echo "$game $seed $sum $win" | tee -a "$out"
  done
done
