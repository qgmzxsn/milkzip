#!/bin/sh
# Run on the Linux CI emulator. Retain diagnostics before Gradle/emulator cleanup.
set -u
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
artifacts="$root/android-artifacts"
mkdir -p "$artifacts"
timeout 15 adb logcat -c || true
adb logcat -v threadtime TestRunner:I AndroidRuntime:E '*:S' > "$artifacts/test-events.txt" &
logger=$!
trap 'kill "$logger" 2>/dev/null || true' EXIT
cd "$root/apps/android" || exit 1
timeout --kill-after=15s 300 ./gradlew connectedDebugAndroidTest --stacktrace
result=$?
timeout 15 adb pull /sdcard/Download/MilkZipScreenshots "$artifacts/" || true
timeout 15 adb logcat -d > "$artifacts/logcat.txt" || true
if [ "$result" != 0 ]; then
  timeout 15 adb shell dumpsys window > "$artifacts/window.txt" || true
  timeout 15 adb exec-out screencap -p > "$artifacts/failure.png" || true
fi
exit "$result"
