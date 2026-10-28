#!/usr/bin/env bash
# Installs the APK on a running emulator/device, plays through launch → background → resume →
# back → relaunch, and fails on native crashes or a dead process. Screenshots and logcat go to
# ./android-smoke/ (uploaded by CI).
# Usage: android-smoke.sh <dir with the .apk>
set -uo pipefail
out=android-smoke; mkdir -p "$out"
apks=("$1"/*.apk)
apk=${apks[0]}
[ -f "$apk" ] || { echo "::error::no APK in $1"; exit 1; }

adb wait-for-device
adb logcat -c
adb install -r "$apk" || { echo "::error::adb install failed"; exit 1; }
pkg=$(adb shell pm list packages -3 | sed 's/package://' | tr -d '\r' | head -1)
echo "package: $pkg"

alive() { [ -n "$(adb shell pidof "$pkg" | tr -d '\r')" ]; }
launch() { adb shell monkey -p "$pkg" -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1; }
shot() { adb exec-out screencap -p > "$out/$1.png"; }
fail() { echo "::error::$1"; adb logcat -d > "$out/logcat.txt"; exit 1; }

launch; sleep 20
alive || fail "app died after launch"
shot 1-launch
adb shell input tap 640 360; sleep 2                        # a touch reaches the game
alive || fail "app died after touch"

adb shell input keyevent KEYCODE_HOME; sleep 5             # background: surface destroyed
launch; sleep 8                                             # resume: surface recreated
alive || fail "app died after resume from background"
shot 2-resumed

adb shell input keyevent KEYCODE_BACK; sleep 5             # back finishes the activity
launch; sleep 15                                            # cold start again
alive || fail "app died on relaunch after back"
shot 3-relaunched

adb logcat -d > "$out/logcat.txt"
if grep -E "FATAL EXCEPTION|Fatal signal|Abort message|backtrace:" "$out/logcat.txt" | grep -v "^--"; then
  fail "native crash in logcat"
fi
echo "Android smoke test passed"
