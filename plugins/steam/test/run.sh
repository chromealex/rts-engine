#!/usr/bin/env bash
# Tests plugins/steam against a fake Steam client. Needs the Steamworks SDK headers
# (STEAMWORKS_SDK=<sdk folder>) and the engine's rts_plugins library built in $BUILD (default build/linux-release).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd); root=$(cd "$here/../../.." && pwd)
sdk=${STEAMWORKS_SDK:?set STEAMWORKS_SDK}; build=${BUILD:-$root/build/linux-release}
out=$(mktemp -d)
python3 "$here/gen_fakes.py" "$sdk/public/steam" "$out/fakes.inc"
c++ -std=c++20 -fPIC -shared -w -isystem "$sdk/public" -I"$out" "$here/fake_steam.cpp" -o "$out/libsteam_api.so"
c++ -std=c++20 -Wall -Wextra -Werror -I"$root" -isystem "$sdk/public" "$here/test_steam.cpp" "$root/plugins/steam/src/SteamPlugin.cpp" \
    -L"$build" -lrts_plugins -L"$out" -lsteam_api -Wl,-rpath,"$out" -o "$out/test_steam"
"$out/test_steam"
