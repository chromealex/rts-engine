#!/usr/bin/env bash
# Lighting cost benchmark: runs the game for N frames with lights off and on every tier at night
# and prints ms per frame. Meant for a software GL (llvmpipe / SwiftShader: rendering on the CPU,
# so fill cost shows up like on a weak GPU) under Xvfb. Fails (exit 1) when the Low tier costs more
# than MAX_LOW_OVERHEAD percent over "off" — the tier old Android phones get must stay cheap.
#
#   tools/perf/lighting_bench.sh [build dir] [game] [frames]
#   env: MAX_LOW_OVERHEAD (default 35), DISPLAY (default :97, starts Xvfb if needed)
set -euo pipefail
BUILD=${1:-build/linux-release}
GAME=${2:-templates/skirmish}
FRAMES=${3:-600}
MAX=${MAX_LOW_OVERHEAD:-35}
export DISPLAY=${DISPLAY:-:97}
if ! xdpyinfo >/dev/null 2>&1; then Xvfb "$DISPLAY" -screen 0 1280x800x24 >/dev/null 2>&1 & sleep 2; fi

run() { # tier -> ms per frame (best of 2: the first run also warms caches)
    local best=999999
    for _ in 1 2; do
        local t0 t1 ms
        t0=$(date +%s%N)
        RTS_CONTENT="$GAME" RTS_LIGHTS="$1" RTS_SHADOWS=low RTS_ADAPTIVE=off RTS_TIME_OF_DAY=0.0 RTS_AUDIO=off \
            RTS_EXIT_AFTER_FRAMES="$FRAMES" "$BUILD/RTSGame" --map 0 --spectate >/dev/null 2>&1
        t1=$(date +%s%N)
        ms=$(( (t1 - t0) / 1000 / FRAMES ))   # microseconds per frame
        (( ms < best )) && best=$ms
    done
    echo "$best"
}

declare -A us
for tier in off low medium high; do us[$tier]=$(run $tier); done
printf "%-8s %10s %10s\n" tier "ms/frame" "vs off"
for tier in off low medium high; do
    printf "%-8s %10.2f %9d%%\n" "$tier" "$(echo "${us[$tier]} / 1000" | bc -l)" $(( (us[$tier] - us[off]) * 100 / us[off] ))
done
over=$(( (us[low] - us[off]) * 100 / us[off] ))
if (( over > MAX )); then echo "FAIL: low tier costs ${over}% over off (limit ${MAX}%)"; exit 1; fi
echo "OK: low tier +${over}% (limit ${MAX}%)"
