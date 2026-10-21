#!/bin/sh
# Called while the emulator is alive; retain reports even when a test fails.
set -u
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$root/apps/android" || exit 1
./gradlew connectedDebugAndroidTest --stacktrace
result=$?
mkdir -p "$root/android-artifacts"
adb pull /sdcard/Android/data/app.localarchiver/files/screenshots "$root/android-artifacts/" || true
adb logcat -d > "$root/android-artifacts/logcat.txt" || true
exit "$result"
